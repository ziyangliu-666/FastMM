#pragma once
// Fill check: how well the l2_queue fill model predicts the passive fills of a live session.
//
// The strategy is not re-run. Each order the session had resting enters the models at its venue
// ack time and leaves them at its venue end time (cancel ack, last fill, expiry; a reconciliation
// that no longer lists it; collect_own_orders, own_orders.hpp), and the journal's book and trade
// messages are applied to a mirror book and to one QueuePositionModel per conservatism value in
// venue time order (exch_ts), exactly as SimTransport does under fill_model = "l2_queue"
// (queue_apply_book, QueuePositionModel::on_trade, and a BookTicker newer than the depth through
// queue_apply_touch). Venue time matters: a venue's execution report reaches the session before
// the public trade that filled the order, so by receive time the trade falls after the order's
// end. An event without a venue time uses its receive time (counted).
// A replace follows the new id; like the simulator, the same price at no more than the leaves keeps
// the queue position.
//
// Millisecond order times (Binance transactTime, execution report T): a trade in the ack's or the
// end's millisecond counts for the order (ack_ties, end_ties), except a trade after the last live
// fill's trade id. The queue ahead is the book as of the ack's venue time, before the order was in
// it; in a live session (the journal has a TSC calibration) the depth feed also shows our own
// orders, and OwnOrderStripper takes them out first.
//
// Orders that cannot rest (market, IOC, FOK) and orders whose price crossed the mirrored book at
// the ack are left out and counted. A model fill is timed by the trade that caused it, a live
// fill by the OrderFill, both in venue time.
#include "fastmm/backtest/own_orders.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fastmm::bt {

using FillCheckEnd = OrderEnd;  // how the live order left the book

struct FillCheckOrder {
  ClientOrderId cl_ord_id;
  InstrumentId instrument;
  Side side = Side::Buy;
  Price price;
  Qty qty;
  // Displayed quantity at the price at the ack's venue time (own excluded), capped by a newer
  // BookTicker's touch (queue_at_placement).
  Qty queue_ahead;
  Timestamp ack_ts;  // venue time
  Timestamp end_ts;  // live end in venue time, or the last event of the journal when still open
  FillCheckEnd end = FillCheckEnd::Open;
  Qty live_filled;
  Timestamp live_first_fill_ts;  // 0: no live fill
  // One entry per conservatism value of the run.
  std::vector<Qty> model_filled;
  std::vector<Timestamp> model_first_fill_ts;  // 0: no model fill
  [[nodiscard]] Duration resting() const noexcept { return end_ts - ack_ts; }
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
  [[nodiscard]] FillCheckSummary summary(std::size_t k) const;
};

// Walks the journal once. `conservatism` values are in [0, 1] (queue_conservatism).
[[nodiscard]] FillCheckResult fill_check(JournalReader& reader,
                                         std::span<const double> conservatism);
// Throws std::runtime_error if the file cannot be opened.
[[nodiscard]] FillCheckResult fill_check(const std::string& path,
                                         std::span<const double> conservatism);

// The report fastmm-data fill-check prints, and the per-order CSV of --csv.
[[nodiscard]] std::string format_fill_check(const FillCheckResult& r);
[[nodiscard]] std::string fill_check_csv(const FillCheckResult& r);

}  // namespace fastmm::bt
