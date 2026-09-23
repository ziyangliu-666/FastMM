// Markouts, the PnL decomposition and the fill-quality diagnostics: hand-computed fixtures for
// compute_metrics(), and a hand-made tape that proves the runner reads the mid AT the horizon.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/fee_model.hpp"
#include "fastmm/backtest/metrics.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <cmath>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

// Three fills, one instrument, two horizons (1 s and 10 s). The run's last event is at 11 s.
//
//   #  t   side qty px   mid  best bid/ask  mid @ +1 s  mid @ +10 s  fee
//   1  1 s buy  2   99   100  99 / 101      102         101          +0.1
//   2  2 s sell 1   101  100  100 / 102     103         (past 11 s)  -0.05 (a rebate)
//   3  3 s buy  1   100  100  99 / 100      (no mid)    (past 11 s)   0
struct Fixture {
  FillRows fills;
  EquityRows equity;
  OrderRows orders;
  MetricsInputs in;

  Fixture() {
    fills.markout_horizon_ns = {seconds(1).ns, seconds(10).ns};
    fills.markout_mid.resize(2);
    add(seconds(1).ns, 0, "2", "99", "100", "99", "101", "0.1", {"102", "101"}, 1, "5");
    add(seconds(2).ns, 1, "1", "101", "100", "100", "102", "-0.05", {"103", ""}, 2, "");
    add(seconds(3).ns, 0, "1", "100", "100", "99", "100", "0", {"", ""}, 3, "0");
    // One bar whose equity equals the decomposition, so the residual is exactly zero.
    equity.ts.push_back(seconds(11).ns);
    equity.realized.push_back(Notional::from_decimal("12.95")->raw);
    equity.unrealized.push_back(0);
    equity.fees.push_back(0);
    equity.position.push_back(0);
    equity.mid.push_back(px("105").raw);
    equity.quoted.push_back(3);
    for (int i = 0; i < 3; ++i) {
      orders.ts.push_back(seconds(i).ns);
      orders.venue_ts.push_back(seconds(i).ns + 500'000'000);  // 0.5 s before each fill
      orders.trigger_ts.push_back(seconds(i).ns);
      orders.cl_ord_id.push_back(static_cast<std::uint64_t>(i + 1));
      orders.instrument.push_back(0);
      orders.side.push_back(0);
      orders.price.push_back(0);
      orders.qty.push_back(0);
      orders.kind.push_back(kOrderKindNew);
      orders.type.push_back(0);
    }
    in.bar = seconds(1);
    in.final_mid = {px("105").raw};
    in.end_ts = seconds(11).ns;
    in.queue_position_known = true;
  }

  void add(std::int64_t ts,
           std::int8_t side,
           const char* qty,
           const char* price,
           const char* mid,
           const char* bid,
           const char* ask,
           const char* fee,
           std::initializer_list<const char*> markout_mid,
           std::uint64_t id,
           const char* queue_ahead) {
    fills.ts.push_back(ts);
    fills.instrument.push_back(0);
    fills.side.push_back(side);
    fills.price.push_back(px(price).raw);
    fills.qty.push_back(qt(qty).raw);
    fills.fee.push_back(Notional::from_decimal(fee)->raw);
    fills.cl_ord_id.push_back(id);
    fills.liquidity.push_back(static_cast<std::uint8_t>(Liquidity::Maker));
    fills.mid.push_back(px(mid).raw);
    fills.best_bid.push_back(px(bid).raw);
    fills.best_ask.push_back(px(ask).raw);
    fills.queue_ahead.push_back(*queue_ahead == '\0' ? -1 : qt(queue_ahead).raw);
    std::size_t j = 0;
    for (const char* m : markout_mid) fills.markout_mid[j++].push_back(*m == '\0' ? 0 : px(m).raw);
  }
};

}  // namespace

