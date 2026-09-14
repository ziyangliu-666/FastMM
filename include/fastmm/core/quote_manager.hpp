#pragma once
// QuoteManager (5.6): diffs the strategy's DesiredQuotes against the orders resting at the
// venue and emits the minimal set of New / Cancel / Replace actions.
//
// Rules:
//   * level i of a side maps to slot i (one order per slot);
//   * hysteresis: keep an order whose price is within min_requote_ticks of the desired
//     price and whose leaves cover min_qty_bps of the desired quantity;
//   * never re-quote a slot within min_requote_interval of its last change;
//   * orders in a Pending* state are never touched: the slot records the target instead and applies
//     it when the order's ack or terminal update arrives via on_order_update() (dropped if the
//     instrument is pulled first);
//   * a slot only counts an order as its own if it is live and carries the slot's tag (order
//     slots are reused, and a replace changes the order's id);
//   * while an instrument is pulled, an order that becomes working late (its ack arrived after
//     the pull) is cancelled at once;
//   * a New rejected by the venue (other than a post-only cross) blocks new orders on that side
//     for reject_backoff, doubling on each further reject up to reject_backoff_max and reset by
//     an ack on that side, so an unfundable side is not resent on every requote;
//   * venues with supports_replace get a single Replace; otherwise Cancel now and New
//     only once the cancel's terminal update arrives via on_order_update() (cancel-then-new
//     keeps momentary exposure down and never double-quotes a level);
//   * pull_quotes(keep_desired) pauses an instrument (the engine during reconciliation): resume()
//     re-applies the last desired quotes unless the instrument was pulled for good meanwhile.
//
// Actions are executed by the caller through a Placer callable so the QuoteManager stays
// independent of risk/OMS/transport wiring:
//   bool place(QuoteAction& a);   // New: sets a.handle on success; returns false on reject
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm {

inline constexpr std::size_t kMaxQuoteLevels = 8;

struct DesiredQuotes {
  StaticVector<Level, kMaxQuoteLevels> bids;  // index 0 = closest to mid
  StaticVector<Level, kMaxQuoteLevels> asks;
  void clear() noexcept {
    bids.clear();
    asks.clear();
  }
  [[nodiscard]] const StaticVector<Level, kMaxQuoteLevels>& side(Side s) const noexcept {
    return s == Side::Buy ? bids : asks;
  }
  [[nodiscard]] bool empty() const noexcept { return bids.empty() && asks.empty(); }
};

struct QuoteParams {
  std::int64_t min_requote_ticks = 1;  // |desired - resting| < this -> keep
  Duration min_requote_interval = milliseconds(50);
  std::int64_t min_qty_bps = 8000;  // leaves >= 80 % of desired -> qty ok
  bool supports_replace = false;
  bool post_only = true;
  Duration reject_backoff = milliseconds(1000);  // 0 = no backoff after venue rejects
  Duration reject_backoff_max = milliseconds(60000);
};

enum class QuoteActionKind : std::uint8_t { New, Cancel, Replace };

struct QuoteAction {
  QuoteActionKind kind;
  InstrumentId instrument;
  Side side;
  std::uint32_t level;
  Handle<Order> handle;     // Cancel/Replace input; New output
  ClientOrderId cl_ord_id;  // New output (the placer fills it)
  Price price;
  Qty qty;
  bool post_only;
};

struct QuoteStats {
  std::uint64_t news = 0;
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
  std::uint64_t kept_hysteresis = 0;
  std::uint64_t kept_interval = 0;
  std::uint64_t skipped_pending = 0;
  std::uint64_t rejected = 0;
  std::uint64_t reject_backoffs = 0;  // venue rejects that started or extended a side backoff
  std::uint64_t kept_backoff = 0;     // News withheld because their side was backing off
};

class QuoteManager {
 public:
  explicit QuoteManager(const QuoteParams& p = {}) noexcept : params_(p) {}

  [[nodiscard]] const QuoteParams& params() const noexcept { return params_; }
  void set_params(const QuoteParams& p) noexcept { params_ = p; }
  [[nodiscard]] const QuoteStats& stats() const noexcept { return stats_; }

  // user_tag encoding so a terminal OmsUpdate can be mapped back to its slot.
  [[nodiscard]] static constexpr std::uint32_t make_tag(Side s, std::uint32_t level) noexcept {
    return 0x5100'0000U | (static_cast<std::uint32_t>(s) << 8) | level;
  }
  [[nodiscard]] static constexpr bool is_quote_tag(std::uint32_t tag) noexcept {
    return (tag & 0xFFFF'0000U) == 0x5100'0000U;
  }

  // Reconciles desired vs resting for one instrument. Returns the number of actions placed.
  template <class Placer>
  std::uint32_t reconcile(const Instrument& inst,
                          const DesiredQuotes& desired,
                          const Oms& oms,
                          Timestamp now,
                          Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    st.pulled = false;
    st.resumable = false;
    st.desired = desired;
    std::uint32_t actions = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
      const auto& want = st.desired.side(side);
      for (std::uint32_t lvl = 0; lvl < kMaxQuoteLevels; ++lvl) {
        Slot& slot = st.slots[static_cast<std::size_t>(side)][lvl];
        const bool has_want = lvl < want.size() && want[lvl].qty.is_positive();
        actions += reconcile_slot(
            inst, st, oms, side, lvl, slot, has_want ? want[lvl] : Level{}, now, place);
      }
    }
    return actions;
  }

  // Cancels every working quote on the instrument and suppresses re-quoting until the
  // next reconcile(). With keep_desired (a pause, not a decision to stop quoting) the last desired
  // quotes are kept for resume(), unless the instrument was already pulled.
  template <class Placer>
  std::uint32_t pull_quotes(const Instrument& inst,
                            const Oms& oms,
                            Placer&& place,
                            bool keep_desired = false) noexcept {
    InstState& st = state_[inst.id.value];
    if (keep_desired) {
      st.resumable = st.resumable || (!st.pulled && !st.desired.empty());
    } else {
      st.resumable = false;
      st.desired.clear();
    }
    st.pulled = true;
    std::uint32_t actions = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
      for (std::uint32_t lvl = 0; lvl < kMaxQuoteLevels; ++lvl) {
        Slot& slot = st.slots[static_cast<std::size_t>(side)][lvl];
        slot.apply_want = false;
        if (!owns(oms, inst, side, lvl, slot)) continue;
        const Order& o = oms.get(slot.handle);
        if (!o.is_working()) continue;  // pending: cannot touch; a later pull will catch it
        actions += cancel(inst, side, lvl, slot, place);
      }
    }
    return actions;
  }

  // Re-applies the desired quotes kept by pull_quotes(keep_desired). Returns the actions placed.
  template <class Placer>
  std::uint32_t resume(const Instrument& inst,
                       const Oms& oms,
                       Timestamp now,
                       Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    if (!st.resumable) return 0;
    const DesiredQuotes desired = st.desired;
    return reconcile(inst, desired, oms, now, place);
  }
  [[nodiscard]] bool resumable(InstrumentId id) const noexcept {
    return state_[id.value].resumable;
  }

  // Feed every OmsUpdate here. A terminal update frees the slot it belonged to and, if a target
  // was recorded for it (cancel-then-new, or a requote that met the order pending), places the New
  // now. The ack of an order that was pending at a requote applies the recorded target. While the
  // instrument is pulled, an order that has just become working is cancelled instead.
  template <class Placer>
  void on_order_update(const OmsUpdate& u,
                       const Instrument& inst,
                       const Oms& oms,
                       Timestamp now,
                       Placer&& place) noexcept {
    if (!is_quote_tag(u.order.user_tag)) return;
    const Side side = static_cast<Side>((u.order.user_tag >> 8) & 1U);
    const std::uint32_t lvl = u.order.user_tag & 0xFFU;
    if (lvl >= kMaxQuoteLevels || u.order.instrument != inst.id) return;
    InstState& st = state_[inst.id.value];
    Slot& slot = st.slots[static_cast<std::size_t>(side)][lvl];
    if (!u.terminal) {
      if (u.prev == OrderState::PendingNew && u.order.is_working())
        st.backoff[static_cast<std::size_t>(side)] = Duration{};  // the venue takes this side
      if (!u.handle.valid() || slot.handle.idx != u.handle.idx ||
          !owns(oms, inst, side, lvl, slot) || !u.order.is_working())
        return;
      if (st.pulled) {
        static_cast<void>(cancel(inst, side, lvl, slot, place));
        return;
      }
      if (slot.apply_want &&
          (u.prev == OrderState::PendingNew || u.prev == OrderState::PendingReplace)) {
        slot.apply_want = false;
        static_cast<void>(reconcile_slot(inst, st, oms, side, lvl, slot, slot.want, now, place));
      }
      return;
    }
    if (u.order.state == OrderState::Rejected && u.prev == OrderState::PendingNew)
      on_venue_reject(st, side, u.order.reject_reason, now);
    // The terminal order was this slot's unless the slot already holds a newer live order. Ids
    // are not compared: a replace changes the order's id, and a stale id used to drop the update
    // and leave the slot waiting for a terminal state forever.
    if (owns(oms, inst, side, lvl, slot)) return;
    slot.handle = Handle<Order>{};
    slot.cl_ord_id = ClientOrderId{};
    slot.awaiting_terminal = false;
    if (st.pulled || !slot.apply_want) return;
    slot.apply_want = false;
    if (!slot.want.qty.is_positive()) return;
    if (backing_off(st, side, now)) {
      ++stats_.kept_backoff;
      return;
    }
    submit_new(inst, side, lvl, slot, slot.want, now, place);
  }

  [[nodiscard]] Handle<Order> slot_handle(InstrumentId id,
                                          Side s,
                                          std::uint32_t level) const noexcept {
    return state_[id.value].slots[static_cast<std::size_t>(s)][level].handle;
  }
  [[nodiscard]] bool pulled(InstrumentId id) const noexcept { return state_[id.value].pulled; }

 private:
  struct Slot {
    Handle<Order> handle{};
    ClientOrderId cl_ord_id{};
    Timestamp last_requote{};
    Level want{};             // target to apply once the slot's pending order resolves
    bool apply_want = false;  // `want` is pending (qty 0: no quote on this level)
    bool awaiting_terminal = false;
  };
  struct InstState {
    Slot slots[2][kMaxQuoteLevels];
    DesiredQuotes desired;
    bool pulled = false;
    bool resumable = false;    // paused by pull_quotes(keep_desired): resume() re-applies `desired`
    Duration backoff[2] = {};  // current backoff per side (0 = none)
    Timestamp blocked_until[2] = {};  // no News on the side before this
  };

  [[nodiscard]] static bool backing_off(const InstState& st, Side side, Timestamp now) noexcept {
    const auto i = static_cast<std::size_t>(side);
    return st.backoff[i].ns > 0 && now < st.blocked_until[i];
  }
  void on_venue_reject(InstState& st, Side side, RejectReason reason, Timestamp now) noexcept {
    // A post-only cross is a stale price, fixed by the next requote; other venue rejects (no
    // balance, filters, permissions) repeat until something changes.
    if (params_.reject_backoff.ns <= 0 || reason == RejectReason::PostOnlyWouldCross) return;
    const auto i = static_cast<std::size_t>(side);
    Duration next = st.backoff[i].ns <= 0 ? params_.reject_backoff : st.backoff[i] + st.backoff[i];
    if (params_.reject_backoff_max.ns > 0 && next.ns > params_.reject_backoff_max.ns)
      next = params_.reject_backoff_max;
    st.backoff[i] = next;
    st.blocked_until[i] = now + next;
    ++stats_.reject_backoffs;
  }

  // One slot of reconcile(): keep, cancel, replace or place so the slot converges on `target`
  // (qty 0: no quote).
  template <class Placer>
  std::uint32_t reconcile_slot(const Instrument& inst,
                               InstState& st,
                               const Oms& oms,
                               Side side,
                               std::uint32_t lvl,
                               Slot& slot,
                               Level target,
                               Timestamp now,
                               Placer& place) noexcept {
    const bool has_want = target.qty.is_positive();
    if (owns(oms, inst, side, lvl, slot)) {
      const Order& o = oms.get(slot.handle);
      if (is_pending(o.state)) {
        // Cannot touch it now; apply the target when its ack or terminal update arrives.
        ++stats_.skipped_pending;
        slot.want = target;
        slot.apply_want = true;
        return 0;
      }
      slot.apply_want = false;
      if (!has_want) return cancel(inst, side, lvl, slot, place);
      const std::int64_t dpx = (target.price - o.price).abs().raw / inst.tick.raw;
      const bool qty_ok = static_cast<Int128>(o.leaves_qty().raw) * 10'000 >=
                          static_cast<Int128>(target.qty.raw) * params_.min_qty_bps;
      if (dpx < params_.min_requote_ticks && qty_ok) {
        ++stats_.kept_hysteresis;
        return 0;
      }
      if (now - slot.last_requote < params_.min_requote_interval) {
        ++stats_.kept_interval;
        return 0;
      }
      if (params_.supports_replace) {
        QuoteAction a{QuoteActionKind::Replace,
                      inst.id,
                      side,
                      lvl,
                      slot.handle,
                      ClientOrderId{},
                      target.price,
                      target.qty,
                      params_.post_only};
        if (!place(a)) {
          ++stats_.rejected;
          return 0;
        }
        ++stats_.replaces;
        slot.last_requote = now;
        return 1;
      }
      slot.want = target;
      slot.apply_want = true;
      return cancel(inst, side, lvl, slot, place);
    }
    // No resting order in this slot. A cancel still in flight keeps the order live (Pending*,
    // handled above), so any earlier order is final in the OMS and there is nothing left to
    // wait for, even if its terminal update never reached on_order_update().
    slot.handle = Handle<Order>{};
    slot.awaiting_terminal = false;
    slot.apply_want = false;
    if (!has_want) return 0;
    if (backing_off(st, side, now)) {
      ++stats_.kept_backoff;
      return 0;
    }
    return submit_new(inst, side, lvl, slot, target, now, place);
  }

  // The slot's order is live and really the slot's: order slots are reused by later orders.
  [[nodiscard]] static bool owns(const Oms& oms,
                                 const Instrument& inst,
                                 Side side,
                                 std::uint32_t lvl,
                                 const Slot& slot) noexcept {
    if (!slot.handle.valid() || !oms.is_live(slot.handle)) return false;
    const Order& o = oms.get(slot.handle);
    return o.instrument == inst.id && o.user_tag == make_tag(side, lvl);
  }

  template <class Placer>
  std::uint32_t cancel(
      const Instrument& inst, Side side, std::uint32_t lvl, Slot& slot, Placer& place) noexcept {
    QuoteAction a{QuoteActionKind::Cancel,
                  inst.id,
                  side,
                  lvl,
                  slot.handle,
                  ClientOrderId{},
                  Price{},
                  Qty{},
                  false};
    if (!place(a)) {
      ++stats_.rejected;
      return 0;
    }
    ++stats_.cancels;
    slot.awaiting_terminal = true;
    return 1;
  }
  template <class Placer>
  std::uint32_t submit_new(const Instrument& inst,
                           Side side,
                           std::uint32_t lvl,
                           Slot& slot,
                           Level target,
                           Timestamp now,
                           Placer& place) noexcept {
    QuoteAction a{QuoteActionKind::New,
                  inst.id,
                  side,
                  lvl,
                  Handle<Order>{},
                  ClientOrderId{},
                  target.price,
                  target.qty,
                  params_.post_only};
    if (!place(a) || !a.handle.valid()) {
      ++stats_.rejected;
      return 0;
    }
    ++stats_.news;
    slot.handle = a.handle;
    slot.cl_ord_id = a.cl_ord_id;
    slot.last_requote = now;
    slot.awaiting_terminal = false;
    return 1;
  }

  QuoteParams params_;
  QuoteStats stats_{};
  InstState state_[kMaxInstruments] = {};
};

}  // namespace fastmm
