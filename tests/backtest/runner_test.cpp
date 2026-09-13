// BacktestRunner end to end: determinism, both shipped strategies on synthetic data, the
// config loader and the parameter sweep.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/backtest/sweep.hpp"
#include "fastmm/strategies/avellaneda_stoikov.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <cmath>
#include <filesystem>
#include <stdexcept>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {
void check_sane(const BacktestResult& r, double max_inventory, double quote_qty) {
  CHECK(r.metrics.fills > 0);
  CHECK(r.engine.fills == r.metrics.fills);
  CHECK(std::isfinite(r.metrics.net_pnl));
  CHECK(std::isfinite(r.metrics.sharpe_bar));
  CHECK(r.metrics.orders > 0);
  CHECK(r.metrics.bars > 0);
  CHECK(r.transport.wire_full == 0);
  CHECK(r.transport.scheduler_full == 0);
  CHECK(r.engine.transport_full == 0);
  // venue-side bar samples may be one fill ahead of the engine's inventory check
  CHECK(r.metrics.inventory_max <= max_inventory + quote_qty + 1e-12);
  CHECK(std::fabs(r.metrics.final_position) <= max_inventory + 1e-12);
  // the backtest ledger and the engine's own position tracker saw the same fills
  CHECK(r.equity.realized.back() == r.engine.realized_pnl_raw);
  CHECK(r.equity.fees.back() == r.engine.fees_raw);
  CHECK(r.outbound_sha256.size() == 64);
}
}  // namespace

TEST_CASE("backtest.runner: two runs with one seed are identical, another seed differs") {
  const BacktestConfig cfg = synthetic_config(5, seconds(10));
  const BacktestResult a = run_backtest(cfg, "basic_mm");
  const BacktestResult b = run_backtest(cfg, "basic_mm");
  BacktestConfig other = cfg;
  other.set_seed(6);
  const BacktestResult c = run_backtest(other, "basic_mm");
  CHECK(a.outbound_sha256 == b.outbound_sha256);
  CHECK(a.fills.ts == b.fills.ts);
  CHECK(a.fills.price == b.fills.price);
  CHECK(a.orders.venue_ts == b.orders.venue_ts);
  CHECK(a.equity.realized == b.equity.realized);
  CHECK(a.equity.unrealized == b.equity.unrealized);
  CHECK(a.metrics.net_pnl == b.metrics.net_pnl);
  CHECK(a.outbound_sha256 != c.outbound_sha256);
  // the template path is the same engine as the registry path
  const BacktestResult t = run_backtest<BasicMM>(cfg);
  CHECK(t.outbound_sha256 == a.outbound_sha256);
  CHECK(t.strategy == "basic_mm");
}

TEST_CASE("backtest.runner: BasicMM on the coupled synthetic market") {
  const BacktestResult r = run_backtest(synthetic_config(42, seconds(30)), "basic_mm");
  check_sane(r, 0.02, 0.002);
  CHECK(r.metrics.maker_fills == r.metrics.fills);  // post-only quotes
  CHECK(r.metrics.quote_uptime > 0.5);
  CHECK(r.metrics.bars >= 30);
  CHECK(r.summary().fills == r.metrics.fills);
  const std::string table = r.summary_table();
  CHECK(table.find("net pnl") != std::string::npos);
  const auto dir = fastmm::test::tmp_dir() / "runner_basic_mm";
  REQUIRE(r.write_all(dir.string()));
  for (const char* f : {"equity.csv", "fills.csv", "orders.csv", "summary.json"})
    CHECK(std::filesystem::file_size(dir / f) > 0);
  const std::string json = fastmm::test::read_file(dir / "summary.json");
  CHECK(json.find("\"outbound_sha256\": \"" + r.outbound_sha256 + "\"") != std::string::npos);
  const std::string fills = fastmm::test::read_file(dir / "fills.csv");
  CHECK(static_cast<std::size_t>(std::count(fills.begin(), fills.end(), '\n')) ==
        r.fills.size() + 1);
}

TEST_CASE("backtest.runner: BasicMM with the L2 queue model on a synthetic stream") {
  BacktestConfig cfg = synthetic_config(42, seconds(30));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 5000;
  check_sane(run_backtest(cfg, "basic_mm"), 0.02, 0.002);
}

TEST_CASE("backtest.runner: AvellanedaStoikov on the coupled synthetic market") {
  BacktestConfig cfg = synthetic_config(42, seconds(30));
  cfg.params = {{"gamma", "0.1"},
                {"kappa", "100"},
                {"sigma_window_s", "5"},
                {"quote_qty", "0.002"},
                {"max_inventory", "0.02"},
                {"min_half_spread_ticks", "1"}};
  const BacktestResult r = run_backtest(cfg, "avellaneda_stoikov");
  CHECK(r.strategy == "avellaneda_stoikov");
  check_sane(r, 0.02, 0.002);
  const BacktestResult again = run_backtest<AvellanedaStoikov>(cfg);
  CHECK(again.outbound_sha256 == r.outbound_sha256);
}

TEST_CASE("backtest.runner: unknown strategies and bad parameters are reported") {
  BacktestConfig cfg = synthetic_config(1, seconds(1));
  CHECK_THROWS_AS(run_backtest(cfg, "nope"), std::invalid_argument);
  cfg.params["no_such_param"] = "1";
  CHECK_THROWS_AS(run_backtest(cfg, "basic_mm"), std::invalid_argument);
  CHECK(register_builtin_strategies() >= 2);
  CHECK(register_builtin_strategies() == register_builtin_strategies());
  CHECK(open_data("synthetic") == nullptr);
  CHECK_THROWS_AS(static_cast<void>(open_data("data.parquet")), std::runtime_error);
}

