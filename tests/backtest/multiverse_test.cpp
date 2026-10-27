// Multiverse: the runs of one pass over recorded data give what each gives alone.
#include "fastmm/backtest/multiverse.hpp"

#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/sweep.hpp"
#include "fastmm/backtest/synthetic_source.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

BacktestConfig queue_config(std::uint64_t seed, Duration d) {
  BacktestConfig c = synthetic_config(seed, d);
  c.transport.fill_model = sim::FillModel::L2Queue;
  c.transport.queue_conservatism_bps = 5'000;
  return c;
}

SourceFactory synthetic_factory(const BacktestConfig& c) {
  return [c]() -> std::unique_ptr<MdSource> {
    return std::make_unique<SyntheticSource>(synthetic_source_config(c));
  };
}

void check_same(const BacktestResult& a, const BacktestResult& b) {
  CHECK(a.outbound_sha256 == b.outbound_sha256);
  CHECK(a.outbound_messages == b.outbound_messages);
  CHECK(a.md_events == b.md_events);
  CHECK(a.engine_steps == b.engine_steps);
  CHECK(a.fills.size() == b.fills.size());
  CHECK(a.orders.size() == b.orders.size());
  CHECK(a.equity.size() == b.equity.size());
  CHECK(a.metrics.net_pnl == b.metrics.net_pnl);
  CHECK(a.metrics.realized_pnl == b.metrics.realized_pnl);
  CHECK(a.metrics.fees == b.metrics.fees);
  CHECK(a.metrics.max_drawdown == b.metrics.max_drawdown);
  CHECK(a.transport.fills == b.transport.fills);
  CHECK(a.transport.cross_fills == b.transport.cross_fills);
  CHECK(a.transport.md_forwarded == b.transport.md_forwarded);
  CHECK(a.start_ts == b.start_ts);
  CHECK(a.end_ts == b.end_ts);
  CHECK(a.fills.ts == b.fills.ts);
  CHECK(a.fills.price == b.fills.price);
  CHECK(a.fills.qty == b.fills.qty);
  CHECK(a.fills.queue_ahead == b.fills.queue_ahead);
  CHECK(a.fills.mid == b.fills.mid);
  CHECK(a.fills.markout_mid == b.fills.markout_mid);
  CHECK(a.equity.ts == b.equity.ts);
  CHECK(a.equity.realized == b.equity.realized);
  CHECK(a.equity.unrealized == b.equity.unrealized);
}

const ParamGrid kGrid = {{"half_spread_bps", {"0.003", "0.01", "0.05"}},
                         {"quote_qty", {"0.001", "0.002"}}};

}  // namespace

TEST_CASE("backtest.multiverse: every point of a sweep is the backtest it is alone") {
  const BacktestConfig base = queue_config(11, seconds(20));
  const SourceFactory factory = synthetic_factory(base);
  const std::vector<SweepPoint> solo = sweep_by_name(base, "basic_mm", kGrid, factory, 1, true);
  for (const int threads : {1, 2, 4}) {
    CAPTURE(threads);
    const std::vector<SweepPoint> mv = sweep_multiverse(base, "basic_mm", kGrid, factory, threads);
    REQUIRE(mv.size() == solo.size());
    for (std::size_t i = 0; i < mv.size(); ++i) {
      CAPTURE(i);
      CHECK(mv[i].params == solo[i].params);
      check_same(mv[i].result, solo[i].result);
    }
  }
  // The points differ from each other: the runs did not leak into one another.
  CHECK(solo[0].result.outbound_sha256 != solo[5].result.outbound_sha256);
  CHECK(solo[0].result.metrics.fills > 0);
}

