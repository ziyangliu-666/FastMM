// The feature extractor: hand-computed rows from a hand-made tape, and the mechanical proof that
// a forward mid is unset until the data reaches the horizon.
#include "fastmm/research/extractor.hpp"

#include "test_support.hpp"

#include "fastmm/backtest/csv_source.hpp"

using namespace fastmm;
using namespace fastmm::research;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}

FeatureTable run(const std::string& tape, const FeatureConfig& cfg) {
  bt::CsvSource src = bt::CsvSource::from_text(tape);
  return extract_features(src, cfg);
}

// Two updates one second apart. Bid 99.99 x 3, ask 100.01 x 1, so the mid is 100, the touch
// imbalance is (3 - 1) / 4 = 0.5 and the microprice is (100.01 * 3 + 99.99 * 1) / 4 = 100.005.
const char* const kTwoUpdates =
    "ts_ns,type,inst,side,price,qty,seq\n"
    "1000000000,S,0,B,99.99,3,1\n"
    "1000000000,S,0,A,100.01,1,1\n"
    "2000000000,D,0,B,99.99,1,2\n"
    "2000000000,D,0,A,100.01,1,2\n";

}  // namespace

TEST_CASE("research.features: the row is the book after the update") {
  FeatureConfig cfg;
  cfg.horizons = {seconds(1)};
  const FeatureTable t = run(kTwoUpdates, cfg);

  REQUIRE(t.rows.size() == 2);
  CHECK(t.book_updates == 2);
  CHECK(t.events == 2);
  CHECK(t.start_ts == seconds(1).ns);
  CHECK(t.end_ts == seconds(2).ns);

  CHECK(t.rows.ts[0] == seconds(1).ns);
  CHECK(t.rows.instrument[0] == 0);
  CHECK(t.rows.mid[0] == px("100").raw);
  CHECK(t.rows.best_bid[0] == px("99.99").raw);
  CHECK(t.rows.best_ask[0] == px("100.01").raw);
  CHECK(t.rows.bid_qty[0] == Qty::from_decimal("3")->raw);
  CHECK(t.rows.ask_qty[0] == Qty::from_decimal("1")->raw);
  CHECK(t.rows.spread[0] == px("0.02").raw);
  CHECK(t.rows.imbalance[0] == Ratio::from_decimal("0.5")->raw);
  CHECK(t.rows.microprice[0] == px("100.005").raw);
  // Equal sizes: imbalance 0 and the microprice falls back on the mid.
  CHECK(t.rows.imbalance[1] == 0);
  CHECK(t.rows.microprice[1] == px("100").raw);

  // The first row is marked at the second update, the second row has nothing left to mark.
  REQUIRE(t.rows.forward_mid.size() == 1);
  CHECK(t.rows.forward_mid[0][0] == px("100").raw);
  CHECK(t.rows.forward_mid[0][1] == 0);
  REQUIRE(t.coverage.size() == 1);
  CHECK(t.coverage[0].horizon_ns == seconds(1).ns);
  CHECK(t.coverage[0].resolved == 1);
  CHECK(t.coverage[0].excluded_past_end == 1);
  CHECK(t.coverage[0].excluded_no_mid == 0);
  CHECK(t.coverage[0].label() == "1s");
}

// The no-look-ahead check, in the shape of tests/backtest/markout_test.cpp: the same row, the same
// horizon, one tape ending 1 ns short of it and one ending on it.
TEST_CASE("research.features: the forward mid is unset at ts + h - 1 ns and set at ts + h") {
  const auto tape = [](std::int64_t last_ts) {
    return std::string(
               "ts_ns,type,inst,side,price,qty,seq\n"
               "1000000000,S,0,B,99.99,1,1\n"
               "1000000000,S,0,A,100.01,1,1\n") +
           std::to_string(last_ts) + ",D,0,B,100.99,1,2\n" + std::to_string(last_ts) +
           ",D,0,A,101.01,1,2\n" + std::to_string(last_ts) + ",D,0,B,99.99,0,2\n" +
           std::to_string(last_ts) + ",D,0,A,100.01,0,2\n";
  };
  FeatureConfig cfg;
  cfg.horizons = {seconds(1)};

  const FeatureTable short_of = run(tape(seconds(2).ns - 1), cfg);
  REQUIRE(short_of.rows.size() == 2);
  CHECK(short_of.rows.ts[0] == seconds(1).ns);
  CHECK(short_of.end_ts == seconds(2).ns - 1);
  CHECK(short_of.rows.forward_mid[0][0] == 0);  // 1 ns short of the horizon: still unset
  CHECK(short_of.coverage[0].resolved == 0);
  CHECK(short_of.coverage[0].excluded_past_end == 2);

  const FeatureTable on_it = run(tape(seconds(2).ns), cfg);
  REQUIRE(on_it.rows.size() == 2);
  CHECK(on_it.end_ts == seconds(2).ns);
  CHECK(on_it.rows.forward_mid[0][0] == px("101").raw);
  CHECK(on_it.coverage[0].resolved == 1);
  CHECK(on_it.coverage[0].excluded_past_end == 1);
}

