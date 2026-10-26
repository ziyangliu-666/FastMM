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

#include "fastmm/backtest/ab_report.hpp"
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/multiverse.hpp"
#include "fastmm/backtest/sweep.hpp"
#include "fastmm/cli/backtest.hpp"
#include "fastmm/cli/modules.hpp"
#include "fastmm/config/config.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/strategies/listing.hpp"
#include "fastmm/strategies/registry.hpp"

#include <algorithm>
#include <chrono>
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

// [ab] assignment = "branch": the configuration once per arm over the same data and seed, each
// run's results in <out>/<arm>/ and its journal there (or next to --journal-out, x.arm-<name>.fmj),
// then the readout of fastmm-data ab over the arms' journals.
int run_branches(const char* prog, const bt::BacktestConfig& cfg, bt::MdSource* source) {
  if (cfg.output_dir.empty() && cfg.journal_out.empty()) {
    std::fprintf(stderr,
                 "%s: [ab] assignment = \"branch\" writes a journal per arm: give --out or "
                 "--journal-out\n",
                 prog);
    return kExitConfig;
  }
  // Every arm restores the same strategy state and none writes its end state back: the arms stay
  // paired.
  bt::BacktestConfig start;
  try {
    start = bt::shared_start(cfg);
  } catch (const std::invalid_argument& e) {
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitConfig;
  }
  std::vector<bt::BacktestConfig> cfgs;
  for (std::size_t k = 0; k < cfg.ab.arms.size(); ++k) {
    const std::string& arm = cfg.ab.arms[k].name;
    bt::BacktestConfig c = start;
    c.ab_arm = static_cast<int>(k);
    if (!cfg.output_dir.empty()) c.output_dir = std::string(cfg.output_dir).append("/").append(arm);
    if (!cfg.journal_out.empty()) {
      std::string base = cfg.journal_out;
      if (base.size() > 4 && base.ends_with(".fmj")) base.resize(base.size() - 4);
      c.journal_out = base.append(".arm-").append(arm).append(".fmj");
    } else {
      c.journal_out = c.output_dir + "/session.fmj";
    }
    cfgs.push_back(std::move(c));
  }
  // Recorded data under l2_queue: the arms share one pass over it (bt::run_multiverse), with the
  // results each would have alone.
  const bool together = source != nullptr && cfg.transport.fill_model == sim::FillModel::L2Queue;
  std::vector<std::string> journals;
  Logger::instance().start(stderr, LogLevel::Warn);
  std::vector<bt::BacktestResult> results;
  for (std::size_t k = 0; k < cfgs.size(); ++k) {
    const bt::BacktestConfig& c = cfgs[k];
    const std::string& arm = cfg.ab.arms[k].name;
    try {
      if (together && k == 0) {
        source->reset();
        results = bt::run_multiverse(cfgs, cfg.strategy, *source);
      } else if (!together) {
        if (source != nullptr) source->reset();
        results.push_back(bt::run_backtest(c, c.strategy, source));
      }
    } catch (const std::invalid_argument& e) {
      Logger::instance().stop();
      std::fprintf(stderr, "%s: %s\n", prog, e.what());
      return kExitConfig;
    } catch (const std::exception& e) {
      Logger::instance().stop();
      std::fprintf(stderr, "%s: run failed: %s\n", prog, e.what());
      return kExitRun;
    }
    const bt::BacktestResult& result = results[k];
    std::printf("arm %s\n%s", arm.c_str(), result.summary_table().c_str());
    if (!c.output_dir.empty() && !result.write_all(c.output_dir)) {
      Logger::instance().stop();
      std::fprintf(stderr, "%s: cannot write results to %s\n", prog, c.output_dir.c_str());
      return kExitRun;
    }
    std::printf("session journal: %s\n\n", c.journal_out.c_str());
    journals.push_back(c.journal_out);
  }
  Logger::instance().stop();
  try {
    std::fputs(bt::format_ab_report(bt::ab_report(journals, bt::AbReportOptions{})).c_str(),
               stdout);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: the readout: %s\n", prog, e.what());
    return kExitRun;
  }
  return 0;
}

