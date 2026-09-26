#pragma once
// Order management (5.8): a Pool<Order> + OpenHashMap<ClientOrderId, Handle> + a ring of
// recently terminal ids used to classify late venue messages.
//
// State machine (terminal states free the slot and go to the recently-terminal ring):
//   submit           -> PendingNew
//   ack              PendingNew -> Live; PendingReplace -> Live/PartiallyFilled (replace ok)
//   reject           PendingNew -> Rejected; PendingReplace -> back to previous working state
//   cancel request   Live/PartiallyFilled -> PendingCancel
//   cancel ack       any open -> Canceled (unsolicited if we did not ask); Filled when the
//                    venue's cum_qty covers the order
//   cancel reject    PendingCancel -> back; >3 rejects -> ReconcileNeeded;
//                    VenueUnknownOrder -> Canceled
//   fill             cum >= qty -> Filled, else PartiallyFilled (pending states keep pending)
//   expired          any open -> Expired; Filled when the venue's cum_qty covers the order
// A cum_qty a venue reports that no fill message covered (a cancel ack, an expiry or a
// reconciliation snapshot) becomes OmsUpdate::missed_qty for the engine to book. That booking is an
// estimate - the order's own price, no fee - so it is remembered here until an execution from the
// venue's trade history (OrderFillMsg::kReplayed) names it: the execution then corrects the price
// and the fee (OmsUpdate::corrected_qty) instead of the quantity being counted twice.
// Races: fill after cancel ack -> LateFill (position still updated from the terminal record's
// instrument and side); cancel-reject after fill -> ignored; ack for unknown id -> CancelUnknown
// (never leave an unknown live order); ack for an order reconciliation marked cancelled ->
// CancelUnknown; duplicate exec_id -> Duplicate; duplicate ack -> Ignored (also for the original
// id while a replace to a new id is pending: only the new id's ack completes the replace).
// Reconciliation (per venue): a cancel or replace still in flight stays pending; orders sent after
// the snapshot was requested (above the Begin's sent watermark) are not marked cancelled.
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/pool.hpp"
#include "fastmm/core/containers/recent_map.hpp"
#include "fastmm/core/containers/ring_buffer.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/result.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace fastmm {

inline constexpr std::size_t kMaxOpenOrders = 4096;
inline constexpr std::size_t kRecentlyTerminal = 4096;
// Fills booked from a cumulative quantity and never named by an execution. One entry per order, so
// this is the number of orders that can be waiting for an execution-history replay at once; a
// private-stream outage covers a handful, and the oldest entry is dropped (and counted) past that.
inline constexpr std::size_t kMaxSyntheticFills = 64;
// Client order ids per session: the wire form is 48 bits, epoch(16) << 32 | seq(32).
inline constexpr std::uint64_t kMaxSeq = 0xFFFF'FFFFULL;

enum class OmsAction : std::uint8_t {
  None = 0,
  Ignored,          // message did not apply (duplicate ack, late cancel-reject, ...)
  Duplicate,        // duplicate exec_id
  CancelUnknown,    // ack/fill for an id we do not know: engine must cancel it
  LateFill,         // fill for a recently terminal order: update position only
  UnknownFill,      // fill for a completely unknown id: update position, alert
  ReconcileNeeded,  // too many cancel rejects
};

struct OmsUpdate {
  Order order{};  // snapshot after the transition; for a recently terminal id only the terminal
                  // record's fields (id, instrument, side, state, cum_qty); empty if unknown
  Handle<Order> handle{};  // invalid once terminal (slot freed) or unknown
  OrderState prev = OrderState::PendingNew;
  OmsAction action = OmsAction::None;
  bool known = false;     // the id mapped to an order (open or recently terminal)
  bool changed = false;   // state or quantities changed
  bool terminal = false;  // the order reached a terminal state in this update
  Qty fill_qty{};         // for fills
  Price fill_px{};
  // Quantity the venue reports as filled that we never saw a fill message for (a private-stream
  // outage covered by a cancel ack, an expiry or a reconciliation snapshot). The engine books it
  // as a synthetic fill at the order's own price; see Engine::book_missed_fill.
  Qty missed_qty{};
  // A cancel-replace to a new id completed: the order carries on under order.cl_ord_id and this
  // one is gone. It never produces an update of its own (the OMS renames the record in place), so
  // anything that tracks orders by id closes it here.
  ClientOrderId replaced_cl_ord_id{};
  // Quantity that was still working when a reconciliation snapshot failed to mention the order.
  // The venue does not say whether it was cancelled or filled, so nothing can be booked; it is
  // reported so the engine can say the position may be wrong instead of assuming it is not.
  Qty unresolved_qty{};
  // Part of a replayed execution (OrderFillMsg::kReplayed) whose quantity a synthetic fill already
  // put in the position: the execution names what it was, so its price and its fee replace the
  // estimate rather than the quantity being counted twice. `synthetic_px` is the price the estimate
  // used. See Engine::on_fill and PositionTracker::correct_fill.
  Qty corrected_qty{};
  Price synthetic_px{};
  // The order's times (Oms::times), and the pool slot it had: set on every update of an open
  // order, the terminal one included (`handle` is invalid then, the slot already free).
  OrderTimes times{};
  Handle<Order> slot{};
};