TEST_CASE("backtest.multiverse: a sweep of the synthetic market and of more points than a pass") {
  // No data: every point reads the synthetic market as a stream, alone or together. 40 points take
  // several passes per worker.
  const BacktestConfig base = queue_config(13, seconds(5));
  ParamGrid grid = {{"half_spread_bps", {}}, {"quote_qty", {"0.001", "0.002"}}};
  for (int i = 1; i <= 20; ++i) grid[0].second.push_back(std::to_string(0.002 * i));
  const std::vector<SweepPoint> solo = sweep_by_name(base, "basic_mm", grid, {}, 3, true);
  const std::vector<SweepPoint> mv = sweep_by_name(base, "basic_mm", grid, {}, 3);
  REQUIRE(mv.size() == 40);
  REQUIRE(solo.size() == 40);
  for (std::size_t i = 0; i < mv.size(); ++i) {
    CAPTURE(i);
    CHECK(mv[i].params == solo[i].params);
    check_same(mv[i].result, solo[i].result);
  }
}

TEST_CASE("backtest.multiverse: a sweep's points set backtest settings, as a grid or a list") {
  const BacktestConfig base = queue_config(11, seconds(15));
  const SourceFactory factory = synthetic_factory(base);
  const ParamGrid grid = {{"latency_fixed_us", {"100", "2000"}},
                          {"queue_conservatism", {"0", "1"}},
                          {"maker_fee_bps", {"-1", "2.5"}}};
  const std::vector<SweepPoint> solo = sweep_by_name(base, "basic_mm", grid, factory, 2, true);
  const std::vector<SweepPoint> mv = sweep_by_name(base, "basic_mm", grid, factory, 2);
  REQUIRE(mv.size() == 8);
  for (std::size_t i = 0; i < mv.size(); ++i) {
    CAPTURE(i);
    CHECK(mv[i].params == solo[i].params);
    check_same(mv[i].result, solo[i].result);
    CHECK(mv[i].result.params.count("latency_fixed_us") == 0);
  }
  CHECK(solo[0].result.metrics.fees < 0.0);  // the rebate
  CHECK(solo[1].result.metrics.fees > 0.0);
  CHECK(solo[0].result.outbound_sha256 != solo[4].result.outbound_sha256);  // the latency

  const BacktestConfig c = point_config(base, {{"latency_fixed_us", "750"}, {"levels", "2"}});
  CHECK(c.transport.order_out.fixed == microseconds(750));
  CHECK(c.transport.ack_in.fixed == microseconds(750));
  CHECK(c.params.at("levels") == "2");
  CHECK_FALSE(c.keep_orders);

  // A list: any points, each with the keys it sets.
  const SweepPoints list = {{{"half_spread_bps", "0.01"}, {"latency_fixed_us", "500"}},
                            {{"quote_qty", "0.002"}}};
  const std::vector<SweepPoint> some = sweep_by_name(base, "basic_mm", list, factory, 2);
  REQUIRE(some.size() == 2);
  CHECK(some[0].params == list[0]);
  BacktestConfig alone = point_config(shared_start(base), list[1]);
  const std::unique_ptr<MdSource> source = factory();
  check_same(some[1].result, run_backtest(alone, "basic_mm", source.get()));

  for (const char* bad : {"-1", "1.5", "x"}) {
    CAPTURE(bad);
    CHECK_THROWS_AS(
        sweep_by_name(base, "basic_mm", ParamGrid{{"latency_fixed_us", {bad}}}, factory, 1),
        std::invalid_argument);
  }
  CHECK_THROWS_AS(
      sweep_by_name(base, "basic_mm", ParamGrid{{"queue_conservatism", {"2"}}}, factory, 1),
      std::invalid_argument);
}

TEST_CASE("backtest.multiverse: runs with their own seeds, latencies and conservatism") {
  // What a run draws and how it fills is its own; only the recorded market is shared.
  std::vector<BacktestConfig> cfgs;
  for (int k = 0; k < 4; ++k) {
    BacktestConfig c = queue_config(11, seconds(15));
    c.transport.seed = 100 + static_cast<std::uint64_t>(k);
    c.transport.order_out.jitter = microseconds(50 * (k + 1));
    c.transport.queue_conservatism_bps = 2'500 * k;
    c.transport.own_orders_in_feed = k % 2 == 0;
    cfgs.push_back(c);
  }
  const SourceFactory factory = synthetic_factory(cfgs.front());
  const std::vector<BacktestResult> mv = run_multiverse(cfgs, "basic_mm", factory, 1);
  REQUIRE(mv.size() == cfgs.size());
  for (std::size_t i = 0; i < cfgs.size(); ++i) {
    CAPTURE(i);
    const std::unique_ptr<MdSource> source = factory();
    check_same(mv[i], run_backtest(cfgs[i], "basic_mm", source.get()));
  }
}