TEST_CASE("backtest.config: TOML sections map onto the backtest config") {
  const Config c = Config::parse(R"(
[engine]
rng_seed = 9
[venues.sim]
kind = "sim"
[venues.sim.fees]
maker_bps = -1.0
taker_bps = 2.5
[[instruments]]
venue = "sim"
symbol = "ETHUSDT"
base = "ETH"
quote = "USDT"
tick = "0.01"
lot = "0.001"
min_qty = "0.001"
[strategy]
name = "basic_mm"
[strategy.params]
half_spread_bps = 2.0
[risk]
max_position = "1"
stp = true
[sim]
seed = 77
start_mid = "2500"
limit_rate_per_s = 123.0
[backtest]
fill_model = "l2_queue"
queue_conservatism = 0.25
latency_fixed_us = 150
latency_jitter_us = 0
duration_s = 12
equity_bar_s = 2
initial_capital = 1000
)");
  const BacktestConfig b = BacktestConfig::from_config(c);
  CHECK(b.engine.rng_seed == 9);
  CHECK(b.seed == 77);
  CHECK(b.transport.seed == 77);
  CHECK(b.instruments.size() == 1);
  CHECK(b.strategy == "basic_mm");
  CHECK(b.params.at("half_spread_bps") == "2");  // TOML floats are stringified by fmt
  CHECK(b.transport.fill_model == sim::FillModel::L2Queue);
  CHECK(b.transport.queue_conservatism_bps == 2500);
  CHECK(b.transport.order_out.fixed == microseconds(150));
  CHECK(b.transport.order_out.jitter.ns == 0);
  CHECK(b.transport.fees.maker_cbps == -100);
  CHECK(b.transport.fees.taker_cbps == 250);
  CHECK(b.transport.stp == sim::StpMode::CancelTaker);
  CHECK(b.generator.start_mid == px("2500"));
  CHECK(b.generator.tick == px("0.01"));
  CHECK(b.generator.lot == qt("0.001"));
  CHECK(b.generator.limit_rate_per_s == 123.0);
  CHECK(b.duration == seconds(12));
  CHECK(b.equity_bar == seconds(2));
  CHECK(b.initial_capital == 1000.0);
  CHECK(b.engine.risk.max_position == qt("1"));

  const auto with = [&](const char* extra) {
    return Config::parse(std::string("[[instruments]]\nvenue = \"sim\"\nsymbol = \"X\"\n"
                                     "tick = \"0.01\"\nlot = \"0.001\"\n[venues.sim]\n"
                                     "kind = \"sim\"\n") +
                         extra);
  };
  CHECK_THROWS_AS(BacktestConfig::from_config(with("[backtest]\nfill_model = \"magic\"\n")),
                  ConfigError);
  CHECK_THROWS_AS(BacktestConfig::from_config(with("[backtest]\nequity_bar_s = 0\n")), ConfigError);
  CHECK_THROWS_AS(BacktestConfig::from_config(with("[backtest]\nqueue_conservatism = 2\n")),
                  ConfigError);
}

TEST_CASE("backtest.sweep: cartesian grid runs in parallel and returns results in grid order") {
  const ParamGrid grid = {{"half_spread_bps", {"0.003", "0.5", "2"}},
                          {"quote_qty", {"0.001", "0.002"}}};
  const std::vector<ParamMap> points = expand_grid(grid);
  REQUIRE(points.size() == 6);
  CHECK(points[0].at("half_spread_bps") == "0.003");
  CHECK(points[0].at("quote_qty") == "0.001");
  CHECK(points[1].at("quote_qty") == "0.002");
  CHECK(points[5].at("half_spread_bps") == "2");
  CHECK(expand_grid({}).size() == 1);

  const BacktestConfig base = synthetic_config(3, seconds(4));
  const std::vector<SweepPoint> res = sweep_by_name(base, "basic_mm", grid, {}, 3);
  REQUIRE(res.size() == points.size());
  for (std::size_t i = 0; i < res.size(); ++i) {
    CAPTURE(i);
    CHECK(res[i].params == points[i]);
    BacktestConfig cfg = base;
    for (const auto& [k, v] : points[i]) cfg.params[k] = v;
    CHECK(res[i].result.outbound_sha256 == run_backtest(cfg, "basic_mm").outbound_sha256);
    CHECK(res[i].result.params.at("quote_qty") == points[i].at("quote_qty"));
  }
  CHECK(res[0].result.outbound_sha256 != res[1].result.outbound_sha256);

  // template path, one CSV cursor per worker
  BacktestConfig qcfg = base;
  qcfg.transport.fill_model = sim::FillModel::L2Queue;
  SyntheticSourceConfig sc;
  sc.generator = qcfg.generator;
  sc.seed = 3;
  sc.duration = seconds(4);
  SyntheticSource synth(sc);
  const std::string text = source_to_csv(synth);
  const SourceFactory factory = [&text] {
    return std::unique_ptr<MdSource>(std::make_unique<CsvSource>(CsvSource::from_text(text)));
  };
  const std::vector<SweepPoint> q = sweep<BasicMM>(qcfg, grid, factory, 4);
  REQUIRE(q.size() == 6);
  for (std::size_t i = 0; i < q.size(); ++i) {
    BacktestConfig cfg = qcfg;
    for (const auto& [k, v] : points[i]) cfg.params[k] = v;
    CsvSource src = CsvSource::from_text(text);
    CHECK(q[i].result.outbound_sha256 == run_backtest<BasicMM>(cfg, &src).outbound_sha256);
  }

  ParamGrid bad = {{"no_such_param", {"1"}}};
  CHECK_THROWS_AS(sweep_by_name(base, "basic_mm", bad, {}, 2), std::invalid_argument);
}