TEST_CASE("research.features: the forward mid is read at the horizon, not on the way there") {
  // The mid moves to 110 at 1.5 s and back to 101 at 2 s. A row at 1 s marked 1 s later must see
  // 101, not the 110 it passed through and not the 100 it started at.
  const std::string tape =
      "ts_ns,type,inst,side,price,qty,seq\n"
      "1000000000,S,0,B,99.99,1,1\n"
      "1000000000,S,0,A,100.01,1,1\n"
      "1500000000,D,0,B,99.99,0,2\n"
      "1500000000,D,0,A,100.01,0,2\n"
      "1500000000,D,0,B,109.99,1,2\n"
      "1500000000,D,0,A,110.01,1,2\n"
      "2000000000,D,0,B,109.99,0,3\n"
      "2000000000,D,0,A,110.01,0,3\n"
      "2000000000,D,0,B,100.99,1,3\n"
      "2000000000,D,0,A,101.01,1,3\n"
      "3000000000,D,0,B,100.99,1,4\n";
  FeatureConfig cfg;
  cfg.horizons = {seconds(1)};
  const FeatureTable t = run(tape, cfg);
  REQUIRE(t.rows.size() >= 1);
  CHECK(t.rows.ts[0] == seconds(1).ns);
  CHECK(t.rows.mid[0] == px("100").raw);
  CHECK(t.rows.forward_mid[0][0] == px("101").raw);
}

TEST_CASE(
    "research.features: a row whose horizon lands on a one-sided book is unset, not carried") {
  const std::string tape =
      "ts_ns,type,inst,side,price,qty,seq\n"
      "1000000000,S,0,B,99.99,1,1\n"
      "1000000000,S,0,A,100.01,1,1\n"
      "2000000000,D,0,A,100.01,0,2\n"  // the ask side empties exactly at the horizon
      "3000000000,D,0,A,100.01,1,3\n";
  FeatureConfig cfg;
  cfg.horizons = {seconds(1)};
  const FeatureTable t = run(tape, cfg);
  REQUIRE(t.rows.size() == 2);  // the update at 2 s leaves no two-sided book: no row
  CHECK(t.skipped_one_sided == 1);
  CHECK(t.rows.ts[1] == seconds(3).ns);
  CHECK(t.rows.forward_mid[0][0] == 0);
  CHECK(t.coverage[0].excluded_no_mid == 1);
  CHECK(t.coverage[0].excluded_past_end == 1);
  CHECK(t.coverage[0].resolved == 0);
}

TEST_CASE("research.features: several horizons resolve independently") {
  std::string tape = "ts_ns,type,inst,side,price,qty,seq\n";
  for (int i = 0; i < 12; ++i) {
    const std::string ts = std::to_string(seconds(i).ns);
    const std::string bid = std::to_string(100 + i) + ".99";
    const std::string ask = std::to_string(101 + i) + ".01";
    tape += ts + ",S,0,B," + bid + ",1," + std::to_string(i + 1) + "\n";
    tape += ts + ",S,0,A," + ask + ",1," + std::to_string(i + 1) + "\n";
  }
  FeatureConfig cfg;
  cfg.horizons = {seconds(1), seconds(10)};
  const FeatureTable t = run(tape, cfg);
  REQUIRE(t.rows.size() == 12);
  REQUIRE(t.coverage.size() == 2);
  CHECK(t.coverage[0].resolved == 11);  // every row but the last
  CHECK(t.coverage[0].excluded_past_end == 1);
  CHECK(t.coverage[1].resolved == 2);  // the rows at 0 s and 1 s
  CHECK(t.coverage[1].excluded_past_end == 10);
  // Row 0 starts at mid 101 and the mid rises by 1 per second.
  CHECK(t.rows.mid[0] == px("101").raw);
  CHECK(t.rows.forward_mid[0][0] == px("102").raw);
  CHECK(t.rows.forward_mid[1][0] == px("111").raw);
}

