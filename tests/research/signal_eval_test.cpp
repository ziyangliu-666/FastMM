// evaluate_signal(): the information coefficient, the bucket table and the conditional touch
// markout, on tables small enough to compute by hand.
#include "fastmm/research/signal_eval.hpp"

#include "test_support.hpp"

#include "fastmm/core/book/book_features.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/time.hpp"

#include <cmath>
#include <functional>
#include <limits>

using namespace fastmm;
using namespace fastmm::research;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}

// A table of `n` rows, all with mid 100 and a 0.02 wide book, whose forward mid at the single
// 1 s horizon moves by `move(i)` ticks of 0.01 and whose signal value is `i`.
FeatureTable straight_line(std::size_t n, const std::function<int(std::size_t)>& move) {
  FeatureTable t;
  t.coverage.resize(1);
  t.coverage[0].horizon_ns = seconds(1).ns;
  t.rows.horizon_ns = {seconds(1).ns};
  t.rows.forward_mid.resize(1);
  for (std::size_t i = 0; i < n; ++i) {
    t.rows.ts.push_back(static_cast<std::int64_t>(i) * seconds(1).ns);
    t.rows.instrument.push_back(0);
    t.rows.mid.push_back(px("100").raw);
    t.rows.microprice.push_back(px("100").raw);
    t.rows.best_bid.push_back(px("99.99").raw);
    t.rows.best_ask.push_back(px("100.01").raw);
    t.rows.bid_qty.push_back(Qty::from_decimal("1")->raw);
    t.rows.ask_qty.push_back(Qty::from_decimal("1")->raw);
    t.rows.imbalance.push_back(0);
    t.rows.spread.push_back(px("0.02").raw);
    t.rows.forward_mid[0].push_back(px("100").raw + move(i) * px("0.01").raw);
    ++t.coverage[0].resolved;
  }
  return t;
}

std::vector<double> ramp(std::size_t n) {
  std::vector<double> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<double>(i);
  return v;
}

}  // namespace

TEST_CASE("research.eval: a monotone signal has an information coefficient of 1") {
  const FeatureTable t = straight_line(100, [](std::size_t i) { return static_cast<int>(i); });
  const std::vector<double> v = ramp(100);
  const SignalEval e = evaluate_signal(t, v, "ramp");
  CHECK(e.signal == "ramp");
  CHECK(e.rows == 100);
  REQUIRE(e.horizons.size() == 1);
  const HorizonEval& h = e.horizons[0];
  CHECK(h.n == 100);
  CHECK(h.label() == "1s");
  CHECK(h.ic == doctest::Approx(1.0));
  CHECK(h.block_ic.size() == 10);
  CHECK(h.block_ic_mean == doctest::Approx(1.0));
  CHECK(h.block_ic_stdev == doctest::Approx(0.0));
  CHECK(h.block_sign_agreement == doctest::Approx(1.0));

  const std::vector<double> down(v.rbegin(), v.rend());
  CHECK(evaluate_signal(t, down, "down").horizons[0].ic == doctest::Approx(-1.0));
}

TEST_CASE("research.eval: a signal that says nothing has an information coefficient near zero") {
  // Forward move alternates sign, so the ranking by signal carries no ranking of the move.
  const FeatureTable t = straight_line(1000, [](std::size_t i) { return i % 2 == 0 ? 1 : -1; });
  const SignalEval e = evaluate_signal(t, ramp(1000), "ramp");
  CHECK(std::abs(e.horizons[0].ic) < 0.05);
}

TEST_CASE("research.eval: the bucket table is the decile table of the forward move") {
  // Ten deciles of ten rows; bucket b moves by b - 5 ticks, so the table is monotone and
  // straddles zero.
  const FeatureTable t =
      straight_line(100, [](std::size_t i) { return static_cast<int>(i / 10) - 5; });
  const SignalEval e = evaluate_signal(t, ramp(100), "ramp");
  const HorizonEval& h = e.horizons[0];
  REQUIRE(h.buckets.size() == 10);
  for (std::size_t b = 0; b < 10; ++b) {
    const SignalBucket& bk = h.buckets[b];
    CHECK(bk.n == 10);
    CHECK(bk.lo == doctest::Approx(static_cast<double>(b * 10)));
    CHECK(bk.hi == doctest::Approx(static_cast<double>(b * 10 + 9)));
    // (b - 5) ticks of 0.01 on a mid of 100 is (b - 5) basis points.
    const double move_bps = static_cast<double>(b) - 5.0;
    CHECK(bk.forward_bps == doctest::Approx(move_bps));
    // The touch is 0.01 away from a mid of 100: a half spread of 1 bp.
    CHECK(bk.half_spread_bps == doctest::Approx(1.0));
    CHECK(bk.buy_bps == doctest::Approx(1.0 + move_bps));
    CHECK(bk.sell_bps == doctest::Approx(1.0 - move_bps));
  }
  // The two sides of one quote average to the half spread: the move cancels.
  for (const SignalBucket& bk : h.buckets)
    CHECK((bk.buy_bps + bk.sell_bps) / 2.0 == doctest::Approx(bk.half_spread_bps));
}

