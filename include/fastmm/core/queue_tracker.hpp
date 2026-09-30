#pragma once
// QueueTracker: the engine's estimate of the quantity resting ahead of each of our orders at its
// price, with the queue model of the l2_queue fill model (core/queue_model.hpp) on the market data
// the engine sees.
//
//   ack          ahead = displayed quantity at the price, less our own (OwnQuantity), as of the
//                book's last update; the touch's quantity instead when a newer BookTicker has the
//                price as its touch (queue_join)
//   replace ack  same price and a quantity no larger than the leaves keeps `ahead` (as the
//                simulator does); anything else joins the back again
//   depth delta  each level at one of our prices: queue_after_level_change(ahead, old, new) with
//                old and new displayed quantities less our own
//   snapshot     ahead is capped at what the level now shows
//   trade        queue_after_trade: consumed ahead first, a trade through our price empties it;
//                one newer than the depth book takes its quantity from the level as the next
//                delta's old quantity, and from the depth book and the touch at placement
//                (TradeTape)
//   ticker       a BookTicker newer than the depth book (update id, else venue time), less our
//                own quantity at its venue time: queue_after_touch
//   own quantity others_shown(): a level showing less than ours there predates our order
//
// Off until enable() (the strategy's first queue_ahead). State is a side array by OMS slot, one
// list and one trade tape per instrument. A trade newer than the depth book goes on the tape, and a
// book update drops what it now shows; a delta also costs, for each of our orders on the
// instrument, a price compare per level on its side, and a trade one step per order.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/queue_model.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace fastmm {

class QueueTracker {
 public:
  static constexpr std::uint32_t kNone = 0xFFFF'FFFFU;

