// Walk-forward sweeps: TimeSliceSource and walk_forward() over synthetic data.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/sweep.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <cmath>
#include <stdexcept>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {
Timestamp time_of(const EventHeader& h) {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

BacktestConfig queue_config(std::uint64_t seed, Duration d) {
  BacktestConfig c = synthetic_config(seed, d);
  c.transport.fill_model = sim::FillModel::L2Queue;
  return c;
}

const ParamGrid kGrid = {{"half_spread_bps", {"0.003", "0.05", "0.5"}},
                         {"quote_qty", {"0.001", "0.002"}}};
}  // namespace

TEST_CASE("backtest.walk_forward: a time slice starts on the book of the full stream at t0") {
  SyntheticSourceConfig sc;
  sc.generator = synthetic_config(5, seconds(6)).generator;
  sc.seed = 5;
  sc.duration = seconds(6);
  SyntheticSource full(sc);
  const Timestamp t0 = sc.start + seconds(2);
  const Timestamp t1 = sc.start + seconds(4);

  L2Book<256> at_t0;
  std::vector<std::string> inside;  // the full stream's events in [t0, t1)
  while (const EventHeader* h = full.next()) {
    if (time_of(*h) < t0) {
      if (h->type == EventType::BookDelta || h->type == EventType::BookSnapshot)
        at_t0.apply_delta(msg_cast<BookDeltaMsg>(h));
    } else if (time_of(*h) < t1) {
      inside.push_back(to_csv_rows(*h));
    }
  }
  REQUIRE(at_t0.is_valid());
  REQUIRE(!inside.empty());

  TimeSliceSource slice(full, t0, t1);
  for (int pass = 0; pass < 2; ++pass) {  // reset() replays the same slice
    CAPTURE(pass);
    slice.reset();
    CHECK(slice.start_ts() == t0);
    const EventHeader* h = slice.next();
    REQUIRE(h != nullptr);
    REQUIRE(h->type == EventType::BookSnapshot);
    CHECK(time_of(*h) == t0);
    L2Book<256> primed;
    primed.apply_delta(msg_cast<BookDeltaMsg>(h));
    CHECK(primed.seq() == at_t0.seq());
    for (Side s : {Side::Buy, Side::Sell}) {
      REQUIRE(primed.depth(s) == at_t0.depth(s));
      for (std::size_t i = 0; i < primed.depth(s); ++i) {
        CHECK(primed.level(s, i).price == at_t0.level(s, i).price);
        CHECK(primed.level(s, i).qty == at_t0.level(s, i).qty);
      }
    }
    std::vector<std::string> rest;
    while (const EventHeader* e = slice.next()) {
      CHECK(time_of(*e) >= t0);
      if (e->type == EventType::BookTicker && time_of(*e) == t0 && rest.empty()) continue;
      rest.push_back(to_csv_rows(*e));
    }
    CHECK(slice.next() == nullptr);
    CHECK(rest == inside);
  }
}