// --sweep: the grid's points, under the queue fill models in multiverses (one pass over the data
// for a group of points, bt::sweep_by_name), with --solo as separate backtests. One line per
// point. --verify K runs K points, spread over the grid, again as backtests of their own and
// compares each with its sweep result (bt::result_difference).
int run_sweep(const char* prog,
              const bt::BacktestConfig& cfg,
              const std::vector<std::string>& sweeps,
              const std::string& data,
              int threads,
              bool solo,
              std::size_t verify) {
  bt::ParamGrid grid;
  for (const std::string& s : sweeps) {
    const std::size_t eq = s.find('=');
    std::vector<std::string> values;
    std::size_t pos = eq + 1;
    for (;;) {
      const std::size_t comma = s.find(',', pos);
      values.push_back(s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    grid.emplace_back(s.substr(0, eq), std::move(values));
  }
  const bt::SourceFactory factory = [&]() -> std::unique_ptr<bt::MdSource> {
    return data.empty() ? bt::open_source(cfg) : bt::open_data(data, &cfg.instruments);
  };
  Logger::instance().start(stderr, LogLevel::Warn);
  std::vector<bt::SweepPoint> points;
  const auto t0 = std::chrono::steady_clock::now();
  try {
    points = bt::sweep_by_name(cfg, cfg.strategy, grid, factory, threads, solo);
  } catch (const std::invalid_argument& e) {
    Logger::instance().stop();
    std::fprintf(stderr, "%s: %s\n", prog, e.what());
    return kExitConfig;
  } catch (const std::exception& e) {
    Logger::instance().stop();
    std::fprintf(stderr, "%s: run failed: %s\n", prog, e.what());
    return kExitRun;
  }
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  Logger::instance().stop();
  std::uint64_t events = 0;
  for (const bt::SweepPoint& p : points) {
    std::string label;
    for (const auto& kv : grid)
      label.append(label.empty() ? "" : " ")
          .append(kv.first)
          .append("=")
          .append(p.params.at(kv.first));
    const bt::Metrics& m = p.result.metrics;
    std::printf("%-40s net %14.4f  fills %8llu  orders %8llu  outbound %s\n",
                label.c_str(),
                m.net_pnl,
                static_cast<unsigned long long>(m.fills),
                static_cast<unsigned long long>(m.orders),
                p.result.outbound_sha256.substr(0, 16).c_str());
    events += p.result.md_events;
  }
  const bool shared = !solo && bt::multiverse_refusal({cfg}).empty();
  std::printf("%zu points %s in %.2f s, %.1f M market-data events a second over the runs\n",
              points.size(),
              shared ? "in multiverses" : "as separate backtests",
              wall,
              wall > 0 ? static_cast<double>(events) / wall / 1e6 : 0.0);
  if (verify == 0 || points.empty()) return 0;
  const std::size_t n = std::min(verify, points.size());
  const bt::BacktestConfig start = bt::shared_start(cfg);
  for (std::size_t k = 0; k < n; ++k) {
    const bt::SweepPoint& p = points[k * points.size() / n];
    bt::BacktestConfig c = start;
    for (const auto& [key, v] : p.params) c.params[key] = v;
    c.output_dir.clear();
    c.journal_out.clear();
    const std::unique_ptr<bt::MdSource> source = factory();
    const std::string diff =
        bt::result_difference(bt::run_backtest(c, cfg.strategy, source.get()), p.result);
    if (!diff.empty()) {
      std::fprintf(
          stderr, "%s: --verify: a point gives another result alone: %s\n", prog, diff.c_str());
      return kExitRun;
    }
  }
  std::printf("%zu points run again alone: the same results, fill for fill\n", n);
  return 0;
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
  std::string format_arg = "text";
  std::vector<std::string> params;
  std::uint64_t seed = 0;
  std::uint64_t duration_s = 0;
  bool list = false;
  std::vector<std::string> sweeps;
  int threads = 0;
  bool solo = false;
  std::size_t verify = 0;

  CLI::App app("Backtests a registered strategy.", program);
  setup(app);
  app.add_option("--config", config_path, "engine / strategy / backtest configuration (required)")
      ->option_text("<file.toml>");
  app.add_option("--data",
                 data,
                 "market data: 'synthetic', a *.fmj / *.csv path, or <source>:<args>; several "
                 "separated by ';' merge by time (default: [backtest] source/path). "
                 "`fastmm-data list` prints the sources")
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
  app.add_option("--sweep",
                 sweeps,
                 "a parameter's values, every combination with the other --sweep keys run "
                 "(repeatable); under l2_queue and l3_queue the points share their passes over "
                 "the data")
      ->option_text("<key=v1,v2,...>")
      ->allow_extra_args(false)
      ->check(key_value());
  app.add_option("--threads", threads, "--sweep: worker threads (default: every core)")
      ->option_text("<n>");
  app.add_flag("--solo", solo, "--sweep: run every point as a backtest of its own");
  app.add_option("--verify",
                 verify,
                 "--sweep: run this many points again as backtests of their own and compare the "
                 "results, fill for fill")
      ->option_text("<n>");
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
    source = data.empty() ? bt::open_source(cfg) : bt::open_data(data, &cfg.instruments);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s: data error: %s\n", prog, e.what());
    return kExitData;
  }

  if (!sweeps.empty()) return run_sweep(prog, cfg, sweeps, data, threads, solo, verify);
  if (cfg.ab.enabled() && cfg.ab.assignment == "branch")
    return run_branches(prog, cfg, source.get());

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
  if (source) {
    if (const std::string note = source->note(); !note.empty()) std::printf("%s\n", note.c_str());
  }
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
