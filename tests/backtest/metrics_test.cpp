// Analytic cases: known fills and equity series give known PnL, fees and metrics.
#include "fastmm/backtest/metrics.hpp"

#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/fee_model.hpp"
#include "fastmm/backtest/pnl.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <cmath>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

TEST_CASE("backtest.fees: maker rebate and taker fee in centi-bps are exact") {
  const FeeModel f = FeeModel::from_bps(-0.5, 3.0);
  CHECK(f.maker_cbps == -50);
  CHECK(f.taker_cbps == 300);
  // 60000 * 0.002 = 120 quote: maker -0.006, taker +0.036
  CHECK(f.fee(px("60000"), qt("0.002"), Liquidity::Maker) == Notional::from_decimal("-0.006"));
  CHECK(f.fee(px("60000"), qt("0.002"), Liquidity::Taker) == Notional::from_decimal("0.036"));
  CHECK(FeeModel::from_bps(1.25, 0).fee(px("100"), qt("1"), Liquidity::Maker) ==
        Notional::from_decimal("0.0125"));
}

TEST_CASE("backtest.pnl: average-cost ledger realizes, marks and nets fees") {
  const BacktestConfig cfg = BacktestConfig::single_instrument("X", px("0.01"), qt("0.001"));
  PnLLedger led(cfg.instruments);
  const InstrumentId id{0};
  const auto fee = Notional::from_decimal("0.01").value();
  led.on_fill(id, Side::Buy, px("100"), qt("2"), fee);
  led.on_fill(id, Side::Buy, px("103"), qt("1"), fee);  // avg (200 + 103) / 3 = 101
  CHECK(led.position(id).avg_px == px("101"));
  led.on_fill(id, Side::Sell, px("102"), qt("2"), fee);  // realize (102 - 101) * 2 = 2
  CHECK(led.realized() == Notional::from_decimal("2"));
  CHECK(led.net_position() == qt("1"));
  led.mark(id, px("99"));  // (99 - 101) * 1 = -2
  CHECK(led.unrealized() == Notional::from_decimal("-2"));
  CHECK(led.fees() == Notional::from_decimal("0.03"));
  CHECK(led.equity() == Notional::from_decimal("-0.03"));
  led.on_fill(id, Side::Sell, px("98"), qt("3"), Notional{});  // close 1 @ 98, open short 2
  CHECK(led.realized() == Notional::from_decimal("-1"));
  CHECK(led.net_position() == qt("-2"));
  CHECK(led.position(id).avg_px == px("98"));
}