TEST_CASE(
    "backtest.walk_forward: K folds give K-1 out-of-sample rows chosen on the previous fold") {
  const BacktestConfig base = queue_config(3, seconds(12));
  const WalkForwardReport rep = walk_forward_by_name(base, "basic_mm", kGrid, {}, 3, "net_pnl", 4);
  const std::vector<ParamMap> points = expand_grid(kGrid);
  REQUIRE(rep.folds.size() == 3);
  CHECK(rep.metric == "net_pnl");
  CHECK(rep.param_names == std::vector<std::string>{"half_spread_bps", "quote_qty"});
  double in = 0.0;
  double out = 0.0;
  double hind = 0.0;
  for (std::size_t i = 0; i < rep.folds.size(); ++i) {
    CAPTURE(i);
    const WalkForwardFold& f = rep.folds[i];
    REQUIRE(f.points.size() == points.size());
    REQUIRE(f.scores.size() == points.size());
    CHECK(f.start_ts < f.end_ts);
    if (i > 0) CHECK(f.start_ts == rep.folds[i - 1].end_ts);
    for (std::size_t p = 0; p < points.size(); ++p) {
      CHECK(f.points[p].params == points[p]);
      CHECK(f.scores[p] == f.points[p].result.metrics.net_pnl);
      CHECK(f.scores[p] <= f.scores[f.best]);
      CHECK(f.points[p].result.metrics.orders > 0);  // quotes from the fold's first instant
      // Every run covers its fold only.
      CHECK(f.points[p].result.start_ts == f.start_ts);
      CHECK(f.points[p].result.end_ts < f.end_ts);
    }
    if (i == 0) {
      CHECK(std::isnan(f.in_sample));
      CHECK(std::isnan(f.out_of_sample));
      continue;
    }
    const WalkForwardFold& prev = rep.folds[i - 1];
    CHECK(f.chosen == prev.best);
    CHECK(f.in_sample == prev.scores[prev.best]);
    CHECK(f.out_of_sample == f.scores[f.chosen]);
    CHECK(f.hindsight == f.scores[f.best]);
    CHECK(f.out_of_sample <= f.hindsight);
    in += f.in_sample;
    out += f.out_of_sample;
    hind += f.hindsight;
  }
  CHECK(rep.mean_in_sample == doctest::Approx(in / 2));
  CHECK(rep.mean_out_of_sample == doctest::Approx(out / 2));
  CHECK(rep.mean_hindsight == doctest::Approx(hind / 2));
  CHECK(rep.choice_changes == (rep.folds[2].chosen != rep.folds[1].chosen ? 1U : 0U));
  CHECK(rep.folds[0].points.front().result.metrics.fills > 0);

  // A fold is an ordinary backtest from a flat engine over the slice.
  const WalkForwardFold& f1 = rep.folds[1];
  BacktestConfig cfg = base;
  for (const auto& [k, v] : points[2]) cfg.params[k] = v;
  SyntheticSource synth(synthetic_source_config(base));
  TimeSliceSource slice(synth, Timestamp{f1.start_ts}, Timestamp{f1.end_ts});
  const BacktestResult alone = run_backtest<BasicMM>(cfg, &slice);
  CHECK(alone.outbound_sha256 == f1.points[2].result.outbound_sha256);
  CHECK(alone.metrics.net_pnl == f1.scores[2]);

  const std::string table = rep.table();
  CHECK(table.find("walk-forward: 3 folds, 6 points, metric net_pnl") == 0);
  CHECK(table.find("choice changes") != std::string::npos);
}

TEST_CASE("backtest.walk_forward: two runs are identical whatever the thread count") {
  const BacktestConfig base = queue_config(9, seconds(8));
  const WalkForwardReport a = walk_forward_by_name(base, "basic_mm", kGrid, {}, 4, "net_pnl", 1);
  const WalkForwardReport b = walk_forward_by_name(base, "basic_mm", kGrid, {}, 4, "net_pnl", 5);
  REQUIRE(a.folds.size() == b.folds.size());
  for (std::size_t i = 0; i < a.folds.size(); ++i) {
    for (std::size_t p = 0; p < a.folds[i].points.size(); ++p) {
      CHECK(a.folds[i].points[p].result.outbound_sha256 ==
            b.folds[i].points[p].result.outbound_sha256);
      CHECK(a.folds[i].scores[p] == b.folds[i].scores[p]);
    }
  }
  CHECK(a.table() == b.table());
}

