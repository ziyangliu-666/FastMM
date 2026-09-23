// The extractor against the path that already exists: a backtest of basic_mm under the l2_queue
// fill model, on the same tape. Every fill's venue mid and every markout mid the runner recorded
// must equal the extractor's row at that timestamp, to the raw integer. A mismatch is a clock or
// a look-ahead bug in one of the two.
#include "test_support.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/fee_model.hpp"
#include "fastmm/research/extractor.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <map>
#include <string>

using namespace fastmm;
using namespace fastmm::research;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

// A tape that fills a resting quote on both sides: the touch holds one lot, the mid walks a tick
// every 200 ms, and every step prints a trade of three lots at the touch - printed before the book
// update that records it, the order BinanceDataset uses - so the quote resting there is consumed.
std::string walking_tape() {
  std::string tape = "ts_ns,type,inst,side,price,qty,seq\n";
  const auto price = [](int ticks) {
    char buf[kMaxDecimalChars];
    const Price p = Price::from_raw(px("100").raw + ticks * px("0.01").raw);
    return std::string(buf, p.to_decimal(buf));
  };
  int bid = -1;
  tape += std::to_string(seconds(1).ns) + ",S,0,B," + price(bid) + ",1,1\n";
  tape += std::to_string(seconds(1).ns) + ",S,0,A," + price(bid + 2) + ",1,1\n";
  for (int i = 1; i <= 80; ++i) {
    const std::int64_t ts = seconds(1).ns + milliseconds(200 * i).ns;
    const std::string seq = std::to_string(i + 1);
    const int step = (i / 7) % 2 == 0 ? 1 : -1;
    const bool buy_aggressor = i % 2 == 1;  // odd steps lift the ask, even steps hit the bid
    const int hit = buy_aggressor ? bid + 2 : bid;
    tape += std::to_string(ts) + ",T,0," + (buy_aggressor ? "B" : "A") + "," + price(hit) + ",3," +
            seq + "\n";
    tape += std::to_string(ts) + ",D,0,B," + price(bid) + ",0," + seq + "\n";
    tape += std::to_string(ts) + ",D,0,A," + price(bid + 2) + ",0," + seq + "\n";
    bid += step;
    tape += std::to_string(ts) + ",D,0,B," + price(bid) + ",1," + seq + "\n";
    tape += std::to_string(ts) + ",D,0,A," + price(bid + 2) + ",1," + seq + "\n";
  }
  return tape;
}

}  // namespace

TEST_CASE("research.cross_check: every fill's mid and markout mid match the extractor") {
  const std::string tape = walking_tape();
  const std::vector<Duration> horizons = {milliseconds(100), seconds(1)};

  bt::BacktestConfig cfg = bt::BacktestConfig::single_instrument("X", px("0.01"), qt("0.001"));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.fees = bt::FeeModel::from_bps(2.0, 5.0);
  cfg.markout_horizons = horizons;
  cfg.transport.queue_conservatism_bps = 10'000;
  cfg.transport.order_out = sim::LatencyParams{microseconds(200), Duration{}};
  cfg.transport.ack_in = sim::LatencyParams{microseconds(200), Duration{}};
  cfg.params = {{"half_spread_bps", "1"},
                {"skew_bps_per_unit", "0"},
                {"quote_qty", "1"},
                {"max_inventory", "50"},
                {"requote_threshold_ticks", "1"},
                {"pull_on_stale_ms", "0"}};
  cfg.measure_wall_clock = false;
  bt::CsvSource run_src = bt::CsvSource::from_text(tape);
  const bt::BacktestResult r = bt::run_backtest<BasicMM>(cfg, &run_src);
  REQUIRE(r.fills.size() > 4);
  REQUIRE(r.fills.markout_horizon_ns.size() == 2);

  // One row per trade: the timestamps a fill can land on, unambiguously.
  FeatureConfig fc;
  fc.horizons = horizons;
  fc.sample_book_updates = false;
  fc.sample_trades = true;
  bt::CsvSource feat_src = bt::CsvSource::from_text(tape);
  const FeatureTable t = extract_features(feat_src, fc);
  REQUIRE(t.rows.size() > 0);

  std::map<std::int64_t, std::size_t> by_ts;
  for (std::size_t i = 0; i < t.rows.size(); ++i) by_ts.emplace(t.rows.ts[i], i);

  std::size_t checked = 0;
  std::size_t marked = 0;
  for (std::size_t f = 0; f < r.fills.size(); ++f) {
    const auto it = by_ts.find(r.fills.ts[f]);
    REQUIRE(it != by_ts.end());
    const std::size_t i = it->second;
    CHECK(t.rows.instrument[i] == r.fills.instrument[f]);
    CHECK(t.rows.mid[i] == r.fills.mid[f]);
    CHECK(t.rows.best_bid[i] == r.fills.best_bid[f]);
    CHECK(t.rows.best_ask[i] == r.fills.best_ask[f]);
    ++checked;
    for (std::size_t j = 0; j < horizons.size(); ++j) {
      CHECK(t.rows.horizon_ns[j] == r.fills.markout_horizon_ns[j]);
      CHECK(t.rows.forward_mid[j][i] == r.fills.markout_mid[j][f]);
      if (r.fills.markout_mid[j][f] != 0) ++marked;
    }
  }
  CHECK(checked == r.fills.size());
  CHECK(marked > 0);  // the comparison is not vacuous
}