TEST_CASE("backtest.metrics: hand-computed Sharpe, drawdown, inventory, uptime, spread, latency") {
  EquityRows eq;
  const std::int64_t unit = kFixedScale;
  const std::int64_t values[] = {1, 3, 2, 5, 4};  // equity per bar (realized only)
  const std::int64_t pos[] = {0, 1, -2, 1, 0};
  const std::uint8_t quoted[] = {3, 1, 3, 3, 0};
  for (int i = 0; i < 5; ++i) {
    eq.ts.push_back(seconds(i + 1).ns);
    eq.realized.push_back(values[i] * unit);
    eq.unrealized.push_back(0);
    eq.fees.push_back(0);
    eq.position.push_back(pos[i] * unit);
    eq.mid.push_back(100 * unit);
    eq.quoted.push_back(quoted[i]);
  }
  FillRows fills;
  auto add_fill = [&](std::int8_t side, const char* price, Liquidity liq) {
    fills.ts.push_back(0);
    fills.instrument.push_back(0);
    fills.side.push_back(side);
    fills.price.push_back(px(price).raw);
    fills.qty.push_back(unit);
    fills.fee.push_back(0);
    fills.cl_ord_id.push_back(1);
    fills.liquidity.push_back(static_cast<std::uint8_t>(liq));
    fills.mid.push_back(100 * unit);
  };
  add_fill(0, "99.99", Liquidity::Maker);   // bought 1 bps below mid
  add_fill(1, "100.02", Liquidity::Taker);  // sold 2 bps above mid
  OrderRows orders;
  auto add_order = [&](std::uint8_t kind, std::int64_t trigger, std::int64_t venue) {
    orders.ts.push_back(trigger);
    orders.venue_ts.push_back(venue);
    orders.trigger_ts.push_back(trigger);
    orders.cl_ord_id.push_back(1);
    orders.instrument.push_back(0);
    orders.side.push_back(0);
    orders.price.push_back(0);
    orders.qty.push_back(0);
    orders.kind.push_back(kind);
    orders.type.push_back(0);
  };
  add_order(kOrderKindNew, 1000, 1000 + 200'000);
  add_order(kOrderKindNew, 5000, 5000 + 300'000);
  add_order(kOrderKindCancel, 9000, 9000 + 250'000);
  add_order(kOrderKindReplace, 9000, 0);  // dropped: excluded from latency
  MetricsInputs in;
  in.bar = seconds(1);
  in.rejects = 2;
  const Metrics m = compute_metrics(eq, fills, orders, in);

  CHECK(m.bars == 5);
  CHECK(m.net_pnl == doctest::Approx(4.0));
  CHECK(m.duration_s == doctest::Approx(5.0));
  // per-bar changes 1, 2, -1, 3, -1: mean 0.8, sample variance 3.2
  const double sharpe = 0.8 / std::sqrt(3.2);
  CHECK(m.sharpe_bar == doctest::Approx(sharpe));
  CHECK(m.sharpe_annualized == doctest::Approx(sharpe * std::sqrt(365.0 * 86400.0)));
  CHECK(m.max_drawdown == doctest::Approx(1.0));
  CHECK(m.max_drawdown_pct == doctest::Approx(1.0 / 3.0));  // peak 3 when the first 1 fell
  CHECK(m.inventory_mean == doctest::Approx(0.0));
  CHECK(m.inventory_abs_mean == doctest::Approx(0.8));
  CHECK(m.inventory_max == doctest::Approx(2.0));
  CHECK(m.final_position == doctest::Approx(0.0));
  CHECK(m.quote_uptime == doctest::Approx(0.6));
  CHECK(m.fills == 2);
  CHECK(m.maker_fills == 1);
  CHECK(m.taker_fills == 1);
  CHECK(m.spread_captured_bps == doctest::Approx(1.5));
  CHECK(m.volume_base == doctest::Approx(2.0));
  CHECK(m.volume_quote == doctest::Approx(200.01));
  CHECK(m.orders == 2);
  CHECK(m.cancels == 1);
  CHECK(m.replaces == 1);
  CHECK(m.rejects == 2);
  CHECK(m.fill_ratio == doctest::Approx(1.0));
  // median of {200, 250, 300} us within the histogram's 6.25 % bucket width
  CHECK(m.virtual_tick_to_order_p50_ns >= 250'000);
  CHECK(m.virtual_tick_to_order_p50_ns <= 266'000);
  CHECK(m.virtual_tick_to_order_p99_ns == 300'000);

  in.initial_capital = 97.0;  // drawdown relative to capital + peak
  CHECK(compute_metrics(eq, fills, orders, in).max_drawdown_pct == doctest::Approx(0.01));
  const Metrics empty = compute_metrics(EquityRows{}, FillRows{}, OrderRows{}, MetricsInputs{});
  CHECK(empty.bars == 0);
  CHECK(empty.sharpe_bar == 0.0);
}

// A hand-made L2 tape where the fills are known in advance:
//   t=1s snapshot 99.99 x 1 / 100.01 x 1; BasicMM (1 bps half spread) joins both touches
//        behind 1 unit of displayed size with 0.5
//   t=2s a sell of 1.5 prints at 99.99 -> 1 unit ahead of us, our bid fills 0.5
//   t=3s a buy of 2 prints at 100.01  -> our ask fills 0.5
// realized = (100.01 - 99.99) * 0.5 = 0.01, maker rebate 0.5 bps on 49.995 + 50.005 = 0.005.
TEST_CASE("backtest.analytic: known fills on an L2 tape give exact PnL and fees") {
  const std::string tape =
      "ts_ns,type,inst,side,price,qty,seq\n"
      "1000000000,S,0,B,99.99,1,1\n"
      "1000000000,S,0,A,100.01,1,1\n"
      "2000000000,T,0,A,99.99,1.5,10\n"
      "3000000000,T,0,B,100.01,2,11\n";
  BacktestConfig cfg = BacktestConfig::single_instrument("X", px("0.01"), qt("0.001"));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 10'000;
  cfg.transport.order_out = sim::LatencyParams{microseconds(200), Duration{}};
  cfg.transport.ack_in = sim::LatencyParams{microseconds(200), Duration{}};
  cfg.transport.fees = FeeModel::from_bps(-0.5, 3.0);
  cfg.params = {{"half_spread_bps", "1"},
                {"skew_bps_per_unit", "0"},
                {"quote_qty", "0.5"},
                {"max_inventory", "5"},
                {"pull_on_stale_ms", "0"}};
  cfg.measure_wall_clock = false;
  CsvSource src = CsvSource::from_text(tape);
  const BacktestResult r = run_backtest<BasicMM>(cfg, &src);

  REQUIRE(r.fills.size() == 2);
  CHECK(r.fills.side[0] == 0);
  CHECK(r.fills.price[0] == px("99.99").raw);
  CHECK(r.fills.qty[0] == qt("0.5").raw);
  CHECK(r.fills.ts[0] == seconds(2).ns);
  CHECK(r.fills.side[1] == 1);
  CHECK(r.fills.price[1] == px("100.01").raw);
  CHECK(r.fills.ts[1] == seconds(3).ns);
  CHECK(r.fills.fee[0] == -249'975);  // 49.995 * -0.00005
  CHECK(r.fills.fee[1] == -250'025);  // 50.005 * -0.00005
  CHECK(r.metrics.maker_fills == 2);
  CHECK(r.metrics.realized_pnl == doctest::Approx(0.01));
  CHECK(r.metrics.fees == doctest::Approx(-0.005));
  CHECK(r.metrics.net_pnl == doctest::Approx(0.015));
  CHECK(r.metrics.final_position == 0.0);
  CHECK(r.metrics.spread_captured_bps == doctest::Approx(1.0));
  // the ledger and the engine agree to the last 1e-8
  const std::size_t last = r.equity.size() - 1;
  CHECK(r.equity.realized[last] == 1'000'000);
  CHECK(r.equity.fees[last] == -500'000);
  CHECK(r.engine.realized_pnl_raw == 1'000'000);
  CHECK(r.engine.fees_raw == -500'000);
  CHECK(r.engine.fills == 2);
  // first bar ends at 2 s with the bid filled and marked at mid 100
  REQUIRE(r.equity.size() >= 2);
  CHECK(r.equity.ts[0] == seconds(2).ns);
  CHECK(r.equity.position[0] == qt("0.5").raw);
  CHECK(r.equity.unrealized[0] == 500'000);
  // 200 us order latency, triggered by the snapshot / fills at their arrival time
  CHECK(r.metrics.virtual_tick_to_order_p50_ns >= 200'000);
  CHECK(r.metrics.virtual_tick_to_order_p50_ns <= 213'000);
  CHECK(r.orders.size() >= 3);
  CHECK(r.orders.venue_ts[0] == seconds(1).ns + microseconds(200).ns);
}
