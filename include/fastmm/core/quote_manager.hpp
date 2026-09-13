#pragma once
// QuoteManager (5.6): diffs the strategy's DesiredQuotes against the orders resting at the
// venue and emits the minimal set of New / Cancel / Replace actions.
//
// Rules:
//   * level i of a side maps to slot i (one order per slot);
//   * hysteresis: keep an order whose price is within min_requote_ticks of the desired
//     price and whose leaves cover min_qty_bps of the desired quantity;
//   * never re-quote a slot within min_requote_interval of its last change;
//   * orders in a Pending* state are never touched;
//   * venues with supports_replace get a single Replace; otherwise Cancel now and New
//     only once the cancel's terminal update arrives via on_order_update() (cancel-then-new
//     keeps momentary exposure down and never double-quotes a level).
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
    st.desired = desired;
    std::uint32_t actions = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
      const auto& want = desired.side(side);
      for (std::uint32_t lvl = 0; lvl < kMaxQuoteLevels; ++lvl) {
        Slot& slot = st.slots[static_cast<std::size_t>(side)][lvl];
        const bool has_want = lvl < want.size() && want[lvl].qty.is_positive();
        const Level target = has_want ? want[lvl] : Level{};
        if (slot.handle.valid() && oms.is_live(slot.handle)) {
          const Order& o = oms.get(slot.handle);
          if (is_pending(o.state)) {
            ++stats_.skipped_pending;
            continue;
          }
          if (!has_want) {
            actions += cancel(inst, side, lvl, slot, place);
            continue;
          }
          const std::int64_t dpx = (target.price - o.price).abs().raw / inst.tick.raw;
          const bool qty_ok = static_cast<Int128>(o.leaves_qty().raw) * 10'000 >=
                              static_cast<Int128>(target.qty.raw) * params_.min_qty_bps;
          if (dpx < params_.min_requote_ticks && qty_ok) {
            ++stats_.kept_hysteresis;
            continue;
          }
          if (now - slot.last_requote < params_.min_requote_interval) {
            ++stats_.kept_interval;
            continue;
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
            if (place(a)) {
              ++stats_.replaces;
              slot.last_requote = now;
              ++actions;
            } else {
              ++stats_.rejected;
            }
          } else {
            slot.want = target;
            slot.renew_after_cancel = true;
            actions += cancel(inst, side, lvl, slot, place);
          }
          continue;
        }
        // No resting order in this slot.
        slot.handle = Handle<Order>{};
        if (!has_want) continue;
        if (slot.awaiting_terminal) {  // cancel in flight: New goes out on the terminal update
          slot.want = target;
          slot.renew_after_cancel = true;
          continue;
        }
        actions += submit_new(inst, side, lvl, slot, target, now, place);
      }
    }
    return actions;
  }

  // Cancels every working quote on the instrument and suppresses re-quoting until the
  // next reconcile().
  template <class Placer>
  std::uint32_t pull_quotes(const Instrument& inst, const Oms& oms, Placer&& place) noexcept {
    InstState& st = state_[inst.id.value];
    st.pulled = true;
    st.desired.clear();
    std::uint32_t actions = 0;
    for (Side side : {Side::Buy, Side::Sell}) {
      for (std::uint32_t lvl = 0; lvl < kMaxQuoteLevels; ++lvl) {
        Slot& slot = st.slots[static_cast<std::size_t>(side)][lvl];
        slot.renew_after_cancel = false;
        if (!slot.handle.valid() || !oms.is_live(slot.handle)) continue;
        const Order& o = oms.get(slot.handle);
        if (!o.is_working()) continue;  // pending: cannot touch; a later pull will catch it
        actions += cancel(inst, side, lvl, slot, place);
      }
    }
    return actions;
  }

  // Feed every OmsUpdate here. On a terminal update of a slot order, the slot is freed and,
  // if a replacement was queued (cancel-then-new), the New is placed now.
  template <class Placer>
  void on_order_update(const OmsUpdate& u,
                       const Instrument& inst,
                       Timestamp now,
                       Placer&& place) noexcept {
    if (!u.terminal || !is_quote_tag(u.order.user_tag)) return;
    const Side side = static_cast<Side>((u.order.user_tag >> 8) & 1U);
    const std::uint32_t lvl = u.order.user_tag & 0xFFU;
    if (lvl >= kMaxQuoteLevels || u.order.instrument != inst.id) return;
    InstState& st = state_[inst.id.value];
    Slot& slot = st.slots[static_cast<std::size_t>(side)][lvl];
    if (slot.cl_ord_id != u.order.cl_ord_id) return;  // stale/other order
    slot.handle = Handle<Order>{};
    slot.cl_ord_id = ClientOrderId{};
    slot.awaiting_terminal = false;
    if (st.pulled) return;
    if (slot.renew_after_cancel) {
      slot.renew_after_cancel = false;
      submit_new(inst, side, lvl, slot, slot.want, now, place);
    }
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
    Level want{};
    bool renew_after_cancel = false;
    bool awaiting_terminal = false;
  };
  struct InstState {
    Slot slots[2][kMaxQuoteLevels];
    DesiredQuotes desired;
    bool pulled = false;
  };

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