struct OmsStats {
  std::uint32_t open = 0;
  std::uint64_t submitted = 0;
  std::uint64_t acked = 0;
  std::uint64_t rejected = 0;
  std::uint64_t canceled = 0;
  std::uint64_t filled = 0;
  std::uint64_t fills = 0;
  std::uint64_t expired = 0;
  std::uint64_t unknown_ids = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t late_fills = 0;
  std::uint64_t unsolicited_cancels = 0;
  std::uint64_t missed_fills = 0;  // cum_qty jumps a fill message never reported
  // Missed fills a replayed execution later named, so their price and fee stopped being estimates.
  std::uint64_t corrected_fills = 0;
  // Missed fills whose entry was evicted before an execution named them: their price and fee stay
  // estimates, and an execution for them would now be counted twice.
  std::uint64_t synthetic_forgotten = 0;
  std::uint64_t ack_timeouts = 0;  // PendingNew orders swept by sweep_pending()
  // Orders a reconciliation snapshot did not report that still had working quantity: the venue
  // ended them and never said how, so the position may be short by up to that much.
  std::uint64_t reconcile_unresolved = 0;
};

enum class OrderClass : std::uint8_t { Open, RecentlyTerminal, Unknown };

class Oms {
 public:
  // `max_seq` is the highest client order id sequence this session may issue; only tests lower it
  // from kMaxSeq (reaching it takes 4.3 billion orders).
  explicit Oms(std::uint16_t session_epoch = 1, std::uint64_t max_seq = kMaxSeq) noexcept
      : epoch_(session_epoch), max_seq_(max_seq) {
    for (auto& per_inst : best_own_) per_inst[0] = per_inst[1] = Price{};
    for (auto& per_inst : open_qty_) per_inst[0] = per_inst[1] = Qty{};
  }
  Oms(const Oms&) = delete;
  Oms& operator=(const Oms&) = delete;

  [[nodiscard]] std::uint16_t session_epoch() const noexcept { return epoch_; }
  // An invalid id once the session's 32-bit sequence is used up: the wire form carries 48 bits
  // (epoch 16 + seq 32), so reusing a sequence would repeat a ClientOrderId within the session and
  // let a late venue message match the wrong order. The engine turns that into a kill switch.
  [[nodiscard]] ClientOrderId next_cl_ord_id() noexcept {
    if (FASTMM_UNLIKELY(seq_ >= max_seq_)) return ClientOrderId{};
    return make_cl_ord_id(epoch_, static_cast<std::uint32_t>(++seq_));
  }
  // Client order ids left in this session.
  [[nodiscard]] std::uint64_t ids_left() const noexcept { return max_seq_ - seq_; }
  [[nodiscard]] const OmsStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::uint32_t open_count() const noexcept { return stats_.open; }
  [[nodiscard]] std::uint32_t open_count(InstrumentId id) const noexcept {
    return open_per_inst_[id.value];
  }
  // Sum of leaves of open orders on (instrument, side): "in-flight same-side exposure".
  [[nodiscard]] Qty open_qty(InstrumentId id, Side s) const noexcept {
    return open_qty_[id.value][static_cast<std::size_t>(s)];
  }
  // Best price among our own open orders on (instrument, side); Price{} if none. For STP.
  [[nodiscard]] Price best_own_px(InstrumentId id, Side s) const noexcept {
    return best_own_[id.value][static_cast<std::size_t>(s)];
  }

  // ---- outbound ---------------------------------------------------------------------------

