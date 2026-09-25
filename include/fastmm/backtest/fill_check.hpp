#pragma once
// Fill check: how well the l2_queue fill model predicts the passive fills of a live session.
//
// The strategy is not re-run. The journal is walked once in recorded order; its market data is
// applied to a mirror book and to one QueuePositionModel per conservatism value exactly as
// SimTransport does under fill_model = "l2_queue" (sim::queue_apply_book, QueuePositionModel::
// on_trade). Each order the session had resting enters the models when the venue acknowledged it
// (OrderAck), behind the displayed quantity at its price at that moment, and leaves them when the
// live order ended: cancel ack, last fill, expiry, or a reconciliation that no longer lists it.
// A replace follows the new id; like the simulator, the same price at no more than the leaves
// keeps the queue position.
//
// Orders that cannot rest (market, IOC, FOK) and orders whose price crossed the mirrored book at
// the ack are left out and counted. Times are the journal's receive times (recv_ts): a model fill
// is timed by the trade that caused it, a live fill by the OrderFill.
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

// How the live order left the book.
enum class FillCheckEnd : std::uint8_t {
  Open = 0,      // still resting when the journal ends
  Canceled = 1,  // cancel ack
  Filled = 2,    // last fill
  Expired = 3,
  Replaced = 4,   // cancel ack of a replaced order; the new id is its own row
  Reconciled = 5  // missing from a reconciliation snapshot
};
[[nodiscard]] std::string_view to_string(FillCheckEnd e) noexcept;

struct FillCheckOrder {
  ClientOrderId cl_ord_id;
  InstrumentId instrument;
  Side side = Side::Buy;
  Price price;
  Qty qty;
  Qty queue_ahead;  // displayed quantity at the price when the ack arrived
  Timestamp ack_ts;
  Timestamp end_ts;  // live end, or the last event of the journal when still open
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
  std::vector<FillCheckOrder> orders;  // the orders placed in the models, in ack order
  std::uint64_t md_events = 0;         // book and trade messages applied
  std::uint64_t orders_sent = 0;       // OutNewOrder + OutReplace (not dropped)
  std::uint64_t rejected = 0;          // OrderReject before any ack
  std::uint64_t not_acked = 0;         // neither acknowledged nor rejected (lost, still pending)
  std::uint64_t ended_before_ack = 0;  // filled or cancelled before the ack arrived
  std::uint64_t not_resting = 0;       // acked market / IOC / FOK orders
  std::uint64_t crossed_at_ack = 0;    // price crossed the mirrored book when the ack arrived
  std::uint64_t unknown_acks = 0;      // acks of orders the journal has no outbound copy of
  std::uint64_t model_full = 0;        // the queue model's table was full
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
