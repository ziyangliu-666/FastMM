// fastmm::cli::gateway: runs the venues of a configuration and lets one strategy process at a time
// trade through them (fastmm-live --gateway <socket>).
#include "command_line.hpp"

#include "fastmm/cli/gateway.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/live/gateway.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/venues/registry.hpp"

#include <cstdio>
#include <exception>
#include <optional>
#include <string>

namespace fastmm::cli {

namespace {

constexpr const char* kFooter =
    "Reads the same configuration as fastmm-live: [engine] (name, journal_dir, spin_mode,\n"
    "net_cpus, ring sizes), [venues.*] and [[instruments]]. A strategy attaches with\n"
    "fastmm-live --gateway <socket>. When its process exits or dies, the gateway cancels\n"
    "every open order on every venue and waits for the next one; the venue connections\n"
    "stay up. SIGINT/SIGTERM cancels all open orders and exits.\n"
    "\n"
    "Exit codes:\n"
    "  0  stopped by --duration or SIGINT/SIGTERM, cancel_all ok\n"
    "  2  bad command line, or a venue has no API keys\n"
    "  3  bad config, or the socket cannot be created\n"
    "  4  venue reference data failed to load\n"
    "  5  a cancel_all failed";

}  // namespace

int gateway(int argc, char** argv) {
  const std::string program = program_name(argc, argv, "fastmm-gateway");
  const char* prog = program.c_str();
  live::GatewayOptions opts;
  opts.program = program;
  std::string config_path;
  std::string log_path;
  bool allow_inline = false;

  CLI::App app("Holds the venue connections that strategy processes attach to.", program);
  setup(app);
  app.footer(kFooter);
  app.add_option("--config", config_path, "engine / venue configuration (required)")
      ->option_text("<file>");
  app.add_option("--socket",
                 opts.socket_path,
                 "attach socket (default <journal_dir>/<engine>.gw, mode 0600)")
      ->option_text("<path>");
  add_duration(
      app, "--duration", opts.duration_ns, "stop after t (e.g. 60s, 5m; default: until SIGINT)");
  app.add_flag("--dry-run", opts.dry_run, "public market data only: no API keys, no orders");
  app.add_option("--log", log_path, "write the log to a file (warnings are mirrored to stderr)")
      ->option_text("<path>");
  app.add_flag(
      "--allow-inline-secrets", allow_inline, "accept literal API secrets in the config file");
  if (const std::optional<int> rc = parse(app, argc, argv)) return *rc;
  if (config_path.empty()) return usage_error(app, "--config is required");

  Config cfg;
  try {
    Config::LoadOptions lo;
    lo.allow_inline_secrets = allow_inline;
    lo.substitute_env = false;
    cfg = Config::load(config_path, lo);
    venues::validate_venues(cfg, cfg.warnings);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return live::kExitConfig;
  }
  if (!live::resolve_venue_env(cfg, opts.dry_run, prog)) return live::kExitUsage;

  std::FILE* log_file = nullptr;
  if (!log_path.empty()) {
    log_file = std::fopen(log_path.c_str(), "a");
    if (log_file == nullptr) {
      std::fprintf(stderr, "%s: cannot open log file %s\n", prog, log_path.c_str());
      return live::kExitUsage;
    }
  }
  Logger::instance().set_level(cfg.log_level());
  Logger::instance().start(log_file != nullptr ? log_file : stderr, cfg.mirror_level());
  for (const std::string& w : cfg.warnings) FASTMM_LOG_WARN("config: {}", w);

  int rc = live::kExitRuntime;
  try {
    rc = live::run_gateway(cfg, opts);
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR("fastmm-gateway: fatal: {}", std::string_view(e.what()));
    rc = live::kExitRuntime;
  }
  Logger::instance().stop();
  if (log_file != nullptr) std::fclose(log_file);
  return rc;
}

}  // namespace fastmm::cli
