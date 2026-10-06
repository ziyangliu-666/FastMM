#pragma once
// Fill check: how well the l2_queue fill model predicts the passive fills of a live session.
//
// The strategy is not re-run. Each order the session had resting enters the models at its venue
// ack time and leaves them at its venue end time (cancel ack, last fill, expiry; a reconciliation
// that no longer lists it; collect_own_orders, own_orders.hpp), and the journal's book and trade
// messages are applied to a mirror book and to one QueuePositionModel per conservatism value in
// venue time order (exch_ts), exactly as SimTransport does under fill_model = "l2_queue"
// (queue_apply_book, QueuePositionModel::on_trade, a BookTicker newer than the depth through
// queue_apply_touch, the trades printed since either through a TradeTape; FillCheckInputs turns
// the last two off, to measure what they contribute). Venue time matters: a venue's execution
// report reaches the session before the public trade that filled the order, so by receive time the
// trade falls after the order's end. An event without a venue time uses its receive time (counted).
// A replace follows the new id; like the simulator, the same price at no more than the leaves keeps
// the queue position.
//
// Millisecond order times (Binance transactTime, execution report T): a trade in the ack's
// millisecond counts for the order (ack_ties); one in the end's millisecond only when the order had
// a live fill in it, and only up to its last fill's trade id (end_ties; Binance's exec id is the
// public trade id). The queue ahead is the book as of the ack's venue time, before the order was in
// it; in a live session (the journal has a TSC calibration) the depth feed also shows our own
// orders, and OwnOrderStripper takes them out first.
//
// Orders that cannot rest (market, IOC, FOK) and orders whose price crossed the mirrored book at
// the ack are left out and counted. A model fill is timed by the trade that caused it, a live
// fill by the OrderFill, both in venue time.
//
// The diagnosis (FillCheckOptions, format_fill_diagnosis) says which live fills the model misses
// and what they were worth. Each order records what the market did while it rested: whether the
// opposite touch reached its price (a live venue matches it there; the model waits for a print),
// whether a public trade printed through its price, how much printed at it, and how its first live
// fill shows in the recorded trades (FillPrint). Each first fill, live and model, gets the mid
// move over `pre_window` before it and its markouts at `horizons_ns`, all against the journal's
// BookTicker mids (mid_series.hpp): s * (mid(t + h) - price) / price, s = +1 for a buy, in basis
// points. The report sets the live fills the model also has beside those it misses, and the fill
// rate (live / model) per bucket of each feature. `market` replays another journal's market data
// recorded over the same period (instruments matched by venue id and symbol) in place of the
// session's own; the session's own orders are still taken out of its depth.
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::bt {

using FillCheckEnd = OrderEnd;  // how the live order left the book

// How the first live fill shows in the recorded trades.
enum class FillPrint : std::uint8_t {
  None = 0,     // no live fill
  Missing = 1,  // no trade at its price against its side in the fill's millisecond, none through
  AtPrice = 2,  // its trade (the exec id is the public trade id on Binance) or one at its price
                // against its side in the fill's millisecond
  Through = 3,  // a trade against its side printed through its price in the fill's millisecond:
                // a sweep took it
};
[[nodiscard]] std::string_view to_string(FillPrint p) noexcept;

inline constexpr double kNoValue = std::numeric_limits<double>::quiet_NaN();

struct FillCheckOrder {
  ClientOrderId cl_ord_id;
  InstrumentId instrument;
  Side side = Side::Buy;
  Price price;
  Qty qty;
  // Displayed quantity at the price at the ack's venue time (own excluded), or a newer
  // BookTicker's touch there, less the trades printed since (queue_at_placement).
  Qty queue_ahead;
  Timestamp ack_ts;  // venue time
  Timestamp end_ts;  // live end in venue time, or the last event of the journal when still open
  FillCheckEnd end = FillCheckEnd::Open;
  Qty live_filled;
  Timestamp live_first_fill_ts;  // 0: no live fill
  // One entry per conservatism value of the run.
  std::vector<Qty> model_filled;
  std::vector<Timestamp> model_first_fill_ts;  // 0: no model fill
  // The model's queue ahead just before its first fill, and just before the first live fill
  // (the order's queue position when the venue filled it); raw -1: none.
  std::vector<Qty> model_ahead_at_fill;
  std::vector<Qty> model_ahead_at_live_fill;

  // Diagnosis. Times in venue time except cancel_sent; NaN: not measured.
  Timestamp cancel_sent;  // engine time of its first cancel or replace; invalid: none
  // Ticks behind the best price on its side at the ack (others' quantity, the touch when newer
  // than the depth); negative: it improved on it. touch_known: that side had a price.
  std::int32_t ticks_behind = 0;
  bool touch_known = false;
  Timestamp touch_crossed;   // first time while it rested the opposite touch was at or through it
  Timestamp traded_through;  // first public trade through its price against its side
  Qty printed_at_px;         // public quantity printed at its price against its side
  FillPrint live_print = FillPrint::None;
  double live_pre_move_bps = kNoValue;   // s * (mid(fill) - mid(fill - pre_window)) / price
  std::vector<double> live_markout_bps;  // per horizon
  // Per conservatism value: the first model fill came from a print through its price; its pre-fill
  // mid move; its markouts (index k * horizons + h).
  std::vector<std::uint8_t> model_through;
  std::vector<double> model_pre_move_bps;
  std::vector<double> model_markout_bps;

  [[nodiscard]] Duration resting() const noexcept { return end_ts - ack_ts; }
  // A cancel or replace had gone out before the first live fill.
  [[nodiscard]] bool live_fill_after_cancel() const noexcept {
    return cancel_sent.valid() && live_first_fill_ts.valid() && live_first_fill_ts > cancel_sent;
  }
};

// One conservatism value, over every order of FillCheckResult::orders.
struct FillCheckSummary {
  double conservatism = 0;
  std::uint64_t both = 0;        // filled live and by the model
  std::uint64_t live_only = 0;   // filled live, not by the model
  std::uint64_t model_only = 0;  // filled by the model, not live
  std::uint64_t neither = 0;
  Qty live_qty;
  Qty model_qty;
  // Median |model first fill - live first fill| over the `both` orders; -1 when there are none.
  std::int64_t median_abs_dt_ns = -1;
  [[nodiscard]] double qty_ratio() const noexcept {
    return live_qty.is_zero()
               ? 0.0
               : static_cast<double>(model_qty.raw) / static_cast<double>(live_qty.raw);
  }
};

struct FillCheckResult {
  std::vector<double> conservatism;
  std::vector<FillCheckOrder> orders;  // the orders placed in the models, in venue ack order
  std::uint64_t md_events = 0;         // book and trade messages applied
  std::uint64_t tickers = 0;           // BookTicker messages
  std::uint64_t tickers_used = 0;      // ... newer than the mirrored depth: they moved the queues
  std::uint64_t orders_sent = 0;       // OutNewOrder + OutReplace (not dropped)
  std::uint64_t rejected = 0;          // OrderReject before any ack
  std::uint64_t not_acked = 0;         // neither acknowledged nor rejected (lost, still pending)
  std::uint64_t ended_before_ack = 0;  // ended at an earlier venue time than its ack
  std::uint64_t not_resting = 0;       // acked market / IOC / FOK orders
  std::uint64_t crossed_at_ack = 0;    // price crossed the mirrored book at the ack
  std::uint64_t unknown_acks = 0;      // acks of orders the journal has no outbound copy of
  std::uint64_t model_full = 0;        // the queue model's table was full
  std::uint64_t order_recv_times = 0;  // order times taken from recv_ts (no venue time)
  std::uint64_t md_recv_times = 0;     // book and trade messages without a venue time
  std::uint64_t ack_ties = 0;          // model fills by a trade in the ack's millisecond
  std::uint64_t end_ties = 0;          // model fills by a trade in the end's millisecond
  bool ms_order_times = false;         // every venue order time is a whole millisecond
  bool own_in_depth = false;           // the depth feed includes our orders (live session)
  // Diagnosis.
  std::vector<std::int64_t> horizons_ns;  // markout horizons
  Duration pre_window;                    // mid move measured before each fill
  std::uint64_t exec_matched = 0;         // first live fills whose exec id printed in the trades
  std::string market;                     // the journal the market data came from, if not this
  std::uint64_t market_unmapped = 0;      // its market messages on instruments the session lacks
  [[nodiscard]] FillCheckSummary summary(std::size_t k) const;
};

// The model's inputs besides the depth book and the trades; both on is the l2_queue fill model.
struct FillCheckInputs {
  bool touch = true;  // a BookTicker newer than the depth moves the queues
  bool tape = true;   // trades printed since a view took their quantity from its levels
};

struct FillCheckOptions {
  FillCheckInputs inputs;
  std::vector<std::int64_t> horizons_ns{100'000'000, 1'000'000'000, 10'000'000'000};
  Duration pre_window = milliseconds(100);
  std::string market;  // journal whose market data replaces the session's own; empty: none
};

// Walks the journal once. `conservatism` values are in [0, 1] (queue_conservatism).
[[nodiscard]] FillCheckResult fill_check(JournalReader& reader,
                                         std::span<const double> conservatism,
                                         const FillCheckInputs& in = {});
[[nodiscard]] FillCheckResult fill_check(JournalReader& reader,
                                         std::span<const double> conservatism,
                                         const FillCheckOptions& opt);
// Throws std::runtime_error if a file cannot be opened.
[[nodiscard]] FillCheckResult fill_check(const std::string& path,
                                         std::span<const double> conservatism,
                                         const FillCheckInputs& in = {});
[[nodiscard]] FillCheckResult fill_check(const std::string& path,
                                         std::span<const double> conservatism,
                                         const FillCheckOptions& opt);

// The report fastmm-data fill-check prints, and the per-order CSV of --csv.
[[nodiscard]] std::string format_fill_check(const FillCheckResult& r);
[[nodiscard]] std::string fill_check_csv(const FillCheckResult& r);
// The live fills the model misses beside those it has, by outcome and by feature, for the
// conservatism value at index k.
[[nodiscard]] std::string format_fill_diagnosis(const FillCheckResult& r, std::size_t k = 0);

}  // namespace fastmm::bt