  Result<Handle<Order>, RejectReason> submit(const NewOrderRequest& req,
                                             ClientOrderId id,
                                             Timestamp now) noexcept {
    if (FASTMM_UNLIKELY(req.instrument.value >= kMaxInstruments))
      return fail(RejectReason::InstrumentDisabled);
    if (FASTMM_UNLIKELY(by_id_.contains(id))) return fail(RejectReason::DuplicateId);
    const Handle<Order> h = pool_.allocate();
    if (FASTMM_UNLIKELY(!h.valid())) return fail(RejectReason::PoolExhausted);
    if (FASTMM_UNLIKELY(by_id_.insert(id, h).first == nullptr)) {
      pool_.free(h);
      return fail(RejectReason::PoolExhausted);
    }
    live_pos_[h.idx] = static_cast<std::uint32_t>(live_.size());
    static_cast<void>(live_.push_back(h.idx));  // cannot fail: one entry per pool slot
    Order& o = pool_.get(h);
    o = Order{};
    o.cl_ord_id = id;
    o.instrument = req.instrument;
    o.venue = req.venue;
    o.side = req.side;
    o.type = req.type;
    o.tif = req.tif;
    o.flags = static_cast<std::uint8_t>((req.post_only ? Order::kPostOnly : 0) |
                                        (req.reduce_only ? Order::kReduceOnly : 0));
    o.state = OrderState::PendingNew;
    o.price = req.price;
    o.qty = req.qty;
    o.created = now;
    o.user_tag = req.user_tag;
    times_[h.idx] = TimesSlot{OrderTimes{now, {}, {}}, {}};
    ++stats_.submitted;
    ++stats_.open;
    ++open_per_inst_[o.instrument.value];
    open_qty_[o.instrument.value][static_cast<std::size_t>(o.side)] += o.qty;
    Price& best = best_own_[o.instrument.value][static_cast<std::size_t>(o.side)];
    if (best.is_zero() || better(o.side, o.price, best)) best = o.price;
    return h;
  }

  Result<void, RejectReason> request_cancel(Handle<Order> h) noexcept {
    if (!pool_.is_live(h)) return fail(RejectReason::UnknownOrder);
    Order& o = pool_.get(h);
    if (!o.is_working()) return fail(RejectReason::InvalidState);
    o.state = OrderState::PendingCancel;
    return {};
  }

  // Cancel of an order that is still waiting for its ack (the ack-timeout sweep only). A venue
  // that never saw the order answers VenueUnknownOrder, and on_cancel_reject ends it.
  Result<void, RejectReason> request_cancel_unacked(Handle<Order> h) noexcept {
    if (!pool_.is_live(h)) return fail(RejectReason::UnknownOrder);
    Order& o = pool_.get(h);
    if (o.state != OrderState::PendingNew) return fail(RejectReason::InvalidState);
    o.state = OrderState::PendingCancel;
    return {};
  }

  // new_id may equal the current id for venues that amend in place. `now`: the replace's send
  // time, which becomes OrderTimes::sent once it is acknowledged.
  Result<void, RejectReason> request_replace(
      Handle<Order> h, ClientOrderId new_id, Price px, Qty qty, Timestamp now = {}) noexcept {
    if (!pool_.is_live(h)) return fail(RejectReason::UnknownOrder);
    Order& o = pool_.get(h);
    if (!o.is_working()) return fail(RejectReason::InvalidState);
    if (new_id != o.cl_ord_id) {
      if (by_id_.contains(new_id)) return fail(RejectReason::DuplicateId);
      if (by_id_.insert(new_id, h).first == nullptr) return fail(RejectReason::PoolExhausted);
    }
    o.pending_cl_ord_id = new_id;
    o.pending_price = px;
    o.pending_qty = qty;
    o.state = OrderState::PendingReplace;
    times_[h.idx].replace_sent = now;
    return {};
  }

  // ---- inbound ----------------------------------------------------------------------------

  OmsUpdate on_ack(const OrderAckMsg& m) noexcept {
    OmsUpdate u;
    Handle<Order> h = lookup(m.cl_ord_id, u);
    if (!h.valid()) {
      if (!u.known) {
        u.action = OmsAction::CancelUnknown;  // a live order we do not know: cancel it
        ++stats_.unknown_ids;
      } else if (u.order.has(Order::kReconciled)) {
        // Reconciliation took it for gone, yet the venue has it (the snapshot missed an order
        // still in flight). Nothing tracks it any more: cancel it rather than leave it live.
        u.action = OmsAction::CancelUnknown;
      } else {
        u.action = OmsAction::Ignored;  // late ack for a terminal order
      }
      return u;
    }
    Order& o = pool_.get(h);
    u.prev = o.state;
    switch (o.state) {
      case OrderState::PendingNew:
        o.state = OrderState::Live;
        o.venue_order_id = m.venue_order_id;
        ++stats_.acked;
        u.changed = true;
        times_[h.idx].t.venue_ack = m.hdr.exch_ts;
        times_[h.idx].t.local_ack = m.hdr.recv_ts;
        break;
      case OrderState::PendingReplace:
        // Only the replacement's ack completes it (the same id for an in-place amend). An ack that
        // repeats the current id while a replace to a new id is in flight is a duplicate of the
        // original ack (Binance sends one from the WS API response and one from the user stream):
        // applying it would take the replace for confirmed, leave the new id mapped after the order
        // ends and let the new leg's ack read a freed slot while that order stays live untracked.
        if (m.cl_ord_id == o.pending_cl_ord_id) {
          const ClientOrderId old_id = o.cl_ord_id;
          const bool rekey = o.pending_cl_ord_id != o.cl_ord_id;
          // An in-place amend keeps the venue's order even when it is given a new client id
          // (Binance Spot keepPriority), so the fills booked against it stay.
          const bool new_venue_order = rekey && (m.flags & OrderAckMsg::kAmendedInPlace) == 0;
          apply_replace(o, rekey, new_venue_order);
          if (o.cl_ord_id != old_id) u.replaced_cl_ord_id = old_id;
          o.venue_order_id = m.venue_order_id;
          u.changed = true;
          TimesSlot& t = times_[h.idx];
          t.t = OrderTimes{t.replace_sent, m.hdr.exch_ts, m.hdr.recv_ts};
        } else {
          u.action = OmsAction::Ignored;
        }
        break;
      default:
        u.action = OmsAction::Ignored;  // duplicate ack
        // A venue that answers twice (API response and user stream) may put its time on either.
        if (m.cl_ord_id == o.cl_ord_id && !times_[h.idx].t.venue_ack.valid())
          times_[h.idx].t.venue_ack = m.hdr.exch_ts;
        break;
    }
    finish_update(u, h, o);
    return u;
  }