TEST_CASE("backtest.markout: hand-computed markouts, buckets and exclusions") {
  const Fixture f;
  const Metrics m = compute_metrics(f.equity, f.fills, f.orders, f.in);
  REQUIRE(m.markouts.size() == 2);

  const MarkoutHorizon& h1 = m.markouts[0];
  CHECK(h1.horizon_ns == seconds(1).ns);
  CHECK(h1.label() == "1s");
  // fill 1: +2 * (102 - 99) = 6, capture +2 * (100 - 99) = 2, notional 198
  // fill 2: -1 * (103 - 101) = -2, capture -1 * (100 - 101) = 1, notional 101
  // fill 3: the venue book had no mid 1 s later, so it is left out entirely
  CHECK(h1.total.fills == 2);
  CHECK(h1.total.markout_quote() == doctest::Approx(4.0));
  CHECK(h1.total.capture_quote() == doctest::Approx(3.0));
  CHECK(h1.total.adverse_selection_quote() == doctest::Approx(-1.0));
  CHECK(h1.total.notional() == doctest::Approx(299.0));
  CHECK(h1.total.markout_bps() == doctest::Approx(4.0 / 299.0 * 1e4));
  CHECK(h1.total.capture_bps() == doctest::Approx(3.0 / 299.0 * 1e4));
  CHECK(h1.total.adverse_selection_bps() == doctest::Approx(-1.0 / 299.0 * 1e4));
  CHECK(h1.buy.fills == 1);
  CHECK(h1.buy.markout_quote() == doctest::Approx(6.0));
  CHECK(h1.buy.capture_quote() == doctest::Approx(2.0));
  CHECK(h1.sell.fills == 1);
  CHECK(h1.sell.markout_quote() == doctest::Approx(-2.0));
  CHECK(h1.sell.capture_quote() == doctest::Approx(1.0));
  CHECK(h1.maker.fills == 2);  // every fill of the fixture is passive
  CHECK(h1.taker.fills == 0);
  REQUIRE(h1.instrument.size() == 1);
  CHECK(h1.instrument[0].markout_quote() == doctest::Approx(4.0));
  // The excluded fill is dropped, never marked at a substitute price.
  CHECK(h1.excluded_fills == 1);
  CHECK(h1.excluded_past_end == 0);
  CHECK(h1.excluded_no_mid == 1);

  const MarkoutHorizon& h10 = m.markouts[1];
  CHECK(h10.label() == "10s");
  // Only the first fill is 10 s before the end of the run.
  CHECK(h10.total.fills == 1);
  CHECK(h10.total.markout_quote() == doctest::Approx(4.0));  // +2 * (101 - 99)
  CHECK(h10.total.capture_quote() == doctest::Approx(2.0));
  CHECK(h10.total.notional() == doctest::Approx(198.0));
  CHECK(h10.excluded_fills == 2);
  CHECK(h10.excluded_past_end == 2);
  CHECK(h10.excluded_no_mid == 0);
  CHECK(m.markout(seconds(10).ns) == &h10);
  CHECK(m.markout(seconds(5).ns) == nullptr);
}

TEST_CASE("backtest.markout: the PnL decomposition adds up to the reported net PnL") {
  const Fixture f;
  const PnlDecomposition d = compute_metrics(f.equity, f.fills, f.orders, f.in).decomposition;
  // capture 2 + 1 + 0, mid drift to 105: +2 * 5 - 1 * 5 + 1 * 5 = 10
  CHECK(d.spread_capture == doctest::Approx(3.0));
  CHECK(d.mid_drift == doctest::Approx(10.0));
  CHECK(d.fees_paid == doctest::Approx(0.1));
  CHECK(d.rebates_received == doctest::Approx(0.05));
  CHECK(d.notional == doctest::Approx(399.0));  // 198 + 101 + 100
  CHECK(d.spread_capture_bps == doctest::Approx(3.0 / 399.0 * 1e4));
  CHECK(d.net == doctest::Approx(12.95));
  CHECK(d.residual == doctest::Approx(0.0));
}

TEST_CASE("backtest.markout: fill-quality diagnostics from the same fixture") {
  const Fixture f;
  const FillQuality q = compute_metrics(f.equity, f.fills, f.orders, f.in).fill_quality;
  CHECK(q.realized_spread_quote == doctest::Approx(3.0));
  CHECK(q.realized_spread_bps == doctest::Approx(3.0 / 399.0 * 1e4));
  // buy at 99 with the bid at 99 is at the touch; sell at 101 with the ask at 102 is behind it;
  // buy at 100 with the ask at 100 crossed the spread.
  CHECK(q.at_touch_share == doctest::Approx(1.0 / 3.0));
  CHECK(q.behind_touch_share == doctest::Approx(1.0 / 3.0));
  CHECK(q.through_touch_share == doctest::Approx(1.0 / 3.0));
  CHECK(q.quotes_placed == 3);
  CHECK(q.quotes_filled == 3);
  CHECK(q.fill_rate_per_quote == doctest::Approx(1.0));
  CHECK(q.time_to_fill_p50_ns == 500'000'000);
  CHECK(q.time_to_fill_p99_ns == 500'000'000);
  // Two fills carried a queue position (5 and 0 lots); the third reported "unknown".
  CHECK(q.queue_position_known);
  CHECK(q.queue_ahead_mean == doctest::Approx(2.5));
  CHECK(q.queue_ahead_p50 == doctest::Approx(5.0));

  // Without a queue-aware fill model no queue statistic is claimed.
  MetricsInputs in = f.in;
  in.queue_position_known = false;
  CHECK_FALSE(compute_metrics(f.equity, f.fills, f.orders, in).fill_quality.queue_position_known);
}

