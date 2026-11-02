// fastmm::cli::shadow: fastmm-shadow's command line. An application with strategies of its own
// calls it with their modules: `return fastmm::cli::shadow(argc, argv, {my_module});`.
//
//   fastmm-shadow --config live.toml                 the configuration's sessions, one after
//   another fastmm-shadow --config live.toml --session x.fmj one session, then exit
//
// Exit codes: 0 done, 2 usage, 3 unreadable configuration or journal, unknown strategy.
#include "command_line.hpp"

#include "fastmm/backtest/shadow.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/cli/shadow.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/shadow_status.hpp"
#include "fastmm/core/thread_utils.hpp"

#include <fmt/format.h>

#include <fcntl.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fastmm::cli {

namespace {

constexpr int kExitInput = 3;

std::atomic<bool> g_stop{false};
void on_signal(int /*sig*/) {
  g_stop.store(true);
}

// Off the session's cores: the ones [shadow] cpus lists, else every one but the engine's and the
// network threads'.
void leave_hot_cores(const Config& cfg, const char* prog) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (!cfg.shadow.cpus.empty()) {
    for (const int c : cfg.shadow.cpus) {
      if (c >= 0 && c < CPU_SETSIZE) CPU_SET(static_cast<std::size_t>(c), &set);
    }
  } else {
    if (sched_getaffinity(0, sizeof set, &set) != 0) return;
    if (cfg.engine.cpu >= 0) CPU_CLR(static_cast<std::size_t>(cfg.engine.cpu), &set);
    for (const int c : cfg.engine.net_cpus) {
      if (c >= 0 && c < CPU_SETSIZE) CPU_CLR(static_cast<std::size_t>(c), &set);
    }
    if (CPU_COUNT(&set) == 0) return;  // nothing left: leave it to the nice value
  }
  if (sched_setaffinity(0, sizeof set, &set) != 0)
    std::fprintf(stderr, "%s: cannot set the CPU affinity\n", prog);
}

}  // namespace

int shadow(int argc, char** argv, std::span<const StrategyModule> modules) {
  const std::string program = program_name(argc, argv, "fastmm-shadow");
  const char* prog = program.c_str();
  std::string config_path;
  std::string session;
  int threads = 0;
  int idle_exit_s = 0;
  bool verbose = false;
  CLI::App app(
      "Runs variants of a live session's strategy over its journal as it is written, beside the "
      "session's real fills.",
      program);
  setup(app);
  app.add_option("--config",
                 config_path,
                 "the session's configuration; its [shadow] table says what to run")
      ->option_text("<file.toml>");
  app.add_option("--session",
                 session,
                 "follow this journal (its first part) to its end and exit; without it, the "
                 "sessions of [engine] journal_dir and name, the newest and each one after it")
      ->option_text("<file.fmj>");
  app.add_option("--threads", threads, "threads that step the points ([shadow] threads)")
      ->option_text("<n>");
  app.add_option("--idle-exit",
                 idle_exit_s,
                 "leave when no session has come for this long after the last ended, default never")
      ->option_text("<s>");
  app.add_flag("--verbose", verbose, "log what the runs do, as a backtest does");
  if (const std::optional<int> rc = parse(app, argc, argv)) return *rc;
  if (config_path.empty()) return usage_error(app, "--config is required");
  if (!register_strategy_modules(program, modules)) return kExitInput;
  Config::LoadOptions lo;
  lo.substitute_env = false;
  lo.allow_inline_secrets = true;  // nothing here connects to anything
  std::optional<Config> cfg;
  try {
    cfg = Config::load(config_path, lo);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitInput;
  }
  // One shadow per session name: fastmm-live starts one at each restart, and the running one goes
  // on.
  const std::filesystem::path lock_dir(cfg->engine.journal_dir);
  std::error_code ec;
  std::filesystem::create_directories(lock_dir, ec);
  const std::string lock_path = cfg->engine.journal_dir + "/" + cfg->engine.name + ".shadow.lock";
  const int lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    std::printf("%s: another shadow holds %s\n", prog, lock_path.c_str());
    return 0;
  }
  // A point's risk rejects and the like are its run's own: every point would log them, for weeks.
  Logger::instance().start(stderr, verbose ? LogLevel::Info : LogLevel::Error);
  Logger::instance().set_level(verbose ? LogLevel::Info : LogLevel::Error);
  leave_hot_cores(*cfg, prog);
  if (::setpriority(PRIO_PROCESS, 0, cfg->shadow.nice) != 0)
    std::fprintf(stderr, "%s: cannot set the nice value\n", prog);
  bt::ShadowOptions opt = bt::shadow_options(cfg->shadow);
  if (threads > 0) opt.threads = threads;
  if (opt.report_path.empty())
    opt.report_path = cfg->engine.journal_dir + "/" + cfg->engine.name + ".shadow.json";
  if (opt.status_path.empty()) opt.status_path = default_shadow_status_path(cfg->engine.name);
  opt.stop = &g_stop;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  int rc = 0;
  try {
    bt::Shadow shadow(opt);
    std::printf("%s: %zu variants beside the live point; report %s, status %s\n",
                prog,
                opt.variants.size(),
                opt.report_path.c_str(),
                opt.status_path.c_str());
    std::fflush(stdout);
    if (session.empty()) {
      shadow.follow(cfg->engine.journal_dir, cfg->engine.name, seconds(idle_exit_s));
    } else {
      const bt::ShadowSessionResult r = shadow.session(session);
      std::printf("%s: %s: %llu events in %llu parts, %llu records late, %zu windows\n",
                  prog,
                  session.c_str(),
                  static_cast<unsigned long long>(r.md_events),
                  static_cast<unsigned long long>(r.parts),
                  static_cast<unsigned long long>(r.late),
                  shadow.windows());
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    rc = kExitInput;
  }
  Logger::instance().stop();
  return rc;
}

}  // namespace fastmm::cli
