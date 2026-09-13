#pragma once
// L3Book: order-by-order book for ITCH-style feeds (5.4).
//
// Levels are a tick-indexed array (window of kWindowTicks around the market; recentred when
// the best drifts past 25 % / 75 % of the window) so price -> level is one subtraction.
// Orders live in a Pool with an intrusive doubly-linked FIFO per level, and order_ref ->
// handle goes through an OpenHashMap. Best levels are tracked incrementally.
//
// Every operation is O(1) except: recentre (rare memmove) and best-level repair after the
// best level empties (scan toward the far side, bounded by the gap to the next level).
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/pool.hpp"
#include "fastmm/core/messages.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace fastmm {

struct L3Order {
  std::uint64_t ref;
  Price price;
  Qty qty;
  Handle<L3Order> prev;
  Handle<L3Order> next;
  Side side;
  std::uint8_t pad_[7];
};
static_assert(sizeof(L3Order) == 40 && std::is_trivially_copyable_v<L3Order>);

struct L3Level {
  Qty qty;
  std::uint32_t count;
  Handle<L3Order> head;  // oldest
  Handle<L3Order> tail;  // newest
  std::uint32_t pad_;
};
static_assert(sizeof(L3Level) == 24);

enum class L3Error : std::uint8_t {
  None,
  DuplicateRef,
  UnknownRef,
  PoolExhausted,
  OutOfWindow,
  OffTick,
  BadQty
};

template <std::size_t kWindowTicks = 1U << 16, std::size_t kMaxOrders = 1U << 20>
class L3Book {
  static_assert((kWindowTicks & (kWindowTicks - 1)) == 0 && kWindowTicks >= 64);

 public:
  using OrderHandle = Handle<L3Order>;
  static constexpr std::size_t kWindow = kWindowTicks;

  explicit L3Book(Price tick) : tick_(tick) {
    FASTMM_CHECK(tick.raw > 0);
    for (auto& side : levels_) side.reset(new L3Level[kWindowTicks]());
  }

  void clear() noexcept {
    orders_.reset();
    by_ref_.clear();
    for (auto& side : levels_) std::fill_n(side.get(), kWindowTicks, L3Level{});
    best_[0] = best_[1] = kNone;
    seq_ = 0;
    order_count_ = 0;
    centred_ = false;
  }

  // ---- mutation ----------------------------------------------------------------------------

  L3Error add(std::uint64_t ref, Side side, Price price, Qty qty) noexcept {
    if (FASTMM_UNLIKELY(!qty.is_positive())) return L3Error::BadQty;
    if (FASTMM_UNLIKELY(price.raw % tick_.raw != 0)) return L3Error::OffTick;
    if (FASTMM_UNLIKELY(by_ref_.contains(ref))) return L3Error::DuplicateRef;
    std::int64_t idx = 0;
    if (FASTMM_UNLIKELY(!index_for(price, idx))) return L3Error::OutOfWindow;
    const OrderHandle h = orders_.allocate();
    if (FASTMM_UNLIKELY(!h.valid())) return L3Error::PoolExhausted;
    if (FASTMM_UNLIKELY(by_ref_.insert(ref, h).first == nullptr)) {
      orders_.free(h);
      return L3Error::PoolExhausted;
    }
    L3Order& o = orders_.get(h);
    o.ref = ref;
    o.price = price;
    o.qty = qty;
    o.side = side;
    o.next = OrderHandle{};
    L3Level& lvl = level_at(side, idx);
    o.prev = lvl.tail;
    if (lvl.tail.valid()) {
      orders_.get(lvl.tail).next = h;
    } else {
      lvl.head = h;
    }
    lvl.tail = h;
    lvl.qty += qty;
    ++lvl.count;
    ++order_count_;
    update_best_on_add(side, idx);
    ++seq_;
    return L3Error::None;
  }