TEST_CASE("backtest.markout: no horizons and no fills leave the markout table empty") {
  Fixture f;
  f.fills.markout_horizon_ns.clear();
  f.fills.markout_mid.clear();
  CHECK(compute_metrics(f.equity, f.fills, f.orders, f.in).markouts.empty());
  const Metrics m = compute_metrics(EquityRows{}, FillRows{}, OrderRows{}, MetricsInputs{});
  CHECK(m.markouts.empty());
  CHECK(m.decomposition.net == doctest::Approx(0.0));
  CHECK(m.fill_quality.fill_rate_per_quote == doctest::Approx(0.0));
}

// A tape whose mid moves away and comes back inside the markout horizon. The markout must use
// the mid AT the horizon (101), not the mid at the fill (100) and not the 110 it passed through.
//
//   t=1 s   snapshot 99.99 x 1 / 100.01 x 1; BasicMM joins both touches behind 1 unit with 0.5
//   t=2 s   a sell of 1.5 prints at 99.99 -> our bid fills 0.5, venue mid 100
//   t=2.5 s the book jumps to 109.99 / 110.01 (mid 110)
//   t=3 s   the book comes back to 100.99 / 101.01 (mid 101) == the fill time + 1 s
//   t=4 s   one more event so the run does not end at the horizon itself
TEST_CASE("backtest.markout: the runner reads the mid at the horizon, not on the way there") {
  const std::string tape =
      "ts_ns,type,inst,side,price,qty,seq\n"
      "1000000000,S,0,B,99.99,1,1\n"
      "1000000000,S,0,A,100.01,1,1\n"
      "2000000000,T,0,A,99.99,1.5,10\n"
      "2500000000,D,0,B,99.99,0,2\n"
      "2500000000,D,0,A,100.01,0,2\n"
      "2500000000,D,0,B,109.99,1,2\n"
      "2500000000,D,0,A,110.01,1,2\n"
      "3000000000,D,0,B,109.99,0,3\n"
      "3000000000,D,0,A,110.01,0,3\n"
      "3000000000,D,0,B,100.99,1,3\n"
      "3000000000,D,0,A,101.01,1,3\n"
      "4000000000,D,0,B,100.99,1,4\n";
  BacktestConfig cfg = BacktestConfig::single_instrument("X", px("0.01"), qt("0.001"));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 10'000;
  cfg.transport.order_out = sim::LatencyParams{microseconds(200), Duration{}};
  cfg.transport.ack_in = sim::LatencyParams{microseconds(200), Duration{}};
  cfg.transport.fees = FeeModel::from_bps(10.0, 10.0);  // Binance spot VIP 0
  cfg.markout_horizons = {seconds(1), seconds(10)};
  cfg.params = {{"half_spread_bps", "1"},
                {"skew_bps_per_unit", "0"},
                {"quote_qty", "0.5"},
                {"max_inventory", "5"},
                {"pull_on_stale_ms", "0"}};
  cfg.measure_wall_clock = false;
  CsvSource src = CsvSource::from_text(tape);
  const BacktestResult r = run_backtest<BasicMM>(cfg, &src);

  REQUIRE(r.fills.size() >= 1);
  CHECK(r.fills.side[0] == 0);
  CHECK(r.fills.price[0] == px("99.99").raw);
  CHECK(r.fills.ts[0] == seconds(2).ns);
  CHECK(r.fills.mid[0] == px("100").raw);
  CHECK(r.fills.best_bid[0] == px("99.99").raw);
  CHECK(r.fills.best_ask[0] == px("100.01").raw);
  CHECK(r.fills.queue_ahead[0] >= 0);  // the L2 queue model supplies a queue position
  REQUIRE(r.fills.markout_horizon_ns.size() == 2);
  REQUIRE(r.fills.markout_mid.size() == 2);
  CHECK(r.fills.markout_mid[0][0] == px("101").raw);  // the mid 1 s later, not 110 and not 100
  CHECK(r.fills.markout_mid[1][0] == 0);              // 10 s later is past the end of the tape

  const MarkoutHorizon* h1 = r.metrics.markout(seconds(1).ns);
  REQUIRE(h1 != nullptr);
  CHECK(h1->buy.fills >= 1);
  // +0.5 * (101 - 99.99) = 0.505 against a spread capture of +0.5 * (100 - 99.99) = 0.005
  CHECK(h1->buy.markout_quote() == doctest::Approx(0.505));
  CHECK(h1->buy.capture_quote() == doctest::Approx(0.005));
  const MarkoutHorizon* h10 = r.metrics.markout(seconds(10).ns);
  REQUIRE(h10 != nullptr);
  CHECK(h10->total.fills == 0);
  CHECK(h10->excluded_past_end == r.fills.size());

  // Turning markouts off leaves the rest of the run bit for bit the same.
  BacktestConfig off = cfg;
  off.markout_horizons.clear();
  CsvSource src2 = CsvSource::from_text(tape);
  const BacktestResult r2 = run_backtest<BasicMM>(off, &src2);
  CHECK(r2.outbound_sha256 == r.outbound_sha256);
  CHECK(r2.metrics.markouts.empty());
  CHECK(r2.metrics.net_pnl == doctest::Approx(r.metrics.net_pnl));
  CHECK(r2.equity.size() == r.equity.size());
}

