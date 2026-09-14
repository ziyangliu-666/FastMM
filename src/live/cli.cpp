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
// overflow), 6 the engine tripped the kill switch itself and [engine] on_kill = "exit".
#include "fastmm/cli/live.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/config/env_subst.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/live/session.hpp"
#include "fastmm/strategies/listing.hpp"
#include "fastmm/strategies/registry.hpp"
#include "fastmm/version.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::cli {

namespace {

using live::kExitConfig;
using live::kExitOk;
using live::kExitRuntime;
using live::kExitUsage;

void usage(std::FILE* out, const char* prog) {
  std::fprintf(
      out,
      "usage: %s --config <file.toml> [options]\n"
      "  --config <file>          engine / venue / strategy configuration (required)\n"
      "  --strategy <name>        registered strategy (default: [strategy] name); a different\n"
      "                           strategy ignores [strategy.params]\n"
      "  --param <key=value>      strategy parameter override (repeatable)\n"
      "  --duration <t>           stop after t (e.g. 60s, 5m, 1500ms; default: until SIGINT)\n"
      "  --dry-run                public market data only: no API keys, no orders\n"
      "  --record-raw <dir>       append raw WebSocket frames to <dir>/<venue>-<channel>.jsonl\n"
      "  --journal <path>         write the session journal (.fmj) here\n"
      "  --no-journal             disable journaling even if [engine] journal = true\n"
      "  --status <path>          live status file for fastmm-top (default "
      "/dev/shm/fastmm-<engine>.status)\n"
      "  --no-status              do not publish live status\n"
      "  --log <path>             write the log to a file (warnings are mirrored to stderr)\n"
      "  --allow-inline-secrets   accept literal API secrets in the config file\n"
      "  --list-strategies        print the strategies this binary can run and exit\n"
      "  --format <text|json>     output format of --list-strategies (default text)\n"
      "  --version | --help\n"
      "\n"
      "API keys come from the environment through ${VAR} references in [venues.*],\n"
      "e.g. FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET.\n"
      "SIGINT/SIGTERM trips the kill switch, cancels all open orders and exits.\n"
      "A kill switch the engine trips itself ([risk] max_loss, a full ring, every venue\n"
      "killed) does the same and exits with code 6, unless [engine] on_kill = \"stay\".\n"
      "\n"
      "Exit codes:\n"
      "  0  stopped by --duration or SIGINT/SIGTERM, cancel_all ok\n"
      "  2  bad command line, or a venue has no API keys\n"
      "  3  bad config, strategy or parameters\n"
      "  4  venue reference data failed to load\n"
      "  5  runtime failure: cancel_all failed, journal, ring overflow, uncaught error\n"
      "  6  kill switch tripped by the engine (on_kill = \"exit\"), cancel_all ok\n",
      prog);
}

// "60s", "5m", "1500ms", "2h", or a bare number of seconds.
bool parse_duration(std::string_view s, std::int64_t& ns) {
  if (s.empty()) return false;
  std::size_t i = 0;
  std::int64_t v = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (s[i] - '0');
    if (v > 10'000'000) return false;
    ++i;
  }
  if (i == 0) return false;
  const std::string_view unit = s.substr(i);
  std::int64_t mult = 1'000'000'000;
  if (unit == "ms") {
    mult = 1'000'000;
  } else if (unit == "s" || unit.empty()) {
    mult = 1'000'000'000;
  } else if (unit == "m") {
    mult = 60'000'000'000;
  } else if (unit == "h") {
    mult = 3'600'000'000'000;
  } else {
    return false;
  }
  ns = v * mult;
  return true;
}

