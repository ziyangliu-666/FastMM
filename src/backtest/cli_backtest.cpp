// fastmm::cli::backtest: in-process backtest of a registered strategy (8.4). The fastmm-backtest
// app is `return fastmm::cli::backtest(argc, argv);`.
//
//   fastmm-backtest --config configs/backtest-example.toml --data synthetic
//   fastmm-backtest --config cfg.toml --data tests/fixtures/journals/sample_1000.fmj
//   fastmm-backtest --config configs/backtest-binance.toml --data binance:BTCUSDT,2024-03-27
//       then: --strategy basic_mm --param half_spread_bps=2 --out runs/bt1 --seed 7
//
// Exit codes: 0 ok, 2 bad command line, 3 bad config / parameters (including a strategy name
// registered twice by different code), 4 unreadable data, 5 run or output failure.
#include "command_line.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/cli/backtest.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/strategies/listing.hpp"
#include "fastmm/strategies/registry.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace fastmm::cli {

namespace {

constexpr int kExitConfig = 3;
constexpr int kExitData = 4;
constexpr int kExitRun = 5;

}  // namespace

int backtest(int argc, char** argv, std::span<const StrategyModule> modules) {
  const std::string program = program_name(argc, argv, "fastmm-backtest");
  const char* prog = program.c_str();
  std::string config_path;
  std::string data;
  std::string strategy;
  std::string out_dir;
  std::string journal_out;
  std::string format_arg = "text";
  std::vector<std::string> params;
  std::uint64_t seed = 0;
  std::uint64_t duration_s = 0;
  bool list = false;

  CLI::App app("Backtests a registered strategy.", program);
  setup(app);
  app.add_option("--config", config_path, "engine / strategy / backtest configuration (required)")
      ->option_text("<file.toml>");
  app.add_option("--data",
                 data,
                 "market data: 'synthetic', a *.fmj / *.csv path, or <source>:<args> (default: "
                 "[backtest] source/path). `fastmm-data list` prints the sources")
      ->option_text("<spec>");
  app.add_option("--strategy", strategy, "registered strategy (default: [strategy] name)")
      ->option_text("<name>");
  app.add_option("--param", params, "strategy parameter override (repeatable)")
      ->option_text("<key=value>")
      ->allow_extra_args(false)
      ->check(key_value());
  const CLI::Option* out_opt =
      app.add_option("--out",
                     out_dir,
                     "write equity.csv fills.csv orders.csv summary.json (default: [backtest] "
                     "output_dir; '-' = don't write)")
          ->option_text("<dir>");
  const CLI::Option* seed_opt =
      app.add_option("--seed", seed, "synthetic market / latency model seed")->option_text("<n>");
  app.add_option("--duration", duration_s, "synthetic horizon")
      ->option_text("<seconds>")
      ->check(CLI::Range(std::uint64_t{1}, std::uint64_t{10'000'000}));
  app.add_option("--journal-out", journal_out, "record the session for fastmm-replay")
      ->option_text("<file>");
  CLI::Option* list_opt =
      app.add_flag("--list-strategies", list, "print registered strategies and their parameters");
  app.add_option("--format", format_arg, "output format of --list-strategies (default text)")
      ->option_text("<text|json>")
      ->check(CLI::IsMember({"text", "json"}))
      ->needs(list_opt);
  if (const std::optional<int> rc = parse(app, argc, argv)) return *rc;
  const bool have_seed = seed_opt->count() > 0;

  if (!register_strategy_modules(program, modules)) return kExitConfig;
  if (list) {
    const std::string text = format_strategies(
        StrategyRegistry::instance(), TransportKind::Sim, *parse_list_format(format_arg));
    std::fputs(text.c_str(), stdout);
    return 0;
  }
  if (config_path.empty()) return usage_error(app, "--config is required");

  bt::BacktestConfig cfg;
  Config raw;
  try {
    ConfigLoadOptions lo;
    lo.substitute_env = false;  // a backtest connects to nothing: a live config's ${API_KEY}s stay
    raw = Config::load(config_path, lo);
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
  for (const std::string& p : params) {
    const std::size_t eq = p.find('=');
    cfg.set_param(p.substr(0, eq), p.substr(eq + 1));
  }
  if (have_seed) cfg.set_seed(seed);
  if (duration_s > 0) cfg.duration = seconds(static_cast<std::int64_t>(duration_s));
  // The journal embeds the configuration the run used, command-line overrides included.
  raw.strategy.name = cfg.strategy;
  raw.strategy.params = cfg.params;
  if (have_seed) raw.backtest.values["seed"] = std::to_string(seed);
  if (duration_s > 0) raw.backtest.values["duration_s"] = std::to_string(duration_s);
  cfg.config_toml = raw.effective_toml();
  if (!journal_out.empty()) cfg.journal_out = journal_out;
  if (out_opt->count() > 0) cfg.output_dir = out_dir == "-" ? std::string() : out_dir;

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
