// fastmm-backtest: in-process backtest of a registered strategy (8.4).
//
//   fastmm-backtest --config configs/backtest-example.toml --data synthetic
//   fastmm-backtest --config cfg.toml --data tests/fixtures/journals/sample_1000.fmj
//       then: --strategy basic_mm --param half_spread_bps=2 --out runs/bt1 --seed 7
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config / parameters, 4 unreadable data,
// 5 run or output failure.
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/strategies/registry.hpp"
#include "fastmm/version.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kExitUsage = 2;
constexpr int kExitConfig = 3;
constexpr int kExitData = 4;
constexpr int kExitRun = 5;

void usage(std::FILE* out) {
  std::fprintf(out,
               "usage: fastmm-backtest --config <file.toml> [options]\n"
               "  --data <path|synthetic>  market data: *.fmj journal, *.csv, or the synthetic\n"
               "                           generator (default: [backtest] source/path)\n"
               "  --strategy <name>        registered strategy (default: [strategy] name)\n"
               "  --param <key=value>      strategy parameter override (repeatable)\n"
               "  --out <dir>              write equity.csv fills.csv orders.csv summary.json\n"
               "                           (default: [backtest] output_dir; '-' = don't write)\n"
               "  --seed <n>               synthetic market / latency model seed\n"
               "  --duration <seconds>     synthetic horizon\n"
               "  --journal-out <file>     record the session for fastmm-replay\n"
               "  --list-strategies        print registered strategies and their parameters\n"
               "  --version | --help\n");
}

bool parse_u64(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const std::string tmp(s);
  const unsigned long long v = std::strtoull(tmp.c_str(), &end, 10);
  if (end == tmp.c_str() || *end != '\0' || tmp[0] == '-') return false;
  out = v;
  return true;
}