// Resolves ${VAR} in every venue string. For dry-run, missing variables in api_key /
// api_secret are dropped (no keys needed); anything else missing is an error.
bool resolve_venue_env(Config& cfg, bool dry_run, const char* prog) {
  for (VenueSection& v : cfg.venues) {
    auto resolve = [&](const char* key, std::string& value, bool secret) {
      if (!has_env_reference(value)) return true;
      auto r = substitute_env(value);
      if (r) {
        value = *r;
        return true;
      }
      if (secret && dry_run) {
        value.clear();
        return true;
      }
      if (secret) {
        std::fprintf(
            stderr,
            "%s: venue '%s' needs API keys: environment variable %s is not set "
            "(venues.%s.%s). Export it, or run with --dry-run for public market data only.\n",
            prog,
            v.name.c_str(),
            r.error().c_str(),
            v.name.c_str(),
            key);
      } else {
        std::fprintf(stderr,
                     "%s: venues.%s.%s: environment variable %s is not set\n",
                     prog,
                     v.name.c_str(),
                     key,
                     r.error().c_str());
      }
      return false;
    };
    if (!resolve("ws_url", v.ws_url, false) || !resolve("ws_api_url", v.ws_api_url, false) ||
        !resolve("rest_url", v.rest_url, false) || !resolve("ca_file", v.ca_file, false) ||
        !resolve("api_key", v.api_key, true) || !resolve("api_secret", v.api_secret, true))
      return false;
    for (auto& [k, val] : v.extra) {
      if (!resolve(k.c_str(), val, false)) return false;
    }
    if (dry_run) {
      v.api_key.clear();
      v.api_secret.clear();
    } else if (v.api_key.empty() || v.api_secret.empty()) {
      std::fprintf(stderr,
                   "%s: venue '%s' has no api_key/api_secret. Set them via ${ENV} references, or "
                   "run with --dry-run for public market data only.\n",
                   prog,
                   v.name.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int live(int argc, char** argv, std::span<const StrategyModule> modules) {
  const std::string program = program_name(argc, argv, "fastmm-live");
  const char* prog = program.c_str();
  live::LiveOptions opts;
  opts.program = program;
  std::string log_path;
  std::string strategy;
  std::string format_arg;
  std::vector<std::pair<std::string, std::string>> overrides;
  bool allow_inline = false;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s: %s needs a value\n", prog, argv[i]);
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (a == "--help" || a == "-h") {
      usage(stdout, prog);
      return kExitOk;
    }
    if (a == "--version") {
      std::printf("%s %s\n", prog, fastmm::build_info());
      return kExitOk;
    }
    if (a == "--config") {
      if (!value(opts.config_path)) return kExitUsage;
    } else if (a == "--strategy") {
      if (!value(strategy)) return kExitUsage;
    } else if (a == "--param") {
      std::string v;
      if (!value(v)) return kExitUsage;
      const std::size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) {
        std::fprintf(stderr, "%s: --param expects key=value, got '%s'\n", prog, v.c_str());
        return kExitUsage;
      }
      overrides.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "--duration") {
      std::string d;
      if (!value(d)) return kExitUsage;
      if (!parse_duration(d, opts.duration_ns) || opts.duration_ns <= 0) {
        std::fprintf(
            stderr, "%s: bad --duration '%s' (examples: 60s, 5m, 1500ms)\n", prog, d.c_str());
        return kExitUsage;
      }
    } else if (a == "--dry-run") {
      opts.dry_run = true;
    } else if (a == "--record-raw") {
      if (!value(opts.record_raw_dir)) return kExitUsage;
    } else if (a == "--journal") {
      if (!value(opts.journal_path)) return kExitUsage;
    } else if (a == "--no-journal") {
      opts.no_journal = true;
    } else if (a == "--status") {
      if (!value(opts.status_path)) return kExitUsage;
    } else if (a == "--no-status") {
      opts.no_status = true;
    } else if (a == "--log") {
      if (!value(log_path)) return kExitUsage;
    } else if (a == "--allow-inline-secrets") {
      allow_inline = true;
    } else if (a == "--list-strategies") {
      list = true;
    } else if (a == "--format") {
      if (!value(format_arg)) return kExitUsage;
    } else {
      std::fprintf(stderr, "%s: unknown argument '%s'\n\n", prog, argv[i]);
      usage(stderr, prog);
      return kExitUsage;
    }
  }
  std::optional<ListFormat> format = ListFormat::Text;
  if (!format_arg.empty()) {
    format = parse_list_format(format_arg);
    if (!format) {
      std::fprintf(stderr, "%s: bad --format '%s' (text or json)\n", prog, format_arg.c_str());
      return kExitUsage;
    }
    if (!list) {
      std::fprintf(stderr, "%s: --format applies to --list-strategies\n", prog);
      return kExitUsage;
    }
  }
  if (!register_strategy_modules(program, modules)) return kExitConfig;
  if (list) {
    const std::string text =
        format_strategies(StrategyRegistry::instance(), TransportKind::Live, *format);
    std::fputs(text.c_str(), stdout);
    return kExitOk;
  }
  if (opts.config_path.empty()) {
    std::fprintf(stderr, "%s: --config is required\n\n", prog);
    usage(stderr, prog);
    return kExitUsage;
  }

  Config cfg;
  try {
    Config::LoadOptions lo;
    lo.allow_inline_secrets = allow_inline;
    lo.substitute_env = false;  // resolved below so --dry-run works without keys
    cfg = Config::load(opts.config_path, lo);
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
  for (const auto& [k, v] : overrides) cfg.strategy.params[k] = v;
  if (!resolve_venue_env(cfg, opts.dry_run, prog)) return kExitUsage;

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