  OmsUpdate on_reject(const OrderRejectMsg& m) noexcept {
    OmsUpdate u;
    Handle<Order> h = lookup(m.cl_ord_id, u);
    if (!h.valid()) {
      u.action = OmsAction::Ignored;
      return u;
    }
    Order& o = pool_.get(h);
    u.prev = o.state;
    if (o.state == OrderState::PendingNew) {
      o.reject_reason = m.reason;
      ++stats_.rejected;
      terminate(u, h, o, OrderState::Rejected);
      return u;
    }
    if (o.state == OrderState::PendingReplace && m.cl_ord_id == o.pending_cl_ord_id) {
      // Replace failed. If the venue already cancelled the original (cancel-then-new
      // style replace) the order is gone; otherwise it keeps working unchanged.
      if (o.has(Order::kReplaceOldCanceled)) {
        clear_pending(o);
        terminate(u, h, o, OrderState::Canceled);
        return u;
      }
      clear_pending(o);
      o.state = o.cum_qty.is_zero() ? OrderState::Live : OrderState::PartiallyFilled;
      u.changed = true;
      finish_update(u, h, o);
      return u;
    }
    u.action = OmsAction::Ignored;
    finish_update(u, h, o);
    return u;
  }

  OmsUpdate on_cancel_ack(const OrderCancelAckMsg& m) noexcept {
    OmsUpdate u;
    Handle<Order> h = lookup(m.cl_ord_id, u);
    if (!h.valid()) {
      u.action = OmsAction::Ignored;
      return u;
    }
    Order& o = pool_.get(h);
    u.prev = o.state;
    absorb_cum(o, m.cum_qty, u);
    if (o.state == OrderState::PendingReplace && m.cl_ord_id == o.cl_ord_id) {
      // Old leg of a cancel-then-new replace: wait for the new leg's ack/reject.
      o.flags |= Order::kReplaceOldCanceled;
      finish_update(u, h, o);
      return u;
    }
    // The cancel raced a fill that finished the order: it is Filled, not Canceled.
    if (o.cum_qty >= o.qty) {
      ++stats_.filled;
      terminate(u, h, o, OrderState::Filled);
      return u;
    }
    if (o.state != OrderState::PendingCancel) {
      o.flags |= Order::kUnsolicitedCancel;
      ++stats_.unsolicited_cancels;
    }
    terminate(u, h, o, OrderState::Canceled);
    return u;
  }

  OmsUpdate on_cancel_reject(const OrderCancelRejectMsg& m) noexcept {
    OmsUpdate u;
    Handle<Order> h = lookup(m.cl_ord_id, u);
    if (!h.valid()) {
      u.action = OmsAction::Ignored;  // e.g. already filled: ignore
      return u;
    }
    Order& o = pool_.get(h);
    u.prev = o.state;
    if (o.state != OrderState::PendingCancel) {
      u.action = OmsAction::Ignored;
      finish_update(u, h, o);
      return u;
    }
    if (m.reason == RejectReason::VenueUnknownOrder) {
      // The venue has no such order: it is gone (filled/cancelled elsewhere).
      terminate(u, h, o, OrderState::Canceled);
      return u;
    }
    o.state = o.cum_qty.is_zero() ? OrderState::Live : OrderState::PartiallyFilled;
    o.cancel_attempts = static_cast<std::uint8_t>(o.cancel_attempts + 1);
    if (o.cancel_attempts > 3) u.action = OmsAction::ReconcileNeeded;
    u.changed = true;
    finish_update(u, h, o);
    return u;
  }