TEST_CASE("backtest.walk_forward: one fold is the plain sweep") {
  const BacktestConfig base = synthetic_config(3, seconds(4));  // coupled matching market
  const std::vector<SweepPoint> s = sweep_by_name(base, "basic_mm", kGrid, {}, 3);
  const WalkForwardReport rep = walk_forward_by_name(base, "basic_mm", kGrid, {}, 1, "net_pnl", 2);
  REQUIRE(rep.folds.size() == 1);
  const WalkForwardFold& f = rep.folds[0];
  REQUIRE(f.points.size() == s.size());
  std::size_t best = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    CAPTURE(i);
    CHECK(f.points[i].params == s[i].params);
    CHECK(f.points[i].result.outbound_sha256 == s[i].result.outbound_sha256);
    CHECK(f.points[i].result.metrics.net_pnl == s[i].result.metrics.net_pnl);
    CHECK(f.points[i].result.fills.size() == s[i].result.fills.size());
    if (s[i].result.metrics.net_pnl > s[best].result.metrics.net_pnl) best = i;
  }
  CHECK(f.best == best);
  CHECK(std::isnan(rep.mean_out_of_sample));
  CHECK(rep.choice_changes == 0);
}

TEST_CASE("backtest.walk_forward: rejects what it cannot slice or rank") {
  const BacktestConfig coupled = synthetic_config(3, seconds(2));
  CHECK_THROWS_AS(walk_forward_by_name(coupled, "basic_mm", kGrid, {}, 2), std::invalid_argument);
  const BacktestConfig queue = queue_config(3, seconds(2));
  CHECK_THROWS_AS(walk_forward_by_name(queue, "basic_mm", kGrid, {}, 0), std::invalid_argument);
  CHECK_THROWS_AS(walk_forward_by_name(queue, "basic_mm", kGrid, {}, 2, "fills"),
                  std::invalid_argument);
}

TEST_CASE("backtest.walk_forward: the in-sample winner of a flat market loses once it trends") {
  // A flat synthetic market for 20 s, then 20 one-second markets whose mid steps up 5 USD
  // each: a trend the tight quote sells into. Two folds cut the data at the change, so the tight
  // quote wins fold 0 (maker rebate on a still mid) and is the wrong choice for fold 1, where a
  // quote too wide to fill scores 0.
  BacktestConfig base = queue_config(21, seconds(20));
  base.params["max_inventory"] = "0.01";
  SyntheticSourceConfig flat = synthetic_source_config(base);
  flat.generator.regimes = false;
  flat.generator.mid_step_rate_per_s = 0.2;
  std::vector<std::unique_ptr<SyntheticSource>> parts;
  parts.push_back(std::make_unique<SyntheticSource>(flat));
  for (std::int64_t i = 0; i < 20; ++i) {
    SyntheticSourceConfig step = flat;
    step.seed = 100 + static_cast<std::uint64_t>(i);
    step.duration = seconds(1);
    step.start = flat.start + flat.duration + seconds(i);
    step.generator.start_mid = flat.generator.start_mid + Price::from_int(5 * (i + 1));
    parts.push_back(std::make_unique<SyntheticSource>(step));
  }
  std::vector<MdSource*> chain;
  for (const auto& p : parts) chain.push_back(p.get());
  ChainSource all(chain);
  const std::string csv = source_to_csv(all);
  const SourceFactory factory = [&csv] {
    return std::unique_ptr<MdSource>(std::make_unique<CsvSource>(CsvSource::from_text(csv)));
  };
  const ParamGrid grid = {{"half_spread_bps", {"0.003", "0.5"}}};
  const WalkForwardReport rep = walk_forward_by_name(base, "basic_mm", grid, factory, 2);
  REQUIRE(rep.folds.size() == 2);
  const WalkForwardFold& f0 = rep.folds[0];
  const WalkForwardFold& f1 = rep.folds[1];
  CHECK(f0.best == 0);  // the tight quote wins in-sample
  CHECK(f0.scores[0] > 0.0);
  CHECK(f1.chosen == 0);  // is carried forward
  CHECK(f1.best == 1);    // and loses to staying out of the trend
  CHECK(f1.out_of_sample < 0.0);
  CHECK(f1.out_of_sample < f1.hindsight);
  CHECK(rep.mean_out_of_sample < rep.mean_in_sample);
}