  // Executes `qty` against the order (removing it if fully filled). Returns the fill price
  // via *px if non-null.
  L3Error execute(std::uint64_t ref, Qty qty, Price* px = nullptr) noexcept {
    OrderHandle* hp = by_ref_.find(ref);
    if (FASTMM_UNLIKELY(hp == nullptr)) return L3Error::UnknownRef;
    L3Order& o = orders_.get(*hp);
    if (px != nullptr) *px = o.price;
    return reduce(*hp, o, qty >= o.qty ? o.qty : qty);
  }
  // Partial cancel (qty > 0) or delete (qty == 0 / >= remaining).
  L3Error cancel(std::uint64_t ref, Qty qty = Qty{}) noexcept {
    OrderHandle* hp = by_ref_.find(ref);
    if (FASTMM_UNLIKELY(hp == nullptr)) return L3Error::UnknownRef;
    L3Order& o = orders_.get(*hp);
    return reduce(*hp, o, (qty.is_zero() || qty >= o.qty) ? o.qty : qty);
  }
  L3Error remove(std::uint64_t ref) noexcept { return cancel(ref, Qty{}); }
  // ITCH replace: delete old, add new at the back of the (possibly new) level's queue.
  L3Error replace(std::uint64_t old_ref, std::uint64_t new_ref, Price price, Qty qty) noexcept {
    OrderHandle* hp = by_ref_.find(old_ref);
    if (FASTMM_UNLIKELY(hp == nullptr)) return L3Error::UnknownRef;
    const Side side = orders_.get(*hp).side;
    const L3Error e = cancel(old_ref);
    if (e != L3Error::None) return e;
    return add(new_ref, side, price, qty);
  }

  void apply(const OrderAddL3Msg& m) noexcept { add(m.order_ref, m.side, m.price, m.qty); }
  void apply(const OrderExecL3Msg& m) noexcept { execute(m.order_ref, m.exec_qty); }
  void apply(const OrderCancelL3Msg& m) noexcept { cancel(m.order_ref, m.canceled_qty); }
  void apply(const OrderReplaceL3Msg& m) noexcept {
    replace(m.old_order_ref, m.new_order_ref, m.price, m.qty);
  }

  // ---- queries ---------------------------------------------------------------------------

  [[nodiscard]] const L3Order* order(std::uint64_t ref) const noexcept {
    const OrderHandle* hp = by_ref_.find(ref);
    return hp == nullptr ? nullptr : &orders_.get(*hp);
  }
  // Quantity queued ahead of `ref` at its level (FIFO position).
  [[nodiscard]] Qty qty_ahead(std::uint64_t ref) const noexcept {
    const OrderHandle* hp = by_ref_.find(ref);
    if (hp == nullptr) return Qty{};
    Qty sum{};
    OrderHandle h = orders_.get(*hp).prev;
    while (h.valid()) {
      const L3Order& o = orders_.get(h);
      sum += o.qty;
      h = o.prev;
    }
    return sum;
  }
  [[nodiscard]] std::size_t order_count() const noexcept { return order_count_; }
  [[nodiscard]] Price tick() const noexcept { return tick_; }
  [[nodiscard]] std::uint32_t recentre_count() const noexcept { return recentres_; }