  OmsUpdate on_fill(const OrderFillMsg& m) noexcept {
    OmsUpdate u;
    u.fill_qty = m.qty;
    u.fill_px = m.price;
    if (!m.exec_id.empty() && !remember_exec(m)) {
      u.action = OmsAction::Duplicate;
      ++stats_.duplicates;
      return u;
    }
    // A replayed execution may be one the venue already counted into a cum_qty the engine booked as
    // a synthetic fill. Taking that quantity out of the pool here is what keeps it from being
    // booked twice, whether the order is still open, recently terminal or gone.
    if (FASTMM_UNLIKELY((m.flags & OrderFillMsg::kReplayed) != 0))
      take_synthetic(m.cl_ord_id, m.qty, u);
    Handle<Order> h = lookup(m.cl_ord_id, u);
    ++stats_.fills;
    if (!h.valid()) {
      if (u.known) {
        u.action = OmsAction::LateFill;
        ++stats_.late_fills;
      } else {
        u.action = OmsAction::UnknownFill;
        ++stats_.unknown_ids;
      }
      return u;
    }
    Order& o = pool_.get(h);
    u.prev = o.state;
    const Qty before = o.cum_qty;
    // Only the part the position does not already hold moves the order forward: a replayed
    // execution carries no cumulative quantity of its own.
    const Qty fresh = m.qty - u.corrected_qty;
    Qty cum = m.cum_qty.is_positive() ? m.cum_qty : o.cum_qty + fresh;
    if (cum < before) cum = before;  // out-of-order cum: never go backwards
    if (cum > o.qty) cum = o.qty;
    open_qty_[o.instrument.value][static_cast<std::size_t>(o.side)] -= (cum - before);
    o.cum_qty = cum;
    if (o.venue_order_id.empty()) o.venue_order_id = m.venue_order_id;
    u.changed = true;
    if (o.cum_qty >= o.qty) {
      ++stats_.filled;
      terminate(u, h, o, OrderState::Filled);
      return u;
    }
    if (o.state == OrderState::Live || o.state == OrderState::PendingNew)
      o.state = OrderState::PartiallyFilled;
    finish_update(u, h, o);
    return u;
  }

  OmsUpdate on_expired(const OrderExpiredMsg& m) noexcept {
    OmsUpdate u;
    Handle<Order> h = lookup(m.cl_ord_id, u);
    if (!h.valid()) {
      u.action = OmsAction::Ignored;
      return u;
    }
    Order& o = pool_.get(h);
    u.prev = o.state;
    absorb_cum(o, m.cum_qty, u);
    if (o.cum_qty >= o.qty) {  // an IOC that filled completely is Filled, not Expired
      ++stats_.filled;
      terminate(u, h, o, OrderState::Filled);
      return u;
    }
    ++stats_.expired;
    terminate(u, h, o, OrderState::Expired);
    return u;
  }

  // ---- reconciliation ---------------------------------------------------------------------

  // Starts reconciling `venue`'s orders (an invalid venue: every venue). `sent_watermark` is the
  // highest id the venue had sent when it requested the snapshot: orders above it cannot be in the
  // snapshot, and reconcile_end() leaves them alone. Without it every unreported order ends.
  void reconcile_begin(VenueId venue = VenueId::invalid(),
                       std::optional<ClientOrderId> sent_watermark = std::nullopt) noexcept {
    ReconcileScope& sc = recon_scope_[venue.value];
    sc.bounded = sent_watermark.has_value();
    sc.watermark = sent_watermark.value_or(ClientOrderId{});
    pool_.for_each([&](Handle<Order>, Order& o) {
      if (in_scope(o, venue)) o.flags &= static_cast<std::uint8_t>(~Order::kSeenInReconcile);
    });
  }
  // Venue reports an open order. Unknown -> CancelUnknown (engine cancels by venue id).
  OmsUpdate reconcile_open_order(const ReconcileMsg& m) noexcept {
    OmsUpdate u;
    Handle<Order> h = lookup(m.cl_ord_id, u);
    if (!h.valid()) {
      u.action = OmsAction::CancelUnknown;
      ++stats_.unknown_ids;
      return u;
    }
    Order& o = pool_.get(h);
    u.prev = o.state;
    o.flags |= Order::kSeenInReconcile | Order::kReconciled;
    if (!o.venue_order_id.empty() || !m.venue_order_id.empty()) o.venue_order_id = m.venue_order_id;
    absorb_cum(o, m.cum_qty, u);
    // The venue has it, so a new order was accepted. A cancel or replace is left pending: its ack
    // or reject is still on the way and settles the order (clearing it made that cancel ack look
    // unsolicited while the quote manager kept the order as a working quote).
    if (o.state == OrderState::PendingNew) {
      o.state = o.cum_qty.is_zero() ? OrderState::Live : OrderState::PartiallyFilled;
      u.changed = true;
    }
    finish_update(u, h, o);
    return u;
  }
  // Every open order of `venue` (invalid: every venue) the snapshot did not report is marked
  // Canceled, except orders sent after the snapshot was requested (see reconcile_begin).
  // F(const OmsUpdate&) runs after each order ends and outside the pool iteration, so it may place
  // or cancel orders.
  template <class F>
  void reconcile_end(F&& f, VenueId venue = VenueId::invalid()) noexcept {
    const ReconcileScope& sc = recon_scope_[venue.value];
    StaticVector<Handle<Order>, kMaxOpenOrders> unseen;
    pool_.for_each([&](Handle<Order> h, const Order& o) {
      if (!in_scope(o, venue) || o.has(Order::kSeenInReconcile)) return;
      if (sc.bounded && o.cl_ord_id.value > sc.watermark.value) return;  // not sent yet
      static_cast<void>(unseen.push_back(h));
    });
    for (const Handle<Order> h : unseen) {
      if (!pool_.is_live(h)) continue;
      Order& o = pool_.get(h);
      OmsUpdate u;
      u.prev = o.state;
      u.known = true;
      o.flags |= Order::kReconciled;
      // Working quantity nobody accounted for: the snapshot proves the order is over, not how it
      // ended. Treating it as cancelled is a guess, so report it instead of hiding it.
      if (o.leaves_qty().is_positive()) {
        u.unresolved_qty = o.leaves_qty();
        ++stats_.reconcile_unresolved;
      }
      terminate(u, h, o, OrderState::Canceled, /*by_reconcile=*/true);
      f(u);
    }
  }

