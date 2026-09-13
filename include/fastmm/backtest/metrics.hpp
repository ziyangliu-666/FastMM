#pragma once
// Metrics (8.4) computed from the result rows: equity bars (1 s by default), fills and
// orders. Every input is an exact integer (raw 1e-8 fixed point); the doubles here are
// reporting-only and never feed back into the simulation.
#include "fastmm/core/time.hpp"

#include <cstdint>

namespace fastmm::bt {

struct FillRows;
struct EquityRows;
struct OrderRows;

struct Metrics {
  // PnL in quote currency at the last bar: net == realized + unrealized - fees.
  double net_pnl = 0.0;
  double realized_pnl = 0.0;
  double unrealized_pnl = 0.0;
  double fees = 0.0;
  double final_position = 0.0;     // base units, summed over instruments
  double sharpe_bar = 0.0;         // mean / sample stdev of per-bar equity changes
  double sharpe_annualized = 0.0;  // sharpe_bar * sqrt(bars per 365-day year)
  double max_drawdown = 0.0;       // quote currency, >= 0
  double max_drawdown_pct = 0.0;   // drawdown / (initial_capital + equity peak); 0 if <= 0
  std::uint64_t fills = 0;
  std::uint64_t maker_fills = 0;
  std::uint64_t taker_fills = 0;
  std::uint64_t orders = 0;  // new orders sent
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
  std::uint64_t rejects = 0;
  double fill_ratio = 0.0;           // fills / new orders
  double spread_captured_bps = 0.0;  // mean of (mid - px) / mid for buys, (px - mid) / mid sells
  double volume_base = 0.0;
  double volume_quote = 0.0;
  double inventory_mean = 0.0;      // over bars
  double inventory_abs_mean = 0.0;  // over bars
  double inventory_max = 0.0;       // max |position| over bars
  double quote_uptime = 0.0;        // fraction of bars with both a bid and an ask resting
  std::uint64_t bars = 0;
  double duration_s = 0.0;
  // Virtual tick-to-order: engine receipt of the triggering event (T0, virtual clock) to the
  // order's arrival at the simulated venue. Deterministic; includes the order latency model.
  std::uint64_t virtual_tick_to_order_p50_ns = 0;
  std::uint64_t virtual_tick_to_order_p99_ns = 0;
  // Measured wall clock per engine step that consumed market data (0 when not measured).
  std::uint64_t wall_tick_to_order_p50_ns = 0;
  std::uint64_t wall_tick_to_order_p99_ns = 0;
};

struct MetricsInputs {
  Duration bar = seconds(1);
  double initial_capital = 0.0;  // quote currency; only used for max_drawdown_pct
  std::uint64_t rejects = 0;
  std::uint64_t wall_p50_ns = 0;
  std::uint64_t wall_p99_ns = 0;
};

[[nodiscard]] Metrics compute_metrics(const EquityRows& equity,
                                      const FillRows& fills,
                                      const OrderRows& orders,
                                      const MetricsInputs& in);

}  // namespace fastmm::bt