TEST_CASE("backtest.multiverse: a run that stops early leaves the others running") {
  std::vector<BacktestConfig> cfgs;
  for (int k = 0; k < 3; ++k) {
    BacktestConfig c = queue_config(7, seconds(20));
    // The middle run's loss limit trips the kill switch; its engine stops quoting.
    if (k == 1) c.engine.risk.max_loss = Notional::from_raw(1);
    cfgs.push_back(c);
  }
  const SourceFactory factory = synthetic_factory(cfgs.front());
  const std::vector<BacktestResult> mv = run_multiverse(cfgs, "basic_mm", factory, 1);
  for (std::size_t i = 0; i < cfgs.size(); ++i) {
    CAPTURE(i);
    const std::unique_ptr<MdSource> source = factory();
    check_same(mv[i], run_backtest(cfgs[i], "basic_mm", source.get()));
  }
}

TEST_CASE("backtest.multiverse: what cannot share a pass is refused") {
  BacktestConfig a = queue_config(3, seconds(2));
  BacktestConfig matching = a;
  matching.transport.fill_model = sim::FillModel::Matching;
  const SourceFactory factory = synthetic_factory(a);
  CHECK_THROWS_AS(static_cast<void>(run_multiverse({a, matching}, "basic_mm", factory, 1)),
                  std::invalid_argument);
  BacktestConfig window = a;
  window.reorder_window = milliseconds(5);
  CHECK_THROWS_AS(static_cast<void>(run_multiverse({a, window}, "basic_mm", factory, 1)),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(run_multiverse({a}, "no_such_strategy", factory, 1)),
                  std::invalid_argument);
  CHECK(run_multiverse({}, "basic_mm", factory, 1).empty());
}

TEST_CASE("backtest.sim: a market-data wire that fills grows and loses nothing") {
  // A second of market-data latency keeps thousands of events in flight.
  BacktestConfig small = queue_config(5, seconds(10));
  small.transport.md_in.fixed = seconds(1);
  small.transport.md_wire_bytes = 4096;
  BacktestConfig big = small;
  big.transport.md_wire_bytes = std::size_t{1} << 26;
  const SourceFactory factory = synthetic_factory(small);
  const std::unique_ptr<MdSource> a = factory();
  const std::unique_ptr<MdSource> b = factory();
  const BacktestResult rs = run_backtest(small, "basic_mm", a.get());
  const BacktestResult rb = run_backtest(big, "basic_mm", b.get());
  CHECK(rs.transport.wire_full == 0);
  CHECK(rs.md_events > 1000);
  check_same(rs, rb);
}

TEST_CASE("backtest.multiverse: a walk-forward's folds share their passes, with the same report") {
  const BacktestConfig base = queue_config(17, seconds(12));
  const RunFn alone = [](const BacktestConfig& c, MdSource* s) {
    return run_backtest(c, "basic_mm", s);
  };
  for (const int folds : {1, 3}) {
    CAPTURE(folds);
    const WalkForwardReport a = walk_forward(base, kGrid, {}, alone, folds, "net_pnl", 2);
    const WalkForwardReport b =
        walk_forward_by_name(base, "basic_mm", kGrid, {}, folds, "net_pnl", 2);
    REQUIRE(a.folds.size() == b.folds.size());
    CHECK(a.table() == b.table());
    for (std::size_t f = 0; f < a.folds.size(); ++f) {
      CHECK(a.folds[f].best == b.folds[f].best);
      CHECK(a.folds[f].start_ts == b.folds[f].start_ts);
      REQUIRE(a.folds[f].points.size() == b.folds[f].points.size());
      for (std::size_t p = 0; p < a.folds[f].points.size(); ++p)
        check_same(a.folds[f].points[p].result, b.folds[f].points[p].result);
    }
  }
}