  // ---- ack timeout ------------------------------------------------------------------------

  // Orders still PendingNew `timeout` after they were submitted: the request or its ack was lost,
  // and nothing else ever frees them. They hold a pool slot, a max_open_orders slot and same-side
  // open quantity that feeds max_position, so the engine force-cancels them (the venue may know
  // the order even though we never saw its ack). F(Handle<Order>, const Order&) runs outside the
  // pool iteration, so it may send. Returns the number reported.
  template <class F>
  std::size_t sweep_pending(Timestamp now, Duration timeout, F&& f) noexcept {
    if (timeout.ns <= 0) return 0;
    StaticVector<Handle<Order>, kMaxOpenOrders> stale;
    pool_.for_each([&](Handle<Order> h, const Order& o) {
      if (o.state != OrderState::PendingNew || !o.created.valid()) return;
      if (now - o.created < timeout) return;
      static_cast<void>(stale.push_back(h));
    });
    for (const Handle<Order> h : stale) {
      if (!pool_.is_live(h)) continue;
      ++stats_.ack_timeouts;
      f(h, pool_.get(h));
    }
    return stale.size();
  }

  // ---- queries ----------------------------------------------------------------------------

  [[nodiscard]] const Order& get(Handle<Order> h) const noexcept { return pool_.get(h); }
  [[nodiscard]] Order& get(Handle<Order> h) noexcept { return pool_.get(h); }
  [[nodiscard]] bool is_live(Handle<Order> h) const noexcept { return pool_.is_live(h); }
  // Send and ack times of an open order.
  [[nodiscard]] const OrderTimes& times(Handle<Order> h) const noexcept { return times_[h.idx].t; }
  [[nodiscard]] Handle<Order> find(ClientOrderId id) const noexcept {
    const Handle<Order>* p = by_id_.find(id);
    return p == nullptr ? Handle<Order>{} : *p;
  }
  [[nodiscard]] OrderClass classify(ClientOrderId id) const noexcept {
    if (by_id_.contains(id)) return OrderClass::Open;
    if (recently_terminal_.find_if([&](const TerminalRecord& r) { return r.cl_ord_id == id; }) !=
        nullptr) {
      return OrderClass::RecentlyTerminal;
    }
    return OrderClass::Unknown;
  }
  // Ascending handle order (deterministic). F(Handle<Order>, const Order&).
  template <class F>
  void for_each_open_order(F&& f) const noexcept {
    pool_.for_each([&](Handle<Order> h, const Order& o) { f(h, o); });
  }
  template <class F>
  void for_each_open_order(InstrumentId id, F&& f) const noexcept {
    pool_.for_each([&](Handle<Order> h, const Order& o) {
      if (o.instrument == id) f(h, o);
    });
  }
  void warm_up() noexcept { pool_.warm_up(); }

 private:
  struct TerminalRecord {
    ClientOrderId cl_ord_id;
    Qty cum_qty;
    InstrumentId instrument;
    OrderState state;
    Side side;
    bool by_reconcile;  // marked Canceled by reconcile_end(), not reported by the venue
  };
  struct ReconcileScope {
    ClientOrderId watermark{};
    bool bounded = false;
  };
  // Quantity booked at `price` because a venue message reported it as cumulative and no execution
  // ever named it (Engine::book_missed_fill). It stays until an execution does.
  struct SyntheticFill {
    ClientOrderId cl_ord_id;
    Qty qty;
    Price price;
  };
  struct TimesSlot {
    OrderTimes t;
    Timestamp replace_sent;  // a replace in flight: its send time
  };