  [[nodiscard]] Level best_bid() const noexcept { return best_level(Side::Buy); }
  [[nodiscard]] Level best_ask() const noexcept { return best_level(Side::Sell); }
  [[nodiscard]] Price mid() const noexcept {
    const Level b = best_bid();
    const Level a = best_ask();
    if (b.qty.is_zero() || a.qty.is_zero()) return Price{};
    return Price::from_raw((b.price.raw + a.price.raw) / 2);
  }
  [[nodiscard]] Price spread() const noexcept {
    const Level b = best_bid();
    const Level a = best_ask();
    if (b.qty.is_zero() || a.qty.is_zero()) return Price{};
    return a.price - b.price;
  }
  // i-th non-empty level from the best (i == 0 best). Bounded scan across the window.
  [[nodiscard]] Level level(Side side, std::size_t i) const noexcept {
    std::int64_t idx = best_[static_cast<std::size_t>(side)];
    if (idx == kNone) return Level{};
    const std::int64_t step = side == Side::Buy ? -1 : 1;
    std::size_t seen = 0;
    while (idx >= 0 && idx < static_cast<std::int64_t>(kWindowTicks)) {
      const L3Level& l = level_at(side, idx);
      if (l.count > 0) {
        if (seen == i) return Level{price_at(idx), l.qty};
        ++seen;
      }
      idx += step;
    }
    return Level{};
  }
  [[nodiscard]] std::size_t depth(Side side) const noexcept {
    std::size_t n = 0;
    for_each_level(side, [&](const Level&) { ++n; });
    return n;
  }
  [[nodiscard]] Qty qty_at_or_better(Side side, Price limit) const noexcept {
    Qty sum{};
    for_each_level(side, [&](const Level& l) {
      if (!at_or_better(side, l.price, limit)) return false;
      sum += l.qty;
      return true;
    });
    return sum;
  }
  [[nodiscard]] Price price_for_qty(Side side, Qty qty) const noexcept {
    Qty cum{};
    Price out{};
    for_each_level(side, [&](const Level& l) {
      cum += l.qty;
      if (cum >= qty) {
        out = l.price;
        return false;
      }
      return true;
    });
    return out;
  }
  template <class F>
  void for_each_level(Side side, F&& f) const noexcept {
    std::int64_t idx = best_[static_cast<std::size_t>(side)];
    if (idx == kNone) return;
    const std::int64_t step = side == Side::Buy ? -1 : 1;
    while (idx >= 0 && idx < static_cast<std::int64_t>(kWindowTicks)) {
      const L3Level& l = level_at(side, idx);
      if (l.count > 0) {
        const Level lv{price_at(idx), l.qty};
        if constexpr (std::is_same_v<std::invoke_result_t<F, const Level&>, bool>) {
          if (!f(lv)) return;
        } else {
          f(lv);
        }
      }
      idx += step;
    }
  }
  template <std::size_t N>
  [[nodiscard]] TopLevels<N> top(Side side) const noexcept {
    TopLevels<N> out;
    for_each_level(side, [&](const Level& l) {
      out.levels[out.count++] = l;
      return out.count < N;
    });
    return out;
  }
  template <std::size_t N>
  [[nodiscard]] TopLevels<N> to_l2_top(Side side) const noexcept {
    return top<N>(side);
  }
  // Visits orders at a level in FIFO order. F(const L3Order&).
  template <class F>
  void for_each_order_at(Side side, Price price, F&& f) const noexcept {
    std::int64_t idx = 0;
    if (!centred_ || !index_for_const(price, idx)) return;
    OrderHandle h = level_at(side, idx).head;
    while (h.valid()) {
      const L3Order& o = orders_.get(h);
      f(o);
      h = o.next;
    }
  }
  [[nodiscard]] bool is_valid() const noexcept {
    return best_[0] != kNone && best_[1] != kNone && best_bid().price < best_ask().price;
  }
  [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
  [[nodiscard]] Timestamp last_update() const noexcept { return last_update_; }
  void set_last_update(Timestamp t) noexcept { last_update_ = t; }

 private:
  static constexpr std::int64_t kNone = -1;

  [[nodiscard]] FASTMM_FORCE_INLINE L3Level& level_at(Side s, std::int64_t idx) noexcept {
    return levels_[static_cast<std::size_t>(s)][static_cast<std::size_t>(idx)];
  }
  [[nodiscard]] FASTMM_FORCE_INLINE const L3Level& level_at(Side s,
                                                            std::int64_t idx) const noexcept {
    return levels_[static_cast<std::size_t>(s)][static_cast<std::size_t>(idx)];
  }
  [[nodiscard]] Price price_at(std::int64_t idx) const noexcept {
    return Price::from_raw((base_tick_ + idx) * tick_.raw);
  }
  [[nodiscard]] Level best_level(Side side) const noexcept {
    const std::int64_t idx = best_[static_cast<std::size_t>(side)];
    if (idx == kNone) return Level{};
    return Level{price_at(idx), level_at(side, idx).qty};
  }
  bool index_for_const(Price p, std::int64_t& idx) const noexcept {
    const std::int64_t t = p.raw / tick_.raw - base_tick_;
    if (t < 0 || t >= static_cast<std::int64_t>(kWindowTicks)) return false;
    idx = t;
    return true;
  }

  // Maps a price to a window index, centring the window on first use and recentring when
  // the price falls outside or the market has drifted past the 25/75 % marks.
  bool index_for(Price p, std::int64_t& idx) noexcept {
    const std::int64_t t = p.raw / tick_.raw;
    if (FASTMM_UNLIKELY(!centred_)) {
      base_tick_ = t - static_cast<std::int64_t>(kWindowTicks / 2);
      centred_ = true;
    }
    std::int64_t rel = t - base_tick_;
    const auto w = static_cast<std::int64_t>(kWindowTicks);
    if (FASTMM_UNLIKELY(rel < 0 || rel >= w || rel < w / 4 || rel >= w - w / 4)) {
      if (order_count_ == 0) {
        base_tick_ = t - w / 2;
      } else if (!recentre(t)) {
        return false;
      }
      rel = t - base_tick_;
      if (rel < 0 || rel >= w) return false;
    }
    idx = rel;
    return true;
  }

  // Shift the window so that the midpoint of (lowest resting, highest resting, target)
  // sits at the centre. Fails if the resting orders span more than the window.
  bool recentre(std::int64_t target_tick) noexcept {
    std::int64_t lo = target_tick;
    std::int64_t hi = target_tick;
    for (std::size_t s = 0; s < 2; ++s) {
      const L3Level* lv = levels_[s].get();
      for (std::int64_t i = 0; i < static_cast<std::int64_t>(kWindowTicks); ++i) {
        if (lv[i].count > 0) {
          const std::int64_t t = base_tick_ + i;
          lo = t < lo ? t : lo;
          hi = t > hi ? t : hi;
          break;
        }
      }
      for (std::int64_t i = static_cast<std::int64_t>(kWindowTicks) - 1; i >= 0; --i) {
        if (lv[i].count > 0) {
          const std::int64_t t = base_tick_ + i;
          lo = t < lo ? t : lo;
          hi = t > hi ? t : hi;
          break;
        }
      }
    }
    const auto w = static_cast<std::int64_t>(kWindowTicks);
    if (hi - lo >= w) return false;
    const std::int64_t new_base = (lo + hi) / 2 - w / 2;
    const std::int64_t shift = new_base - base_tick_;  // positive: window moves up
    if (shift == 0) return true;
    for (std::size_t s = 0; s < 2; ++s) {
      L3Level* lv = levels_[s].get();
      if (shift > 0) {
        std::memmove(lv, lv + shift, sizeof(L3Level) * static_cast<std::size_t>(w - shift));
        std::fill_n(lv + (w - shift), static_cast<std::size_t>(shift), L3Level{});
      } else {
        std::memmove(lv - shift, lv, sizeof(L3Level) * static_cast<std::size_t>(w + shift));
        std::fill_n(lv, static_cast<std::size_t>(-shift), L3Level{});
      }
      if (best_[s] != kNone) best_[s] -= shift;
    }
    base_tick_ = new_base;
    ++recentres_;
    return true;
  }

  FASTMM_FORCE_INLINE void update_best_on_add(Side side, std::int64_t idx) noexcept {
    std::int64_t& b = best_[static_cast<std::size_t>(side)];
    if (b == kNone || (side == Side::Buy ? idx > b : idx < b)) b = idx;
  }
  void repair_best(Side side, std::int64_t from) noexcept {
    std::int64_t& b = best_[static_cast<std::size_t>(side)];
    if (b != from) return;  // not the best level
    const std::int64_t step = side == Side::Buy ? -1 : 1;
    std::int64_t idx = from;
    while (idx >= 0 && idx < static_cast<std::int64_t>(kWindowTicks)) {
      if (level_at(side, idx).count > 0) {
        b = idx;
        return;
      }
      idx += step;
    }
    b = kNone;
  }

  L3Error reduce(OrderHandle h, L3Order& o, Qty by) noexcept {
    std::int64_t idx = 0;
    index_for_const(o.price, idx);  // resting orders are always inside the window
    L3Level& lvl = level_at(o.side, idx);
    o.qty -= by;
    lvl.qty -= by;
    ++seq_;
    if (o.qty.is_positive()) return L3Error::None;
    // unlink
    if (o.prev.valid()) {
      orders_.get(o.prev).next = o.next;
    } else {
      lvl.head = o.next;
    }
    if (o.next.valid()) {
      orders_.get(o.next).prev = o.prev;
    } else {
      lvl.tail = o.prev;
    }
    --lvl.count;
    --order_count_;
    by_ref_.erase(o.ref);
    const Side side = o.side;
    orders_.free(h);
    if (lvl.count == 0) repair_best(side, idx);
    return L3Error::None;
  }

  Price tick_;
  std::int64_t base_tick_ = 0;
  bool centred_ = false;
  std::int64_t best_[2] = {kNone, kNone};
  std::uint64_t seq_ = 0;
  std::size_t order_count_ = 0;
  std::uint32_t recentres_ = 0;
  Timestamp last_update_{};
  std::unique_ptr<L3Level[]> levels_[2];
  Pool<L3Order, kMaxOrders> orders_;
  OpenHashMap<std::uint64_t, OrderHandle, kMaxOrders * 2> by_ref_;
};

static_assert(BookView<L3Book<1U << 10, 1U << 10>>);

}  // namespace fastmm
