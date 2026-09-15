// fastmm::cli::backtest: in-process backtest of a registered strategy (8.4). The fastmm-backtest
// app is `return fastmm::cli::backtest(argc, argv);`.
//
//   fastmm-backtest --config configs/backtest-example.toml --data synthetic
//   fastmm-backtest --config cfg.toml --data tests/fixtures/journals/sample_1000.fmj
//       then: --strategy basic_mm --param half_spread_bps=2 --out runs/bt1 --seed 7
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config / parameters (including a strategy name
// registered twice by different code), 4 unreadable data, 5 run or output failure.
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/cli/backtest.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
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

constexpr int kExitUsage = 2;
constexpr int kExitConfig = 3;
constexpr int kExitData = 4;
constexpr int kExitRun = 5;

void usage(std::FILE* out, const char* prog) {
  std::fprintf(out,
               "usage: %s --config <file.toml> [options]\n"
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
               "  --format <text|json>     output format of --list-strategies (default text)\n"
               "  --version | --help\n",
               prog);
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

}  // namespace

int backtest(int argc, char** argv, std::span<const StrategyModule> modules) {
  const std::string program = program_name(argc, argv, "fastmm-backtest");
  const char* prog = program.c_str();
  std::string config_path;
  std::string data;
  std::string strategy;
  std::string out_dir;
  std::string journal_out;
  std::string format_arg;
  std::vector<std::pair<std::string, std::string>> overrides;
  std::uint64_t seed = 0;
  std::uint64_t duration_s = 0;
  bool have_seed = false;
  bool have_out = false;
  bool list = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s: %s needs a value\n", prog, argv[i]);
        return false;
      }
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--help" || a == "-h") {
      usage(stdout, prog);
      return 0;
    } else if (a == "--version") {
      std::printf("%s %s\n", prog, build_info());
      return 0;
    } else if (a == "--config") {
      if (!value(config_path)) return kExitUsage;
    } else if (a == "--data") {
      if (!value(data)) return kExitUsage;
    } else if (a == "--strategy") {
      if (!value(strategy)) return kExitUsage;
    } else if (a == "--out") {
      if (!value(out_dir)) return kExitUsage;
      have_out = true;
    } else if (a == "--journal-out") {
      if (!value(journal_out)) return kExitUsage;
    } else if (a == "--list-strategies") {
      list = true;
    } else if (a == "--format") {
      if (!value(format_arg)) return kExitUsage;
    } else if (a == "--param") {
      if (!value(v)) return kExitUsage;
      const std::size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) {
        std::fprintf(stderr, "%s: --param expects key=value, got '%s'\n", prog, v.c_str());
        return kExitUsage;
      }
      overrides.emplace_back(v.substr(0, eq), v.substr(eq + 1));
    } else if (a == "--seed") {
      if (!value(v)) return kExitUsage;
      if (!parse_u64(v, seed)) {
        std::fprintf(stderr, "%s: --seed expects an unsigned integer\n", prog);
        return kExitUsage;
      }
      have_seed = true;
    } else if (a == "--duration") {
      if (!value(v)) return kExitUsage;
      if (!parse_u64(v, duration_s) || duration_s == 0) {
        std::fprintf(stderr, "%s: --duration expects a positive integer\n", prog);
        return kExitUsage;
      }
    } else {
      std::fprintf(stderr, "%s: unknown argument '%s'\n", prog, argv[i]);
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
        format_strategies(StrategyRegistry::instance(), TransportKind::Sim, *format);
    std::fputs(text.c_str(), stdout);
    return 0;
  }
  if (config_path.empty()) {
    std::fprintf(stderr, "%s: --config is required\n", prog);
    usage(stderr, prog);
    return kExitUsage;
  }

  bt::BacktestConfig cfg;
  Config raw;
  try {
    raw = Config::load(config_path);
    Logger::instance().set_level(raw.log_level());
    cfg = bt::BacktestConfig::from_config(raw);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: config error: %s\n", prog, e.what());
    return kExitConfig;
  }
  for (const std::string& w : cfg.warnings)
    std::fprintf(stderr, "%s: warning: %s: %s\n", prog, config_path.c_str(), w.c_str());
  if (!strategy.empty() && strategy != cfg.strategy) {
    if (!cfg.params.empty()) {
      std::fprintf(stderr,
                   "%s: note: ignoring [strategy.params] of '%s' for --strategy %s\n",
                   prog,
                   cfg.strategy.c_str(),
                   strategy.c_str());
    }
    cfg.params.clear();
    cfg.strategy = strategy;
  }
  if (cfg.strategy.empty()) {
    std::fprintf(stderr, "%s: no strategy (use --strategy or [strategy] name)\n", prog);
    return kExitConfig;
  }
  const StrategyEntry* entry = StrategyRegistry::instance().find(cfg.strategy);
  if (entry == nullptr || !entry->supports(TransportKind::Sim)) {
    std::fprintf(stderr, "%s: unknown strategy '%s'; available:\n", prog, cfg.strategy.c_str());
    for (const StrategyEntry& e : list_strategies()) {
      if (e.supports(TransportKind::Sim))
        std::fprintf(stderr, "  %.*s\n", static_cast<int>(e.name.size()), e.name.data());
    }
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
    std::fprintf(stderr, "%s: data error: %s\n", prog, e.what());
    return kExitData;
  }

  Logger::instance().start(stderr, LogLevel::Warn);
  bt::BacktestResult result;
  int rc = 0;
  try {
    result = bt::run_backtest(cfg, cfg.strategy, source.get());
  } catch (const std::invalid_argument& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    rc = kExitConfig;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: run failed: %s\n", prog, e.what());
    rc = kExitRun;
  }
  Logger::instance().stop();
  if (rc != 0) return rc;

  std::fputs(result.summary_table().c_str(), stdout);
  if (!cfg.output_dir.empty()) {
    if (!result.write_all(cfg.output_dir)) {
      std::fprintf(stderr, "%s: cannot write results to %s\n", prog, cfg.output_dir.c_str());
      return kExitRun;
    }
    std::printf("results written to %s/{equity,fills,orders}.csv and summary.json\n",
                cfg.output_dir.c_str());
  }
  if (!cfg.journal_out.empty()) std::printf("session journal: %s\n", cfg.journal_out.c_str());
  return 0;
}

}  // namespace fastmm::cli