  explicit QueueTracker(std::int64_t conservatism_bps = 10'000)
      : bps_(conservatism_bps < 0        ? 0
             : conservatism_bps > 10'000 ? 10'000
                                         : conservatism_bps),
        slots_(std::make_unique<Slot[]>(kMaxOpenOrders)),
        touch_(std::make_unique<QueueTouch[]>(kMaxInstruments)),
        tape_(std::make_unique<TradeTape[]>(kMaxInstruments)) {
    head_.fill(kNone);
  }

  [[nodiscard]] std::int64_t conservatism_bps() const noexcept { return bps_; }
  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  void enable() noexcept { enabled_ = true; }
  [[nodiscard]] bool any(InstrumentId id) const noexcept { return head_[id.value] != kNone; }
  [[nodiscard]] bool tracked(Handle<Order> h) const noexcept {
    return h.valid() && slots_[h.idx].on;
  }
  [[nodiscard]] std::optional<Qty> ahead(Handle<Order> h) const noexcept {
    if (!tracked(h)) return std::nullopt;
    return slots_[h.idx].ahead;
  }

  // The order joins the back of its level; `shown` is the level less our own quantity.
  void place(Handle<Order> h, const Order& o, Qty shown) noexcept {
    Slot& s = slots_[h.idx];
    if (!s.on) link(h.idx, o.instrument);
    s.ahead = shown;
    s.level = shown;
    s.px = o.price;
    s.inst = o.instrument;
    s.side = o.side;
  }

  void remove(Handle<Order> h) noexcept {
    if (!tracked(h)) return;
    Slot& s = slots_[h.idx];
    if (s.prev != kNone) {
      slots_[s.prev].next = s.next;
    } else {
      head_[s.inst.value] = s.next;
    }
    if (s.next != kNone) slots_[s.next].prev = s.prev;
    s.on = false;
    s.next = s.prev = kNone;
  }

  // `depth_ahead`, or the latest BookTicker's side when it is newer than the depth book `b`
  // (queue_join). own(side, px, t): our quantity in the feed at venue time t.
  template <class Own>
  [[nodiscard]] Qty at_placement(Qty depth_ahead,
                                 const Order& o,
                                 const L2Book<256>& b,
                                 Own&& own) const noexcept {
    const QueueTouch& t = touch_[o.instrument.value];
    const TradeTape& tape = tape_[o.instrument.value];
    depth_ahead = tape.after(o.side, o.price, depth_ahead, b.last_update());
    if (!t.valid() || !t.newer_than(b.seq(), b.last_update())) return depth_ahead;
    const Price touch = t.px(o.side);
    const Qty shown = others_shown(t.qty(o.side), own(o.side, touch, t.ts));
    return queue_join(depth_ahead, o.side, o.price, touch, tape.after(o.side, touch, shown, t.ts));
  }

  // A BookTicker: kept for placements, and applied to the instrument's orders when it is newer
  // than the depth book `b`.
  template <class Own>
  void on_ticker(const BookTickerMsg& m, const L2Book<256>& b, Own&& own) noexcept {
    const InstrumentId id = m.hdr.instrument;
    QueueTouch& t = touch_[id.value];
    t = queue_touch(m, [](Side, Price) { return Qty{}; });
    if (head_[id.value] == kNone || !t.newer_than(b.seq(), b.last_update())) return;
    const TradeTape& tape = tape_[id.value];
    const Qty shown[2] = {
        tape.after(
            Side::Buy, t.bid_px, others_shown(t.bid_qty, own(Side::Buy, t.bid_px, t.ts)), t.ts),
        tape.after(
            Side::Sell, t.ask_px, others_shown(t.ask_qty, own(Side::Sell, t.ask_px, t.ts)), t.ts)};
    for (std::uint32_t i = head_[id.value]; i != kNone; i = slots_[i].next) {
      Slot& s = slots_[i];
      const bool buy = s.side == Side::Buy;
      s.ahead = queue_after_touch(s.ahead, s.side, s.px, t.px(s.side), shown[buy ? 0 : 1]);
    }
  }

  // After `b` applied `d`. own(side, px): our quantity in the feed at that price as of the book's
  // last update (zero where the feed does not show our orders).
  template <class Own>
  void on_book(const BookDeltaMsg& d, const L2Book<256>& b, Own&& own) noexcept {
    const InstrumentId id = d.hdr.instrument;
    tape_[id.value].forget_through(b.last_update());
    if (d.is_snapshot()) {
      for (std::uint32_t i = head_[id.value]; i != kNone; i = slots_[i].next) {
        Slot& s = slots_[i];
        const Qty shown = others_shown(level_qty(b, s.side, s.px), own(s.side, s.px));
        if (shown < s.ahead) s.ahead = shown;
        s.level = shown;
      }
      return;
    }
    const Level* lv = d.levels();
    for (std::uint32_t i = head_[id.value]; i != kNone;) {
      Slot& s = slots_[i];
      i = s.next;
      const bool buy = s.side == Side::Buy;
      const std::uint32_t end = buy ? d.bid_count : d.bid_count + d.ask_count;
      for (std::uint32_t k = buy ? 0 : d.bid_count; k < end; ++k) {
        if (lv[k].price != s.px) continue;
        const Qty shown = others_shown(lv[k].qty, own(s.side, s.px));
        s.ahead = queue_after_level_change(s.ahead, s.level, shown, bps_);
        s.level = shown;
      }
    }
  }

  // A trade on an instrument whose depth book is `b`.
  void on_trade(const TradeMsg& t, const Oms& oms, const L2Book<256>& b) noexcept {
    const InstrumentId id = t.hdr.instrument;
    const Timestamp ts = t.hdr.exch_ts.valid() ? t.hdr.exch_ts : t.hdr.recv_ts;
    const bool newer = ts > b.last_update();
    if (newer) tape_[id.value].add(t, ts);
    for (std::uint32_t i = head_[id.value]; i != kNone; i = slots_[i].next) {
      Slot& s = slots_[i];
      const Qty leaves = oms.get(Handle<Order>{i}).leaves_qty();
      static_cast<void>(
          queue_after_trade(s.ahead, s.side, s.px, leaves, t.price, t.qty, t.aggressor));
      if (newer) s.level = level_after_print(s.level, s.side, s.px, t.price, t.qty, t.aggressor);
    }
  }

 private:
  struct Slot {
    Qty ahead;
    Qty level;  // what the level showed (less our own) when last seen
    Price px;
    InstrumentId inst;
    Side side = Side::Buy;
    bool on = false;
    std::uint32_t next = kNone;
    std::uint32_t prev = kNone;
  };

  void link(std::uint32_t idx, InstrumentId id) noexcept {
    Slot& s = slots_[idx];
    s.on = true;
    s.prev = kNone;
    s.next = head_[id.value];
    if (s.next != kNone) slots_[s.next].prev = idx;
    head_[id.value] = idx;
  }

  std::int64_t bps_;
  bool enabled_ = false;
  std::array<std::uint32_t, kMaxInstruments> head_{};
  std::unique_ptr<Slot[]> slots_;
  std::unique_ptr<QueueTouch[]> touch_;  // the latest BookTicker per instrument, as published
  std::unique_ptr<TradeTape[]> tape_;    // trades newer than the depth book, per instrument
};

}  // namespace fastmm