  [[nodiscard]] static bool in_scope(const Order& o, VenueId venue) noexcept {
    return !venue.valid() || o.venue == venue;
  }

  // A venue message carries more cumulative quantity than the fills we booked: the difference
  // traded while we were not listening. Advance cum_qty (and the open-quantity accounting) and
  // report the difference so the caller can book it; without that the position, the PnL and the
  // max-loss budget silently lose those fills.
  void absorb_cum(Order& o, Qty reported, OmsUpdate& u) noexcept {
    Qty cum = reported;
    if (cum > o.qty) cum = o.qty;
    if (cum <= o.cum_qty) return;
    const Qty diff = cum - o.cum_qty;
    open_qty_[o.instrument.value][static_cast<std::size_t>(o.side)] -= diff;
    o.cum_qty = cum;
    u.missed_qty += diff;
    u.changed = true;
    ++stats_.missed_fills;
    note_synthetic(o.cl_ord_id, diff, o.price);
  }

  // ---- synthetic fills waiting to be named ------------------------------------------------

  // The engine is about to book `qty` at `px` without knowing what it really traded at. Remember
  // it against the order so a later execution-history replay corrects it instead of adding it
  // again. The pool is empty in normal operation, which is the only cost on_fill pays for this.
  void note_synthetic(ClientOrderId id, Qty qty, Price px) noexcept {
    for (SyntheticFill& f : synthetic_) {
      if (f.cl_ord_id == id) {
        f.qty += qty;
        f.price = px;
        return;
      }
    }
    if (synthetic_.full()) {
      ++stats_.synthetic_forgotten;
      synthetic_.erase_front();
    }
    static_cast<void>(synthetic_.push_back(SyntheticFill{id, qty, px}));
  }

  // Takes up to `qty` of the order's outstanding estimate into `u`.
  void take_synthetic(ClientOrderId id, Qty qty, OmsUpdate& u) noexcept {
    if (FASTMM_LIKELY(synthetic_.empty())) return;
    for (std::size_t i = 0; i < synthetic_.size(); ++i) {
      SyntheticFill& f = synthetic_[i];
      if (f.cl_ord_id != id) continue;
      u.corrected_qty = qty < f.qty ? qty : f.qty;
      u.synthetic_px = f.price;
      f.qty -= u.corrected_qty;
      ++stats_.corrected_fills;
      if (!f.qty.is_positive()) synthetic_.erase_at(i);
      return;
    }
  }

  // Finds an open order; sets u.known if the id is open or recently terminal.
  Handle<Order> lookup(ClientOrderId id, OmsUpdate& u) noexcept {
    const Handle<Order>* p = by_id_.find(id);
    if (p != nullptr) {
      u.known = true;
      u.handle = *p;
      return *p;
    }
    const TerminalRecord* r =
        recently_terminal_.find_if([&](const TerminalRecord& t) { return t.cl_ord_id == id; });
    if (r != nullptr) {
      // The slot is gone; a late fill still needs to know what it was for.
      u.known = true;
      u.order.cl_ord_id = r->cl_ord_id;
      u.order.instrument = r->instrument;
      u.order.side = r->side;
      u.order.state = r->state;
      u.order.cum_qty = r->cum_qty;
      if (r->by_reconcile) u.order.flags |= Order::kReconciled;
    }
    return Handle<Order>{};
  }

  void finish_update(OmsUpdate& u, Handle<Order> h, const Order& o) noexcept {
    u.order = o;
    u.handle = h;
    u.times = times_[h.idx].t;
    u.slot = h;
  }

  // `rekey`: the replacement carries a new client id. `new_venue_order`: it is a different order
  // on the venue (cancel-replace), so its cumulative quantity starts at zero; an in-place amend
  // rekeys without that.
  void apply_replace(Order& o, bool rekey, bool new_venue_order) noexcept {
    const std::size_t s = static_cast<std::size_t>(o.side);
    Qty& open = open_qty_[o.instrument.value][s];
    open -= o.leaves_qty();
    if (rekey) {
      by_id_.erase(o.cl_ord_id);
      o.cl_ord_id = o.pending_cl_ord_id;
    }
    if (new_venue_order) o.cum_qty = Qty{};
    const Price old_px = o.price;
    o.price = o.pending_price;
    o.qty = o.pending_qty;
    if (o.cum_qty > o.qty) o.cum_qty = o.qty;
    open += o.leaves_qty();
    o.pending_cl_ord_id = ClientOrderId{};
    o.pending_price = Price{};
    o.pending_qty = Qty{};
    o.flags &= static_cast<std::uint8_t>(~Order::kReplaceOldCanceled);
    o.state = o.cum_qty.is_zero() ? OrderState::Live : OrderState::PartiallyFilled;
    Price& best = best_own_[o.instrument.value][s];
    if (best.is_zero() || better(o.side, o.price, best)) {
      best = o.price;
    } else if (old_px == best) {
      recompute_best_own(o.instrument, o.side);
    }
  }
  void clear_pending(Order& o) noexcept {
    if (o.pending_cl_ord_id.valid() && o.pending_cl_ord_id != o.cl_ord_id)
      by_id_.erase(o.pending_cl_ord_id);
    o.pending_cl_ord_id = ClientOrderId{};
    o.pending_price = Price{};
    o.pending_qty = Qty{};
    o.flags &= static_cast<std::uint8_t>(~Order::kReplaceOldCanceled);
  }

