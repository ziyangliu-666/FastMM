// fastmm-live: run a strategy against live venues (Binance Spot / Bybit v5 testnets, or the
// local Binance-compatible simulator).
//
//   fastmm-live --config configs/binance-testnet.toml --dry-run --duration 60s
//   FASTMM_BINANCE_API_KEY=... FASTMM_BINANCE_API_SECRET=... fastmm-live --config
//   configs/binance-testnet.toml
//
// Exit codes: 0 ok, 2 bad command line or missing API keys, 3 bad config / strategy, 4 venue
// reference data failed, 5 runtime failure (cancel-all failed, journal, ring overflow).
#include "live_backend.hpp"
#include "live_runners.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/config/env_subst.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/version.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>

namespace {

using namespace fastmm;
using namespace fastmm::live;

void usage(std::FILE* out) {
  std::fprintf(
      out,
      "usage: fastmm-live --config <file.toml> [options]\n"
      "  --config <file>          engine / venue / strategy configuration (required)\n"
      "  --duration <t>           stop after t (e.g. 60s, 5m, 1500ms; default: until SIGINT)\n"
      "  --dry-run                public market data only: no API keys, no orders\n"
      "  --record-raw <dir>       append raw WebSocket frames to <dir>/<venue>-<channel>.jsonl\n"
      "  --journal <path>         write the session journal (.fmj) here\n"
      "  --no-journal             disable journaling even if [engine] journal = true\n"
      "  --log <path>             write the log to a file (warnings are mirrored to stderr)\n"
      "  --allow-inline-secrets   accept literal API secrets in the config file\n"
      "  --list-strategies        print the strategies this binary can run and exit\n"
      "  --version | --help\n"
      "\n"
      "API keys come from the environment through ${VAR} references in [venues.*],\n"
      "e.g. FASTMM_BINANCE_API_KEY / FASTMM_BINANCE_API_SECRET.\n"
      "SIGINT/SIGTERM trips the kill switch, cancels all open orders and exits.\n");
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
bool resolve_venue_env(Config& cfg, bool dry_run) {
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
            "fastmm-live: venue '%s' needs API keys: environment variable %s is not set "
            "(venues.%s.%s). Export it, or run with --dry-run for public market data only.\n",
            v.name.c_str(),
            r.error().c_str(),
            v.name.c_str(),
            key);
      } else {
        std::fprintf(stderr,
                     "fastmm-live: venues.%s.%s: environment variable %s is not set\n",
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
      std::fprintf(
          stderr,
          "fastmm-live: venue '%s' has no api_key/api_secret. Set them via ${ENV} references, or "
          "run with --dry-run for public market data only.\n",
          v.name.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  LiveOptions opts;
  std::string log_path;
  bool allow_inline = false;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "fastmm-live: %s needs a value\n", argv[i]);
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return kExitOk;
    }
    if (a == "--version") {
      std::printf("fastmm-live %s\n", fastmm::build_info());
      return kExitOk;
    }
    if (a == "--config") {
      if (!value(opts.config_path)) return kExitUsage;
    } else if (a == "--duration") {
      std::string d;
      if (!value(d)) return kExitUsage;
      if (!parse_duration(d, opts.duration_ns) || opts.duration_ns <= 0) {
        std::fprintf(
            stderr, "fastmm-live: bad --duration '%s' (examples: 60s, 5m, 1500ms)\n", d.c_str());
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
    } else if (a == "--log") {
      if (!value(log_path)) return kExitUsage;
    } else if (a == "--allow-inline-secrets") {
      allow_inline = true;
    } else if (a == "--list-strategies") {
      list = true;
    } else {
      std::fprintf(stderr, "fastmm-live: unknown argument '%s'\n\n", argv[i]);
      usage(stderr);
      return kExitUsage;
    }
  }
  register_live_strategies();
  if (list) {
    for (const StrategyEntry& e : list_strategies()) {
      if (!e.supports(TransportKind::Live)) continue;
      std::printf("%.*s\n", static_cast<int>(e.name.size()), e.name.data());
      for (const ParamDesc& d : *e.schema) {
        std::printf("  %-26s %-6s default=%-10g [%g, %g]  %s\n",
                    d.name,
                    std::string(to_string(d.type)).c_str(),
                    d.def,
                    d.min,
                    d.max,
                    d.doc);
      }
    }
    return kExitOk;
  }
  if (opts.config_path.empty()) {
    std::fprintf(stderr, "fastmm-live: --config is required\n\n");
    usage(stderr);
    return kExitUsage;
  }

  Config cfg;
  try {
    Config::LoadOptions lo;
    lo.allow_inline_secrets = allow_inline;
    lo.substitute_env = false;  // resolved below so --dry-run works without keys
    cfg = Config::load(opts.config_path, lo);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-live: %s\n", e.what());
    return kExitConfig;
  }
  if (!resolve_venue_env(cfg, opts.dry_run)) return kExitUsage;

  std::FILE* log_file = nullptr;
  if (!log_path.empty()) {
    log_file = std::fopen(log_path.c_str(), "a");
    if (log_file == nullptr) {
      std::fprintf(stderr, "fastmm-live: cannot open log file %s\n", log_path.c_str());
      return kExitUsage;
    }
  }
  Logger::instance().set_level(cfg.log_level());
  Logger::instance().start(log_file != nullptr ? log_file : stderr, cfg.mirror_level());
  for (const std::string& w : cfg.warnings) FASTMM_LOG_WARN("config: {}", w);

  int rc = kExitRuntime;
  try {
    rc = run_live(cfg, opts);
  } catch (const std::exception& e) {
    FASTMM_LOG_ERROR("fastmm-live: fatal: {}", std::string_view(e.what()));
    rc = kExitRuntime;
  }
  Logger::instance().stop();
  if (log_file != nullptr) std::fclose(log_file);
  return rc;
}