TEST_CASE("backtest.fees: per-venue and per-instrument schedules") {
  const Config c = Config::parse(R"(
[venues.a]
kind = "sim"
[venues.a.fees]
maker_bps = -0.5
taker_bps = 3.0
[venues.b]
kind = "sim"
[venues.b.fees]
maker_bps = 10.0
taker_bps = 10.0
[[instruments]]
venue = "a"
symbol = "AAA"
tick = "0.01"
lot = "0.001"
[[instruments]]
venue = "b"
symbol = "BBB"
tick = "0.01"
lot = "0.001"
[[instruments]]
venue = "b"
symbol = "CCC"
tick = "0.01"
lot = "0.001"
maker_bps = 2.5
taker_bps = 5.0
[strategy]
name = "basic_mm"
)");
  const BacktestConfig b = BacktestConfig::from_config(c);
  const FeeModel& f = b.transport.fees;
  CHECK(f.schedule(InstrumentId{0}).maker_cbps == -50);   // venue a
  CHECK(f.schedule(InstrumentId{1}).maker_cbps == 1000);  // venue b
  CHECK(f.schedule(InstrumentId{2}).maker_cbps == 250);   // per-instrument override
  CHECK(f.schedule(InstrumentId{2}).taker_cbps == 500);
  CHECK_FALSE(f.is_uniform());
  // 100 quote of notional: a rebate of 0.005 on venue a, a fee of 0.1 on venue b.
  CHECK(f.fee(InstrumentId{0}, px("100"), qt("1"), Liquidity::Maker) ==
        Notional::from_decimal("-0.005"));
  CHECK(f.fee(InstrumentId{1}, px("100"), qt("1"), Liquidity::Maker) ==
        Notional::from_decimal("0.1"));
  CHECK(f.fee(InstrumentId{2}, px("100"), qt("1"), Liquidity::Taker) ==
        Notional::from_decimal("0.05"));
  // An instrument nobody configured falls back to the default schedule (venue 0).
  CHECK(f.schedule(InstrumentId{7}).maker_cbps == -50);
  CHECK(FeeModel::from_bps(1.0, 2.0).is_uniform());

  // The effective TOML round-trips the per-instrument rates, so a journal replays with them.
  const Config back = Config::parse(c.effective_toml());
  REQUIRE(back.instruments.size() == 3);
  CHECK_FALSE(back.instruments[0].maker_bps.has_value());
  REQUIRE(back.instruments[2].maker_bps.has_value());
  CHECK(*back.instruments[2].maker_bps == doctest::Approx(2.5));
  CHECK(*back.instruments[2].taker_bps == doctest::Approx(5.0));
}

TEST_CASE("backtest.config: markout_horizons_s parses seconds and rejects nonsense") {
  const auto with = [](const char* backtest_section) {
    return BacktestConfig::from_config(
        Config::parse(std::string("[venues.sim]\nkind = \"sim\"\n[[instruments]]\n"
                                  "venue = \"sim\"\nsymbol = \"X\"\ntick = \"0.01\"\n"
                                  "lot = \"0.001\"\n") +
                      backtest_section));
  };
  CHECK(with("").markout_horizons == std::vector<Duration>{seconds(1), seconds(10), seconds(60)});
  const auto h = with("[backtest]\nmarkout_horizons_s = \"10, 0.5,1\"\n").markout_horizons;
  REQUIRE(h.size() == 3);  // sorted ascending and de-duplicated
  CHECK(h[0] == milliseconds(500));
  CHECK(h[1] == seconds(1));
  CHECK(h[2] == seconds(10));
  CHECK(with("[backtest]\nmarkout_horizons_s = \"\"\n").markout_horizons.empty());
  CHECK(with("[backtest]\nmarkout_horizons_s = \"[1, 2]\"\n").markout_horizons.size() == 2);
  CHECK_THROWS_AS(static_cast<void>(with("[backtest]\nmarkout_horizons_s = \"0\"\n")), ConfigError);
  CHECK_THROWS_AS(static_cast<void>(with("[backtest]\nmarkout_horizons_s = \"-1\"\n")),
                  ConfigError);
  CHECK_THROWS_AS(static_cast<void>(with("[backtest]\nmarkout_horizons_s = \"soon\"\n")),
                  ConfigError);
}