TEST_CASE("research.eval: rows without a forward mid are left out and counted") {
  FeatureTable t = straight_line(10, [](std::size_t) { return 1; });
  t.rows.forward_mid[0][3] = 0;
  t.coverage[0].resolved = 9;
  t.coverage[0].excluded_past_end = 1;
  const SignalEval e = evaluate_signal(t, ramp(10), "ramp");
  CHECK(e.horizons[0].n == 9);
  CHECK(e.horizons[0].excluded_past_end == 1);
  std::uint64_t counted = 0;
  for (const SignalBucket& b : e.horizons[0].buckets) counted += b.n;
  CHECK(counted == 9);
}

TEST_CASE("research.eval: a signal value that is not finite drops its row") {
  const FeatureTable t = straight_line(10, [](std::size_t) { return 1; });
  std::vector<double> v = ramp(10);
  v[2] = std::numeric_limits<double>::quiet_NaN();
  CHECK(evaluate_signal(t, v, "ramp").horizons[0].n == 9);
}

TEST_CASE("research.eval: the built-in features read the table's own columns") {
  FeatureTable t = straight_line(4, [](std::size_t) { return 0; });
  t.rows.imbalance[0] = Ratio::from_decimal("0.5")->raw;
  t.rows.microprice[1] = px("100.01").raw;
  const std::vector<double> imb = feature_values(t, Feature::Imbalance);
  CHECK(imb[0] == doctest::Approx(0.5));
  CHECK(imb[1] == doctest::Approx(0.0));
  // (100.01 - 100) / 100 is 1 basis point.
  CHECK(feature_values(t, Feature::MicropriceEdge)[1] == doctest::Approx(1.0));
  // A 0.02 spread on a mid of 100 is 2 basis points.
  CHECK(feature_values(t, Feature::Spread)[0] == doctest::Approx(2.0));
  CHECK(to_string(Feature::Imbalance) == "imbalance");
  CHECK(evaluate_feature(t, Feature::Imbalance).signal == "imbalance");
}

TEST_CASE("research.eval: block ICs disagree when the relationship flips halfway") {
  // The move follows the signal in the first half and fights it in the second.
  const FeatureTable t = straight_line(
      200, [](std::size_t i) { return i < 100 ? static_cast<int>(i) : static_cast<int>(200 - i); });
  EvalConfig cfg;
  cfg.blocks = 2;
  const SignalEval e = evaluate_signal(t, ramp(200), "ramp", cfg);
  const HorizonEval& h = e.horizons[0];
  REQUIRE(h.block_ic.size() == 2);
  CHECK(h.block_ic[0] == doctest::Approx(1.0));
  CHECK(h.block_ic[1] == doctest::Approx(-1.0));
  CHECK(h.block_sign_agreement == doctest::Approx(0.5));
  CHECK(h.block_ic_stdev == doctest::Approx(1.0));
}

TEST_CASE("research.eval: bad arguments are rejected") {
  const FeatureTable t = straight_line(4, [](std::size_t) { return 0; });
  CHECK_THROWS_AS(static_cast<void>(evaluate_signal(t, ramp(3), "short")), std::invalid_argument);
  EvalConfig zero_buckets;
  zero_buckets.buckets = 0;
  CHECK_THROWS_AS(static_cast<void>(evaluate_signal(t, ramp(4), "x", zero_buckets)),
                  std::invalid_argument);
  EvalConfig zero_blocks;
  zero_blocks.blocks = 0;
  CHECK_THROWS_AS(static_cast<void>(evaluate_signal(t, ramp(4), "x", zero_blocks)),
                  std::invalid_argument);
}

TEST_CASE("research.eval: the printed table names every horizon") {
  FeatureTable t = straight_line(20, [](std::size_t i) { return static_cast<int>(i); });
  const std::string s = evaluate_signal(t, ramp(20), "ramp").table();
  CHECK(s.find("signal: ramp") != std::string::npos);
  CHECK(s.find("horizon 1s") != std::string::npos);
  CHECK(s.find("IC (Spearman) +1.0000") != std::string::npos);
}
