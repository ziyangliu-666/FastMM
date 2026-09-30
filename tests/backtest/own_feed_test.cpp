// The simulated feed shows the strategy's own resting orders, as a live venue's does
// ([backtest] own_orders_in_feed): recorded depth and tickers carry our quantity, and a strategy
// that reads the touch keeps an order that has become the touch instead of chasing the level
// below it.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/replay.hpp"

#include <filesystem>
#include <string>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// TGT is quoted, LDR's mid is the fair value (no fx).
constexpr const char* kConfig = R"(
[engine]
min_requote_interval_ms = 0
min_requote_ticks = 1
post_only = true
supports_replace = false

[venues.binance]
kind = "sim"

[[instruments]]
venue = "binance"
symbol = "TGT"
base = "BTC"
quote = "U"
tick = "0.01"
lot = "0.0001"

[[instruments]]
venue = "binance"
symbol = "LDR"
tick = "0.01"
lot = "0.0001"
enabled = false

[strategy]
name = "lead_mm"
[strategy.params]
target = 0
leader = 1
fx = -1
edge_min_bps = 1.2
hysteresis_bps = 0.1
skew_bps_per_unit = 0
quote_qty = 0.001
max_inventory = 0.01
improve = true
max_leader_age_ms = 1000

[risk]
max_order_qty = "1"
max_order_notional = "1000"
max_position = "1"
max_open_orders = 8
stale_md_ms = 0

[backtest]
fill_model = "l2_queue"
latency_fixed_us = 100
latency_jitter_us = 0
markout_horizons_s = ""
)";

constexpr std::int64_t kT0 = 1'790'000'000'000'000'000LL;
std::string ms(std::int64_t v) {
  return std::to_string(kT0 + v * 1'000'000);
}

// The target's spread is 10 ticks around the leader's mid 100.05: lead_mm bids one tick inside,
// at 100.01, and offers at 100.09. At 50 ms the recorded 100.00 bid goes; the next bid is 99.95. At
// 80 ms the leader updates, which makes lead_mm look again.
std::string feed() {
  std::string c = "ts_ns,type,inst,side,price,qty,seq\n";
  c += ms(1) + ",S,0,B,100.00,1,1\n" + ms(1) + ",S,0,B,99.95,1,1\n" + ms(1) + ",S,0,A,100.10,1,1\n";
  c += ms(1) + ",S,1,B,100.04,1,1\n" + ms(1) + ",S,1,A,100.06,1,1\n";
  c += ms(1) + ",B,0,B,100.00,1,2\n" + ms(1) + ",B,0,A,100.10,1,2\n";
  c += ms(50) + ",D,0,B,100.00,0,3\n";
  c += ms(80) + ",D,1,B,100.04,2,2\n";
  c += ms(200) + ",D,1,A,100.06,2,3\n";
  return c;
}

BacktestResult run(bool own_in_feed, const std::string& journal = "") {
  BacktestConfig cfg = BacktestConfig::from_config(Config::parse(kConfig));
  cfg.measure_wall_clock = false;
  cfg.transport.own_orders_in_feed = own_in_feed;
  cfg.journal_out = journal;
  CsvSource src = CsvSource::from_text(feed());
  return run_backtest(cfg, "lead_mm", &src);
}

std::size_t new_orders(const BacktestResult& r, Side side) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < r.orders.size(); ++i) {
    n += r.orders.kind[i] == kOrderKindNew && r.orders.side[i] == static_cast<std::int8_t>(side)
             ? 1U
             : 0U;
  }
  return n;
}

}  // namespace

TEST_CASE("backtest.own_feed: lead_mm keeps a bid that became the touch, as it does live") {
  const BacktestResult r = run(true);
  // The bid at 100.01 is the best bid of the feed once the recorded 100.00 goes, so lead_mm does
  // not improve on it again (lead_mm.hpp: it never improves on its own order).
  CHECK(new_orders(r, Side::Buy) == 1);
  CHECK(new_orders(r, Side::Sell) == 1);
  CHECK(r.metrics.cancels == 0);
  CHECK(r.transport.own_tickers >= 2);  // our bid and ask each made the top of book
  CHECK(r.transport.own_levels >= 2);

  // A feed without our orders (the simulator before own_orders_in_feed): the touch looks like
  // 99.95, and lead_mm moves its bid down to 99.96 at once.
  const BacktestResult without = run(false);
  CHECK(new_orders(without, Side::Buy) == 2);
  CHECK(without.metrics.cancels == 1);
  CHECK(without.transport.own_tickers == 0);
}

TEST_CASE("backtest.own_feed: a run with our orders in its feed replays to the same hash") {
  const auto path = fastmm::test::tmp_dir() / "own_feed_replay.fmj";
  std::filesystem::remove(path);
  const BacktestResult rec = run(true, path.string());
  BacktestConfig cfg = BacktestConfig::from_config(Config::parse(kConfig));
  const ReplayResult rp = replay_journal(path.string(), cfg);
  CHECK(rp.outbound_sha256 == rec.outbound_sha256);
  CHECK(rp.first_mismatch == -1);
  CHECK(rp.ok());
  // The journal says its feed showed our orders, so strip_own takes them out again.
  JournalReader reader;
  REQUIRE(reader.open(path.string()));
  CHECK(feed_shows_own(reader.header()));
  CHECK_NOTHROW(JournalSource(path.string(), true));
}
