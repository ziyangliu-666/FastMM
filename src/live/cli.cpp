// fastmm::cli::live: run a strategy against live venues (Binance Spot / Bybit v5 testnets, or the
// local Binance-compatible simulator). The fastmm-live app is `return fastmm::cli::live(argc,
// argv);`.
//
//   fastmm-live --config configs/binance-testnet.toml --dry-run --duration 60s
//   FASTMM_BINANCE_API_KEY=... FASTMM_BINANCE_API_SECRET=... fastmm-live --config
//   configs/binance-testnet.toml
//
// Exit codes (live/session.hpp; --help lists them): 0 ok, 2 bad command line or missing API keys,
// 3 bad config / strategy / parameters (including a strategy name registered twice by different
// code), 4 venue reference data failed, 5 runtime failure (cancel-all failed, journal, ring
// overflow), 6 the engine tripped the kill switch itself and [engine] on_kill = "exit", 7 the slow
// tier of a Python strategy failed (python -m fastmm run; fastmm-live never returns it).
#include "command_line.hpp"

#include "fastmm/cli/live.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/strategies/listing.hpp"
#include "fastmm/strategies/registry.hpp"
#include "fastmm/venues/registry.hpp"

#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::cli {

namespace {

using live::kExitConfig;
using live::kExitOk;
using live::kExitRuntime;
using live::kExitUsage;

constexpr const char* kFooter =
    "API keys come from the environment through ${VAR} references in [venues.*],\n"
    "e.g. FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET.\n"
    "SIGINT/SIGTERM trips the kill switch, cancels all open orders and exits. A second\n"
    "SIGINT/SIGTERM 5 s or more after the first, or a shutdown still running 60 s after\n"
    "the stop, exits at once with code 5 without waiting for cancel_all.\n"
    "SIGHUP clears the kill switch and resumes quoting (on_kill = \"stay\").\n"
    "fastmm-ctl talks to the control socket: pull, resume, param, limits, flatten,\n"
    "kill, unkill, stop and status.\n"
    "A kill switch the engine trips itself ([risk] max_loss, a full ring, every venue\n"
    "killed) does the same and exits with code 6, unless [engine] on_kill = \"stay\".\n"
    "A max_loss trip is latched in [engine] kill_file: the next start refuses to trade\n"
    "(exit code 6) until --clear-kill or the file is removed.\n"
    "\n"
    "Exit codes:\n"
    "  0  stopped by --duration or SIGINT/SIGTERM, cancel_all ok\n"
    "  2  bad command line, or a venue has no API keys\n"
    "  3  bad config, strategy or parameters\n"
    "  4  venue reference data failed to load\n"
    "  5  runtime failure: cancel_all failed, journal, ring overflow, forced exit, uncaught error\n"
    "  6  kill switch tripped by the engine (on_kill = \"exit\"), or a latched max_loss trip\n"
    "  7  a Python strategy's slow tier failed (python -m fastmm run), cancel_all ok";

}  // namespace