void list_strategies_to(std::FILE* out) {
  for (const fastmm::StrategyEntry& e : fastmm::list_strategies()) {
    std::fprintf(out, "%.*s\n", static_cast<int>(e.name.size()), e.name.data());
    for (const fastmm::ParamDesc& d : *e.schema) {
      std::fprintf(out,
                   "  %-26s %-7s default=%-10g [%g, %g]  %s\n",
                   d.name,
                   std::string(to_string(d.type)).c_str(),
                   d.def,
                   d.min,
                   d.max,
                   d.doc);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace fastmm;
  std::string config_path;
  std::string data;
  std::string strategy;
  std::string out_dir;
  std::string journal_out;
  std::vector<std::pair<std::string, std::string>> overrides;
  std::uint64_t seed = 0;
  std::uint64_t duration_s = 0;
  bool have_seed = false;
  bool have_out = false;
  bool list = false;

  bt::register_builtin_strategies();
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "fastmm-backtest: %s needs a value\n", argv[i]);
        std::exit(kExitUsage);
      }
      dst = argv[++i];
    };
    std::string v;
    if (a == "--help" || a == "-h") {
      usage(stdout);
      return 0;
    } else if (a == "--version") {
      std::printf("fastmm-backtest %s\n", build_info());
      return 0;
    } else if (a == "--config") {
      value(config_path);
    } else if (a == "--data") {
      value(data);
    } else if (a == "--strategy") {
      value(strategy);
    } else if (a == "--out") {
      value(out_dir);
      have_out = true;
    } else if (a == "--journal-out") {
      value(journal_out);
    } else if (a == "--list-strategies") {
      list = true;
    } else if (a == "--param") {
      value(v);
      const std::size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) {
        std::fprintf(stderr, "fastmm-backtest: --param expects key=value, got '%s'\n", v.c_str());
        return kExitUsage;
      }
      overrides.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "--seed") {
      value(v);
      if (!parse_u64(v, seed)) {
        std::fprintf(stderr, "fastmm-backtest: --seed expects an unsigned integer\n");
        return kExitUsage;
      }
      have_seed = true;
    } else if (a == "--duration") {
      value(v);
      if (!parse_u64(v, duration_s) || duration_s == 0) {
        std::fprintf(stderr, "fastmm-backtest: --duration expects a positive integer\n");
        return kExitUsage;
      }
    } else {
      std::fprintf(stderr, "fastmm-backtest: unknown argument '%s'\n", argv[i]);
      usage(stderr);
      return kExitUsage;
    }
  }
  if (list) {
    list_strategies_to(stdout);
    return 0;
  }
  if (config_path.empty()) {
    std::fprintf(stderr, "fastmm-backtest: --config is required\n");
    usage(stderr);
    return kExitUsage;
  }

  bt::BacktestConfig cfg;
  Config raw;
  try {
    raw = Config::load(config_path);
    Logger::instance().set_level(raw.log_level());
    cfg = bt::BacktestConfig::from_config(raw);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-backtest: config error: %s\n", e.what());
    return kExitConfig;
  }
  if (!strategy.empty() && strategy != cfg.strategy) {
    if (!cfg.params.empty()) {
      std::fprintf(stderr,
                   "fastmm-backtest: note: ignoring [strategy.params] of '%s' for --strategy %s\n",
                   cfg.strategy.c_str(),
                   strategy.c_str());
    }
    cfg.params.clear();
    cfg.strategy = strategy;
  }
  if (cfg.strategy.empty()) {
    std::fprintf(stderr, "fastmm-backtest: no strategy (use --strategy or [strategy] name)\n");
    return kExitConfig;
  }
  if (StrategyRegistry::instance().find(cfg.strategy) == nullptr) {
    std::fprintf(
        stderr, "fastmm-backtest: unknown strategy '%s'; available:\n", cfg.strategy.c_str());
    for (const StrategyEntry& e : list_strategies())
      std::fprintf(stderr, "  %.*s\n", static_cast<int>(e.name.size()), e.name.data());
    return kExitConfig;
  }
  for (const auto& [k, v] : overrides) cfg.set_param(k, v);
  if (have_seed) cfg.set_seed(seed);
  if (duration_s > 0) cfg.duration = seconds(static_cast<std::int64_t>(duration_s));
  // The journal embeds the configuration the run used, command-line overrides included.
  raw.strategy.name = cfg.strategy;
  raw.strategy.params = cfg.params;
  if (have_seed) raw.backtest.values["seed"] = std::to_string(seed);
  if (duration_s > 0) raw.backtest.values["duration_s"] = std::to_string(duration_s);
  cfg.config_toml = raw.effective_toml();
  if (!journal_out.empty()) cfg.journal_out = journal_out;
  if (have_out) cfg.output_dir = out_dir == "-" ? std::string() : out_dir;

  std::unique_ptr<bt::MdSource> source;
  try {
    source = data.empty() ? bt::open_source(cfg) : bt::open_data(data);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-backtest: data error: %s\n", e.what());
    return kExitData;
  }

  Logger::instance().start(stderr, LogLevel::Warn);
  bt::BacktestResult result;
  int rc = 0;
  try {
    result = bt::run_backtest(cfg, cfg.strategy, source.get());
  } catch (const std::invalid_argument& e) {
    std::fprintf(stderr, "fastmm-backtest: %s\n", e.what());
    rc = kExitConfig;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastmm-backtest: run failed: %s\n", e.what());
    rc = kExitRun;
  }
  Logger::instance().stop();
  if (rc != 0) return rc;

  std::fputs(result.summary_table().c_str(), stdout);
  if (!cfg.output_dir.empty()) {
    if (!result.write_all(cfg.output_dir)) {
      std::fprintf(stderr, "fastmm-backtest: cannot write results to %s\n", cfg.output_dir.c_str());
      return kExitRun;
    }
    std::printf("results written to %s/{equity,fills,orders}.csv and summary.json\n",
                cfg.output_dir.c_str());
  }
  if (!cfg.journal_out.empty()) std::printf("session journal: %s\n", cfg.journal_out.c_str());
  return 0;
}