  void terminate(OmsUpdate& u,
                 Handle<Order> h,
                 Order& o,
                 OrderState final_state,
                 bool by_reconcile = false) noexcept {
    o.state = final_state;
    if (final_state == OrderState::Canceled) ++stats_.canceled;
    --stats_.open;
    --open_per_inst_[o.instrument.value];
    const std::size_t s = static_cast<std::size_t>(o.side);
    open_qty_[o.instrument.value][s] -= o.leaves_qty();
    by_id_.erase(o.cl_ord_id);
    if (o.pending_cl_ord_id.valid() && o.pending_cl_ord_id != o.cl_ord_id)
      by_id_.erase(o.pending_cl_ord_id);
    recently_terminal_.push(
        TerminalRecord{o.cl_ord_id, o.cum_qty, o.instrument, final_state, o.side, by_reconcile});
    u.order = o;
    u.handle = Handle<Order>{};
    u.times = times_[h.idx].t;
    u.slot = h;
    u.changed = true;
    u.terminal = true;
    const InstrumentId inst = o.instrument;
    const Side side = o.side;
    const Price px = o.price;
    // Swap-remove from the dense list of live orders.
    const std::uint32_t pos = live_pos_[h.idx];
    const std::uint32_t last = live_.back();
    live_[pos] = last;
    live_pos_[last] = pos;
    live_.pop_back();
    pool_.free(h);
    if (px == best_own_[inst.value][s]) recompute_best_own(inst, side);
  }

  // Scans the live orders (live_), not the kMaxOpenOrders-slot pool.
  void recompute_best_own(InstrumentId inst, Side side) noexcept {
    Price best{};
    for (const std::uint32_t idx : live_) {
      const Order& o = pool_.get(Handle<Order>{idx});
      if (o.instrument != inst || o.side != side) continue;
      if (best.is_zero() || better(side, o.price, best)) best = o.price;
    }
    best_own_[inst.value][static_cast<std::size_t>(side)] = best;
  }

  // An execution is what the venue says it is: its id on that instrument and side (the two halves
  // of a self-trade share an id). Not the order it names: the private stream names it and a
  // replayed trade history may not (a venue order id the connector no longer maps), and keying on
  // the order booked such an execution twice.
  bool remember_exec(const OrderFillMsg& m) noexcept {
    const std::uint64_t key = m.exec_id.hash() ^
                              (static_cast<std::uint64_t>(m.hdr.instrument.value) << 1U) ^
                              (static_cast<std::uint64_t>(m.side) * 0x9E3779B97F4A7C15ULL);
    return exec_seen_.assign(key, 1);
  }

  std::uint16_t epoch_;
  std::uint64_t max_seq_;
  std::uint64_t seq_ = 0;  // 64-bit so the comparison with max_seq_ cannot itself wrap
  OmsStats stats_{};
  Pool<Order, kMaxOpenOrders> pool_;
  // OrderTimes by pool slot, apart from Order so the order record keeps its 128-byte layout.
  std::unique_ptr<TimesSlot[]> times_ = std::make_unique<TimesSlot[]>(kMaxOpenOrders);
  // Pool slots of the live orders in no particular order, and each live slot's position in it.
  // Only order-independent scans use it (recompute_best_own); iteration that produces messages
  // keeps the pool's slot order.
  StaticVector<std::uint32_t, kMaxOpenOrders> live_;
  std::uint32_t live_pos_[kMaxOpenOrders] = {};
  OpenHashMap<ClientOrderId, Handle<Order>, kMaxOpenOrders * 4> by_id_;  // ids + pending ids
  RingBuffer<TerminalRecord, kRecentlyTerminal> recently_terminal_;
  RecentMap<std::uint64_t, std::uint8_t, kRecentlyTerminal> exec_seen_;
  Price best_own_[kMaxInstruments][2];
  Qty open_qty_[kMaxInstruments][2];
  std::uint32_t open_per_inst_[kMaxInstruments] = {};
  ReconcileScope recon_scope_[std::numeric_limits<VenueId::rep_type>::max() + 1U] = {};
  StaticVector<SyntheticFill, kMaxSyntheticFills> synthetic_;
};

}  // namespace fastmm
