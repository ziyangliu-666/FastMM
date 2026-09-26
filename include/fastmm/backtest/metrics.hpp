#pragma once
// Metrics (8.4) computed from the result rows: equity bars (1 s by default), fills and
// orders. Every input is an exact integer (raw 1e-8 fixed point); the doubles here are
// reporting-only and never feed back into the simulation.
#include "fastmm/backtest/markout.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>
#include <vector>

namespace fastmm::bt {

struct FillRows;
struct EquityRows;
struct OrderRows;

// How the net PnL of the run splits into the parts a market maker can act on:
//   net == spread_capture + mid_drift - fees_paid + rebates_received + residual
// `mid_drift` is the move of the mid from each fill to the end of the run, valued at the signed
// quantity of the fill: adverse selection plus the mark-to-market of whatever is still open.
// `residual` is whatever the average-cost ledger books differently (fixed-point rounding, a
// contract multiplier, an instrument whose book never had two sides). A large residual means the
// decomposition does not describe this run; it is printed, not hidden.
//
// A fill whose venue mid is unknown at the moment of the fill (the sweep that filled it emptied
// that side of the book) has no spread capture to attribute, so its whole
// signed_qty * (final mid - price) goes to mid_drift instead. `capture_fills` and
// `capture_notional` cover the rest, and are what spread_capture_bps divides by.
struct PnlDecomposition {
  double spread_capture = 0.0;  // sum of signed_qty * (mid at fill - fill price)
  double spread_capture_bps = 0.0;
  double mid_drift = 0.0;         // sum of signed_qty * (final mid - mid at fill)
  double fees_paid = 0.0;         // sum of the positive fees, >= 0
  double rebates_received = 0.0;  // sum of the negative fees with the sign flipped, >= 0
  double net = 0.0;               // spread_capture + mid_drift - fees_paid + rebates_received
  double residual = 0.0;          // reported net PnL - net
  double notional = 0.0;          // traded notional of every fill
  double capture_notional = 0.0;  // traded notional of the fills that had a mid at the fill
  std::uint64_t capture_fills = 0;
  std::uint64_t fills = 0;
};

// Diagnostics of how the fills were obtained, next to the markouts.
struct FillQuality {
  // Notional-weighted (mid at fill - price) for buys, (price - mid at fill) for sells, over the
  // fills that had a venue mid. This is the realised spread: what markouts start from.
  double realized_spread_bps = 0.0;
  double realized_spread_quote = 0.0;
  double at_touch_share = 0.0;       // fills at the venue's best price on our side
  double inside_touch_share = 0.0;   // fills at a better price than the touch (it improved it)
  double behind_touch_share = 0.0;   // fills at a worse price than the touch
  double through_touch_share = 0.0;  // fills at or beyond the opposite touch (aggressive)
  // Venue arrival of a new order to its first fill. Only orders that filled are counted.
  std::uint64_t time_to_fill_p50_ns = 0;
  std::uint64_t time_to_fill_p90_ns = 0;
  std::uint64_t time_to_fill_p99_ns = 0;
  // Share of new orders that got at least one fill (fill_ratio counts fills per order instead).
  double fill_rate_per_quote = 0.0;
  std::uint64_t quotes_placed = 0;
  std::uint64_t quotes_filled = 0;
  // Displayed quantity still ahead of the order when it filled; the L2Queue fill model only.
  bool queue_position_known = false;
  double queue_ahead_mean = 0.0;  // base units
  double queue_ahead_p50 = 0.0;
  double queue_ahead_p90 = 0.0;
};

struct Metrics {
  // PnL in quote currency at the last bar: net == realized + unrealized - fees.
  double net_pnl = 0.0;
  double realized_pnl = 0.0;
  double unrealized_pnl = 0.0;
  double fees = 0.0;
  double final_position = 0.0;  // base units, summed over instruments
  double sharpe_bar = 0.0;      // mean / sample stdev of per-bar equity changes
  // sharpe_bar * sqrt(bars per 365-day year); NaN for runs shorter than
  // kMinAnnualizedDurationS, where the scaling turns noise into four-digit values.
  double sharpe_annualized = 0.0;
  double max_drawdown = 0.0;  // largest fall of equity from its peak, quote currency, >= 0
  // max_drawdown / initial_capital; NaN when initial_capital <= 0.
  double max_drawdown_pct = 0.0;
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
  // Where the net PnL came from, and how good the fills were. See markout.hpp.
  PnlDecomposition decomposition;
  FillQuality fill_quality;
  std::vector<MarkoutHorizon> markouts;  // one entry per configured horizon, in order

  // The markout of one horizon, or nullptr when it was not measured.
  [[nodiscard]] const MarkoutHorizon* markout(std::int64_t horizon_ns) const noexcept;
};

// Shortest run whose Sharpe ratio is annualised: one day.
inline constexpr double kMinAnnualizedDurationS = 86400.0;

struct MetricsInputs {
  Duration bar = seconds(1);
  double initial_capital = 0.0;  // quote currency; only used for max_drawdown_pct
  std::uint64_t rejects = 0;
  std::uint64_t wall_p50_ns = 0;
  std::uint64_t wall_p99_ns = 0;
  // Venue mid of each instrument at the end of the run (raw price, indexed by instrument id).
  // The decomposition marks the leftover inventory and the mid drift against it; without it
  // only instrument 0's last equity-bar mid is available.
  std::vector<std::int64_t> final_mid;
  // Set when the venue could report a queue position (FillModel::L2Queue).
  bool queue_position_known = false;
  // Time of the last event of the run: a fill whose horizon falls after it could not be marked
  // because the data ended, which is a different thing from a one-sided book.
  std::int64_t end_ts = 0;
};

[[nodiscard]] Metrics compute_metrics(const EquityRows& equity,
                                      const FillRows& fills,
                                      const OrderRows& orders,
                                      const MetricsInputs& in);

}  // namespace fastmm::bt