TEST_CASE("research.features: subsample keeps at most one row per interval") {
  std::string tape = "ts_ns,type,inst,side,price,qty,seq\n";
  for (int i = 0; i < 10; ++i) {
    const std::string ts = std::to_string(milliseconds(i).ns);
    tape += ts + ",S,0,B,99.99,1," + std::to_string(i + 1) + "\n";
    tape += ts + ",S,0,A,100.01,1," + std::to_string(i + 1) + "\n";
  }
  FeatureConfig cfg;
  cfg.horizons = {milliseconds(1)};
  cfg.subsample = milliseconds(5);
  const FeatureTable t = run(tape, cfg);
  CHECK(t.book_updates == 10);
  REQUIRE(t.rows.size() == 2);  // 0 ms and 5 ms
  CHECK(t.rows.ts[0] == 0);
  CHECK(t.rows.ts[1] == milliseconds(5).ns);
  CHECK(t.skipped_subsample == 8);
}

TEST_CASE("research.features: sample_trades adds a row at the trade, before the update it caused") {
  const std::string tape =
      "ts_ns,type,inst,side,price,qty,seq\n"
      "1000000000,S,0,B,99.99,1,1\n"
      "1000000000,S,0,A,100.01,1,1\n"
      "2000000000,T,0,A,99.99,1,10\n"  // a sell into the bid, printed before the book update
      "2000000000,D,0,B,99.99,0,2\n"
      "2000000000,D,0,B,99.98,1,2\n"
      "3000000000,D,0,B,99.98,1,3\n";
  FeatureConfig cfg;
  cfg.horizons = {seconds(1)};
  cfg.sample_trades = true;
  const FeatureTable t = run(tape, cfg);
  REQUIRE(t.rows.size() == 4);
  CHECK(t.rows.ts[1] == seconds(2).ns);
  CHECK(t.rows.mid[1] == px("100").raw);  // the book as the trade saw it, not after the delete
  CHECK(t.rows.mid[2] == px("99.995").raw);

  FeatureConfig without = cfg;
  without.sample_trades = false;
  const FeatureTable u = run(tape, without);
  CHECK(u.rows.size() == 3);
}

TEST_CASE("research.features: horizons are sorted, de-duplicated and stripped of non-positives") {
  FeatureConfig cfg;
  cfg.horizons = {seconds(10), Duration{}, seconds(1), seconds(10), Duration{-1}};
  const FeatureTable t = run(kTwoUpdates, cfg);
  REQUIRE(t.rows.horizon_ns.size() == 2);
  CHECK(t.rows.horizon_ns[0] == seconds(1).ns);
  CHECK(t.rows.horizon_ns[1] == seconds(10).ns);

  FeatureConfig empty;
  const FeatureTable d = run(kTwoUpdates, empty);
  REQUIRE(d.rows.horizon_ns.size() == 4);
  CHECK(d.rows.horizon_ns[0] == milliseconds(100).ns);
  CHECK(d.rows.horizon_ns[3] == seconds(60).ns);
  CHECK(d.coverage[3].label() == "1m");
}

TEST_CASE("research.features: bad configuration is rejected") {
  bt::CsvSource src = bt::CsvSource::from_text(kTwoUpdates);
  FeatureConfig zero_inst;
  zero_inst.instruments = 0;
  CHECK_THROWS_AS(static_cast<void>(extract_features(src, zero_inst)), std::invalid_argument);
  FeatureConfig zero_levels;
  zero_levels.imbalance_levels = 0;
  CHECK_THROWS_AS(static_cast<void>(extract_features(src, zero_levels)), std::invalid_argument);
}

TEST_CASE("research.features: the CSV leaves an unset forward mid empty") {
  FeatureConfig cfg;
  cfg.horizons = {seconds(1)};
  const FeatureTable t = run(kTwoUpdates, cfg);
  const std::string csv = t.csv();
  CHECK(csv.find("ts_ns,inst,mid,microprice,best_bid,best_ask,bid_qty,ask_qty,imbalance,spread,"
                 "mid_1s\n") == 0);
  CHECK(csv.find("1000000000,0,100,100.005,99.99,100.01,3,1,0.5,0.02,100\n") != std::string::npos);
  CHECK(csv.find("2000000000,0,100,100,99.99,100.01,1,1,0,0.02,\n") != std::string::npos);
}