int live(int argc, char** argv, std::span<const StrategyModule> modules) {
  const std::string program = program_name(argc, argv, "fastmm-live");
  const char* prog = program.c_str();
  live::LiveOptions opts;
  opts.program = program;
  std::string log_path;
  std::string strategy;
  std::string format_arg = "text";
  std::vector<std::string> params;
  bool allow_inline = false;
  bool list = false;

  CLI::App app("Trades a strategy on the venues of a configuration.", program);
  setup(app);
  app.footer(kFooter);
  app.add_option("--config", opts.config_path, "engine / venue / strategy configuration (required)")
      ->option_text("<file>");
  app.add_option("--strategy",
                 strategy,
                 "registered strategy (default: [strategy] name); a different strategy ignores "
                 "[strategy.params]")
      ->option_text("<name>");
  app.add_option("--param", params, "strategy parameter override (repeatable)")
      ->option_text("<key=value>")
      ->allow_extra_args(false)
      ->check(key_value());
  add_duration(app,
               "--duration",
               opts.duration_ns,
               "stop after t (e.g. 60s, 5m, 1500ms; default: until SIGINT)");
  app.add_flag("--dry-run", opts.dry_run, "public market data only: no API keys, no orders");
  app.add_option("--record-raw",
                 opts.record_raw_dir,
                 "append raw WebSocket frames to <dir>/<venue>-<channel>.jsonl")
      ->option_text("<dir>");
  app.add_option("--journal", opts.journal_path, "write the session journal (.fmj) here")
      ->option_text("<path>");
  app.add_flag(
      "--no-journal", opts.no_journal, "disable journaling even if [engine] journal = true");
  app.add_option("--status",
                 opts.status_path,
                 "live status file for fastmm-top (default /dev/shm/fastmm-<engine>.status)")
      ->option_text("<path>");
  app.add_flag("--no-status", opts.no_status, "do not publish live status");
  app.add_option("--control",
                 opts.control_path,
                 "control socket for fastmm-ctl (default <journal_dir>/<engine>.ctl, mode 0600)")
      ->option_text("<path>");
  app.add_flag("--no-control", opts.no_control, "do not open a control socket");
  app.add_option("--gateway",
                 opts.gateway_path,
                 "trade through the fastmm-gateway at this socket instead of connecting to the "
                 "venues (no API keys here)")
      ->option_text("<path>");
  app.add_flag("--clear-kill",
               opts.clear_kill,
               "clear a latched kill switch and the cumulative PnL before starting; arms the "
               "whole [risk] max_loss budget again");
  app.add_option("--log", log_path, "write the log to a file (warnings are mirrored to stderr)")
      ->option_text("<path>");
  app.add_flag(
      "--allow-inline-secrets", allow_inline, "accept literal API secrets in the config file");
  CLI::Option* list_opt =
      app.add_flag("--list-strategies", list, "print the strategies this binary can run and exit");
  app.add_option("--format", format_arg, "output format of --list-strategies (default text)")
      ->option_text("<text|json>")
      ->check(CLI::IsMember({"text", "json"}))
      ->needs(list_opt);
  if (const std::optional<int> rc = parse(app, argc, argv)) return *rc;

  if (!register_strategy_modules(program, modules)) return kExitConfig;
  if (list) {
    const std::string text = format_strategies(
        StrategyRegistry::instance(), TransportKind::Live, *parse_list_format(format_arg));
    std::fputs(text.c_str(), stdout);
    return kExitOk;
  }
  if (opts.config_path.empty()) return usage_error(app, "--config is required");

  Config cfg;
  try {
    Config::LoadOptions lo;
    lo.allow_inline_secrets = allow_inline;
    lo.substitute_env = false;  // resolved below so --dry-run works without keys
    cfg = Config::load(opts.config_path, lo);
    // Each [venues.<name>] section is checked by the connector its `kind` names: the central
    // schema knows only the generic keys (venues/registry.hpp).
    venues::validate_venues(cfg, cfg.warnings);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitConfig;
  }
  // Command-line overrides change the configuration itself, so the journal embeds (and hashes) the
  // strategy and parameters the session really ran with.
  if (!strategy.empty() && strategy != cfg.strategy.name) {
    if (!cfg.strategy.params.empty()) {
      std::fprintf(stderr,
                   "%s: note: ignoring [strategy.params] of '%s' for --strategy %s\n",
                   prog,
                   cfg.strategy.name.c_str(),
                   strategy.c_str());
    }
    cfg.strategy.params.clear();
    cfg.strategy.name = strategy;
  }
  for (const std::string& p : params) {
    const std::size_t eq = p.find('=');
    cfg.strategy.params[p.substr(0, eq)] = p.substr(eq + 1);
  }
  // Attached to a gateway, this process holds no venue connection and needs no keys.
  if (opts.gateway_path.empty() && !live::resolve_venue_env(cfg, opts.dry_run, prog))
    return kExitUsage;

  std::FILE* log_file = nullptr;
  if (!log_path.empty()) {
    log_file = std::fopen(log_path.c_str(), "a");
    if (log_file == nullptr) {
      std::fprintf(stderr, "%s: cannot open log file %s\n", prog, log_path.c_str());
      return kExitUsage;
    }
  }
  Logger::instance().set_level(cfg.log_level());
  Logger::instance().start(log_file != nullptr ? log_file : stderr, cfg.mirror_level());
  for (const std::string& w : cfg.warnings) FASTMM_LOG_WARN("config: {}", w);

  int rc = kExitRuntime;
  try {
    rc = live::run_live(cfg, opts);
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR("fastmm-live: fatal: {}", std::string_view(e.what()));
    rc = kExitRuntime;
  }
  Logger::instance().stop();
  if (log_file != nullptr) std::fclose(log_file);
  return rc;
}

}  // namespace fastmm::cli
