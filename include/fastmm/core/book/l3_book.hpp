#pragma once
// L3Book: order-by-order book for ITCH-style feeds (5.4, ADR-0015).
//
// Capacities are constructor arguments (L3BookConfig); every buffer is allocated by the
// constructor and nothing allocates afterwards.
//
// Levels near the market live in a tick-indexed window of price_window_ticks per side, so
// price -> level is one subtraction. A bitmap per side marks the non-empty window levels:
// finding the next level (best-level repair, depth walks) skips 64 empty ticks per word.
// Levels outside the window (stub quotes, deep levels) live in a per-side overflow store: an
// array of at most max_overflow_levels levels sorted best-first. Adding or removing an order
// outside the window touches only that store. The window is moved (recentred) when a side's
// best level lies outside it: onto the midpoint of the two touches when they are less than a
// window apart, onto the touch of a one-sided book. Touches further apart than the window keep
// the window on the one it holds (it moves to the bid when it holds neither), so a stub-only
// side does not pull the window back and forth. Levels leaving the window move to the overflow
// store and overflow levels inside the new window move in, with
// their FIFO queues unchanged. A recentre that would overflow the store is skipped (counted in
// recentre_failures()); the book stays correct, the out-of-window touch is only slower.
//
// Orders live in a fixed pool with an intrusive doubly-linked FIFO per level; order_ref ->
// order goes through an open-addressing index sized 2 x max_orders (power of two).
//
// Every operation is O(1) except: recentre (moves the window arrays, rare), overflow levels
// (binary search plus a memmove of the store) and best-level repair (bitmap scan).
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/strong_id.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>

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
  OutOfWindow,  // outside the window and the overflow store of the side is full
  OffTick,
  BadQty
};

struct L3BookConfig {
  std::size_t price_window_ticks = std::size_t{1} << 16;  // per side; multiple of 64
  std::size_t max_orders = std::size_t{1} << 20;
  std::size_t max_overflow_levels = 1024;  // per side
};

namespace detail {

// order ref -> pool index. Linear probing with backward-shift deletion (no tombstones); the
// capacity is a power of two >= 2 x max entries, so an insert never finds the table full.
class L3RefIndex {
 public:
  explicit L3RefIndex(std::size_t max_entries)
      : mask_(std::bit_ceil(std::max<std::size_t>(max_entries * 2, 4)) - 1),
        shift_(64 - std::countr_zero(mask_ + 1)),
        slots_(new Slot[mask_ + 1]) {
    clear();
  }

  void clear() noexcept {
    for (std::size_t i = 0; i <= mask_; ++i) slots_[i] = Slot{0, kNullHandle, 0};
  }
  [[nodiscard]] FASTMM_FORCE_INLINE std::uint32_t find(std::uint64_t key) const noexcept {
    std::size_t i = home(key);
    for (;;) {
      const Slot& s = slots_[i];
      if (s.value == kNullHandle) return kNullHandle;
      if (s.key == key) return s.value;
      i = (i + 1) & mask_;
    }
  }
  // False if the key is present.
  FASTMM_FORCE_INLINE bool insert(std::uint64_t key, std::uint32_t value) noexcept {
    std::size_t i = home(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.value == kNullHandle) {
        s.key = key;
        s.value = value;
        return true;
      }
      if (s.key == key) return false;
      i = (i + 1) & mask_;
    }
  }
  FASTMM_FORCE_INLINE void erase(std::uint64_t key) noexcept {
    std::size_t hole = home(key);
    for (;;) {
      if (slots_[hole].value == kNullHandle) return;
      if (slots_[hole].key == key) break;
      hole = (hole + 1) & mask_;
    }
    std::size_t j = hole;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].value == kNullHandle) break;
      const std::size_t h = home(slots_[j].key);
      // Slot j may move into `hole` iff its home is not in the cyclic range (hole, j].
      const bool in_range = hole <= j ? (h > hole && h <= j) : (h > hole || h <= j);
      if (!in_range) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].value = kNullHandle;
  }

 private:
  struct Slot {
    std::uint64_t key;
    std::uint32_t value;  // kNullHandle: empty
    std::uint32_t pad_;
  };
  [[nodiscard]] FASTMM_FORCE_INLINE std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
  }

  std::size_t mask_;
  int shift_;
  std::unique_ptr<Slot[]> slots_;
};

}  // namespace detail

class L3Book {
 public:
  using OrderHandle = Handle<L3Order>;

  // Construction happens at startup; failing to allocate is fatal by design.
  explicit L3Book(Price tick, const L3BookConfig& cfg = {})
      : tick_(tick),
        window_(static_cast<std::int64_t>(cfg.price_window_ticks)),
        words_(cfg.price_window_ticks / 64),
        max_orders_(cfg.max_orders),
        ov_cap_(cfg.max_overflow_levels),
        orders_(new L3Order[cfg.max_orders]),
        by_ref_(cfg.max_orders),
        scratch_(new OvLevel[cfg.max_overflow_levels > 0 ? cfg.max_overflow_levels : 1]) {
    FASTMM_CHECK(tick.raw > 0);
    FASTMM_CHECK(cfg.price_window_ticks >= 64 && cfg.price_window_ticks % 64 == 0);
    FASTMM_CHECK(cfg.max_orders > 0 && cfg.max_orders < kNullHandle);
    for (std::size_t s = 0; s < 2; ++s) {
      levels_[s] = std::make_unique<L3Level[]>(cfg.price_window_ticks);
      bits_[s] = std::make_unique<std::uint64_t[]>(words_);
      ov_[s] = std::make_unique<OvLevel[]>(ov_cap_ > 0 ? ov_cap_ : 1);
      ov_tmp_[s] = std::make_unique<OvLevel[]>(ov_cap_ > 0 ? ov_cap_ : 1);
    }
    clear();
  }
  L3Book(const L3Book&) = delete;
  L3Book& operator=(const L3Book&) = delete;

  void clear() noexcept {
    for (std::size_t i = 0; i < max_orders_; ++i) orders_[i].next = OrderHandle{next_index(i)};
    free_head_ = 0;
    by_ref_.clear();
    for (std::size_t s = 0; s < 2; ++s) {
      std::fill_n(levels_[s].get(), static_cast<std::size_t>(window_), L3Level{});
      std::fill_n(bits_[s].get(), words_, std::uint64_t{0});
      ov_n_[s] = 0;
      best_[s] = kNone;
    }
    seq_ = 0;
    order_count_ = 0;
    centred_ = false;
  }

  // ---- mutation ----------------------------------------------------------------------------

  L3Error add(std::uint64_t ref, Side side, Price price, Qty qty) noexcept {
    if (FASTMM_UNLIKELY(!qty.is_positive())) return L3Error::BadQty;
    const std::int64_t t = price.raw / tick_.raw;
    if (FASTMM_UNLIKELY(price.raw % tick_.raw != 0)) return L3Error::OffTick;
    const std::uint32_t h = free_head_;
    if (FASTMM_UNLIKELY(h == kNullHandle)) return L3Error::PoolExhausted;
    if (FASTMM_UNLIKELY(!by_ref_.insert(ref, h))) return L3Error::DuplicateRef;
    L3Order& o = orders_[h];
    free_head_ = o.next.idx;
    o.ref = ref;
    o.price = price;
    o.qty = qty;
    o.side = side;
    o.next = OrderHandle{};
    if (FASTMM_UNLIKELY(order_count_ == 0 || !centred_)) {
      base_tick_ = t - window_ / 2;
      centred_ = true;
    }
    const std::int64_t rel = t - base_tick_;
    if (FASTMM_LIKELY(static_cast<std::uint64_t>(rel) < static_cast<std::uint64_t>(window_))) {
      L3Level& lvl = level_at(side, rel);
      link_back(lvl, OrderHandle{h}, o);
      if (lvl.count == 1) set_bit(side, rel);
      update_best_on_add(side, rel);
    } else if (FASTMM_UNLIKELY(!add_overflow(OrderHandle{h}, o, t))) {
      by_ref_.erase(ref);
      release(h);
      return L3Error::OutOfWindow;
    }
    ++order_count_;
    ++seq_;
    return L3Error::None;
  }

  // Executes `qty` against the order (removing it if fully filled). Returns the fill price
  // via *px if non-null.
  L3Error execute(std::uint64_t ref, Qty qty, Price* px = nullptr) noexcept {
    const OrderHandle h = find(ref);
    if (FASTMM_UNLIKELY(!h.valid())) return L3Error::UnknownRef;
    if (px != nullptr) *px = orders_[h.idx].price;
    return execute(h, qty);
  }
  // Partial cancel (qty > 0) or delete (qty == 0 / >= remaining).
  L3Error cancel(std::uint64_t ref, Qty qty = Qty{}) noexcept {
    const OrderHandle h = find(ref);
    if (FASTMM_UNLIKELY(!h.valid())) return L3Error::UnknownRef;
    return cancel(h, qty);
  }
  L3Error remove(std::uint64_t ref) noexcept { return cancel(ref, Qty{}); }
  // ITCH replace: delete old, add new at the back of the (possibly new) level's queue.
  L3Error replace(std::uint64_t old_ref, std::uint64_t new_ref, Price price, Qty qty) noexcept {
    const OrderHandle h = find(old_ref);
    if (FASTMM_UNLIKELY(!h.valid())) return L3Error::UnknownRef;
    const Side side = orders_[h.idx].side;
    reduce(h, orders_[h.idx].qty);
    return add(new_ref, side, price, qty);
  }

  // Handle forms: `h` must come from find() with no mutation in between.
  L3Error execute(OrderHandle h, Qty qty) noexcept {
    const L3Order& o = orders_[h.idx];
    reduce(h, qty >= o.qty ? o.qty : qty);
    return L3Error::None;
  }
  L3Error cancel(OrderHandle h, Qty qty = Qty{}) noexcept {
    const L3Order& o = orders_[h.idx];
    reduce(h, (qty.is_zero() || qty >= o.qty) ? o.qty : qty);
    return L3Error::None;
  }

  void apply(const OrderAddL3Msg& m) noexcept { add(m.order_ref, m.side, m.price, m.qty); }
  void apply(const OrderExecL3Msg& m) noexcept { execute(m.order_ref, m.exec_qty); }
  void apply(const OrderCancelL3Msg& m) noexcept { cancel(m.order_ref, m.canceled_qty); }
  void apply(const OrderReplaceL3Msg& m) noexcept {
    replace(m.old_order_ref, m.new_order_ref, m.price, m.qty);
  }

  // ---- queries ---------------------------------------------------------------------------

  [[nodiscard]] FASTMM_FORCE_INLINE OrderHandle find(std::uint64_t ref) const noexcept {
    return OrderHandle{by_ref_.find(ref)};
  }
  [[nodiscard]] const L3Order& at(OrderHandle h) const noexcept { return orders_[h.idx]; }
  [[nodiscard]] const L3Order* order(std::uint64_t ref) const noexcept {
    const OrderHandle h = find(ref);
    return h.valid() ? &orders_[h.idx] : nullptr;
  }
  // Quantity queued ahead of `ref` at its level (FIFO position).
  [[nodiscard]] Qty qty_ahead(std::uint64_t ref) const noexcept {
    const OrderHandle h0 = find(ref);
    if (!h0.valid()) return Qty{};
    Qty sum{};
    OrderHandle h = orders_[h0.idx].prev;
    while (h.valid()) {
      const L3Order& o = orders_[h.idx];
      sum += o.qty;
      h = o.prev;
    }
    return sum;
  }
  [[nodiscard]] std::size_t order_count() const noexcept { return order_count_; }
  [[nodiscard]] std::size_t max_orders() const noexcept { return max_orders_; }
  [[nodiscard]] std::size_t window_ticks() const noexcept {
    return static_cast<std::size_t>(window_);
  }
  [[nodiscard]] Price tick() const noexcept { return tick_; }
  [[nodiscard]] std::uint32_t recentre_count() const noexcept { return recentres_; }
  [[nodiscard]] std::uint32_t recentre_failures() const noexcept { return recentre_failures_; }
  // Levels currently held in the overflow store.
  [[nodiscard]] std::size_t overflow_levels(Side side) const noexcept {
    return ov_n_[static_cast<std::size_t>(side)];
  }
  // Lowest price inside the window (Price{} before the first order).
  [[nodiscard]] Price window_low() const noexcept { return price_of(base_tick_); }

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
  // i-th non-empty level from the best (i == 0 best).
  [[nodiscard]] Level level(Side side, std::size_t i) const noexcept {
    Level out{};
    std::size_t seen = 0;
    for_each_level(side, [&](const Level& l) {
      if (seen++ == i) {
        out = l;
        return false;
      }
      return true;
    });
    return out;
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
  // Visits levels best-first: overflow levels better than the window, the window, then
  // overflow levels worse than the window. F(const Level&) may return bool (false stops).
  template <class F>
  void for_each_level(Side side, F&& f) const noexcept {
    const auto s = static_cast<std::size_t>(side);
    const OvLevel* ov = ov_[s].get();
    const std::size_t n = ov_n_[s];
    std::size_t i = 0;
    for (; i < n && better_than_window(side, ov[i].tick); ++i) {
      if (!visit(f, Level{price_of(ov[i].tick), ov[i].lvl.qty})) return;
    }
    std::int64_t idx = best_[s];
    while (idx != kNone) {
      if (!visit(f, Level{price_of(base_tick_ + idx), level_at(side, idx).qty})) return;
      idx = side == Side::Buy ? next_set_down(s, idx - 1) : next_set_up(s, idx + 1);
    }
    for (; i < n; ++i) {
      if (!visit(f, Level{price_of(ov[i].tick), ov[i].lvl.qty})) return;
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
  // Up to out.size() best levels of `side`, best first; returns the count written.
  std::size_t top_levels(Side side, std::span<Level> out) const noexcept {
    std::size_t n = 0;
    if (out.empty()) return 0;
    for_each_level(side, [&](const Level& l) {
      out[n++] = l;
      return n < out.size();
    });
    return n;
  }
  // Visits orders at a level in FIFO order. F(const L3Order&).
  template <class F>
  void for_each_order_at(Side side, Price price, F&& f) const noexcept {
    if (!centred_) return;
    const L3Level* lvl = find_level(side, price.raw / tick_.raw);
    if (lvl == nullptr) return;
    OrderHandle h = lvl->head;
    while (h.valid()) {
      const L3Order& o = orders_[h.idx];
      f(o);
      h = o.next;
    }
  }
  [[nodiscard]] bool is_valid() const noexcept {
    const Level b = best_bid();
    const Level a = best_ask();
    return !b.qty.is_zero() && !a.qty.is_zero() && b.price < a.price;
  }
  [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
  [[nodiscard]] Timestamp last_update() const noexcept { return last_update_; }
  void set_last_update(Timestamp t) noexcept { last_update_ = t; }

 private:
  static constexpr std::int64_t kNone = -1;

  struct OvLevel {
    std::int64_t tick;
    L3Level lvl;
  };
  static_assert(sizeof(OvLevel) == 32 && std::is_trivially_copyable_v<OvLevel>);

  template <class F>
  static FASTMM_FORCE_INLINE bool visit(F& f, const Level& l) noexcept {
    if constexpr (std::is_same_v<std::invoke_result_t<F&, const Level&>, bool>) {
      return f(l);
    } else {
      f(l);
      return true;
    }
  }

  [[nodiscard]] std::uint32_t next_index(std::size_t i) const noexcept {
    return i + 1 < max_orders_ ? static_cast<std::uint32_t>(i + 1) : kNullHandle;
  }
  FASTMM_FORCE_INLINE void release(std::uint32_t h) noexcept {
    orders_[h].next = OrderHandle{free_head_};
    free_head_ = h;
  }
  FASTMM_FORCE_INLINE void link_back(L3Level& lvl, OrderHandle h, L3Order& o) noexcept {
    o.prev = lvl.tail;
    if (lvl.tail.valid()) {
      orders_[lvl.tail.idx].next = h;
    } else {
      lvl.head = h;
    }
    lvl.tail = h;
    lvl.qty += o.qty;
    ++lvl.count;
  }
  FASTMM_FORCE_INLINE void unlink(L3Level& lvl, const L3Order& o) noexcept {
    if (o.prev.valid()) {
      orders_[o.prev.idx].next = o.next;
    } else {
      lvl.head = o.next;
    }
    if (o.next.valid()) {
      orders_[o.next.idx].prev = o.prev;
    } else {
      lvl.tail = o.prev;
    }
    --lvl.count;
  }

  [[nodiscard]] FASTMM_FORCE_INLINE L3Level& level_at(Side s, std::int64_t idx) noexcept {
    return levels_[static_cast<std::size_t>(s)][static_cast<std::size_t>(idx)];
  }
  [[nodiscard]] FASTMM_FORCE_INLINE const L3Level& level_at(Side s,
                                                            std::int64_t idx) const noexcept {
    return levels_[static_cast<std::size_t>(s)][static_cast<std::size_t>(idx)];
  }
  [[nodiscard]] Price price_of(std::int64_t t) const noexcept {
    return Price::from_raw(t * tick_.raw);
  }
  [[nodiscard]] FASTMM_FORCE_INLINE bool in_window(std::int64_t t) const noexcept {
    return static_cast<std::uint64_t>(t - base_tick_) < static_cast<std::uint64_t>(window_);
  }
  // Tick `t` (outside the window) is better than every window price of `side`.
  [[nodiscard]] FASTMM_FORCE_INLINE bool better_than_window(Side side,
                                                            std::int64_t t) const noexcept {
    return side == Side::Buy ? t >= base_tick_ + window_ : t < base_tick_;
  }
  // `a` ranks before `b` in the side's best-first order.
  [[nodiscard]] static FASTMM_FORCE_INLINE bool ranks_before(Side side,
                                                             std::int64_t a,
                                                             std::int64_t b) noexcept {
    return side == Side::Buy ? a > b : a < b;
  }

  // ---- bitmap ------------------------------------------------------------------------------

  FASTMM_FORCE_INLINE void set_bit(Side s, std::int64_t idx) noexcept {
    const auto i = static_cast<std::size_t>(idx);
    bits_[static_cast<std::size_t>(s)][i >> 6U] |= std::uint64_t{1} << (i & 63U);
  }
  FASTMM_FORCE_INLINE void clear_bit(Side s, std::int64_t idx) noexcept {
    const auto i = static_cast<std::size_t>(idx);
    bits_[static_cast<std::size_t>(s)][i >> 6U] &= ~(std::uint64_t{1} << (i & 63U));
  }
  // Highest non-empty window index <= from, or kNone.
  [[nodiscard]] std::int64_t next_set_down(std::size_t s, std::int64_t from) const noexcept {
    if (from < 0) return kNone;
    const std::uint64_t* b = bits_[s].get();
    auto w = static_cast<std::int64_t>(static_cast<std::uint64_t>(from) >> 6U);
    const auto bit = static_cast<unsigned>(from & 63);
    std::uint64_t word =
        b[w] & (bit == 63 ? ~std::uint64_t{0} : (std::uint64_t{1} << (bit + 1)) - 1);
    for (;;) {
      if (word != 0) return w * 64 + 63 - std::countl_zero(word);
      if (--w < 0) return kNone;
      word = b[w];
    }
  }
  // Lowest non-empty window index >= from, or kNone.
  [[nodiscard]] std::int64_t next_set_up(std::size_t s, std::int64_t from) const noexcept {
    if (from >= window_) return kNone;
    const std::uint64_t* b = bits_[s].get();
    std::size_t w = static_cast<std::uint64_t>(from) >> 6U;
    std::uint64_t word = b[w] & (~std::uint64_t{0} << static_cast<unsigned>(from & 63));
    for (;;) {
      if (word != 0) return static_cast<std::int64_t>(w * 64) + std::countr_zero(word);
      if (++w >= words_) return kNone;
      word = b[w];
    }
  }
  // Set bits in window indices [lo, hi).
  [[nodiscard]] std::size_t count_bits(std::size_t s,
                                       std::int64_t lo,
                                       std::int64_t hi) const noexcept {
    std::size_t n = 0;
    for (std::int64_t i = next_set_up(s, lo); i != kNone && i < hi; i = next_set_up(s, i + 1)) ++n;
    return n;
  }

  // ---- best levels -------------------------------------------------------------------------

  [[nodiscard]] Level best_level(Side side) const noexcept {
    const auto s = static_cast<std::size_t>(side);
    const std::int64_t idx = best_[s];
    if (FASTMM_UNLIKELY(ov_n_[s] != 0) &&
        (idx == kNone || better_than_window(side, ov_[s][0].tick))) {
      const OvLevel& o = ov_[s][0];
      return Level{price_of(o.tick), o.lvl.qty};
    }
    if (idx == kNone) return Level{};
    return Level{price_of(base_tick_ + idx), level_at(side, idx).qty};
  }
  // Best tick of `side` over the window and the overflow store; false if the side is empty.
  bool best_tick(Side side, std::int64_t& out) const noexcept {
    const auto s = static_cast<std::size_t>(side);
    if (ov_n_[s] != 0 && (best_[s] == kNone || better_than_window(side, ov_[s][0].tick))) {
      out = ov_[s][0].tick;
      return true;
    }
    if (best_[s] == kNone) return false;
    out = base_tick_ + best_[s];
    return true;
  }
  FASTMM_FORCE_INLINE void update_best_on_add(Side side, std::int64_t idx) noexcept {
    std::int64_t& b = best_[static_cast<std::size_t>(side)];
    if (b == kNone || (side == Side::Buy ? idx > b : idx < b)) b = idx;
  }
  // The level at `from` just emptied (its bit is clear).
  void repair_best(Side side, std::int64_t from) noexcept {
    const auto s = static_cast<std::size_t>(side);
    if (best_[s] != from) return;
    best_[s] = side == Side::Buy ? next_set_down(s, from - 1) : next_set_up(s, from + 1);
  }

  // ---- reduce --------------------------------------------------------------------------------

  void reduce(OrderHandle h, Qty by) noexcept {
    L3Order& o = orders_[h.idx];
    const Side side = o.side;
    const std::int64_t t = o.price.raw / tick_.raw;
    const std::int64_t rel = t - base_tick_;
    ++seq_;
    if (FASTMM_LIKELY(static_cast<std::uint64_t>(rel) < static_cast<std::uint64_t>(window_))) {
      L3Level& lvl = level_at(side, rel);
      o.qty -= by;
      lvl.qty -= by;
      if (o.qty.is_positive()) return;
      unlink(lvl, o);
      free_order(h, o);
      if (lvl.count == 0) {
        clear_bit(side, rel);
        repair_best(side, rel);
        if (best_[static_cast<std::size_t>(side)] == kNone &&
            ov_n_[static_cast<std::size_t>(side)] != 0)
          try_recentre();
      }
      return;
    }
    reduce_overflow(h, o, t, by);
  }
  FASTMM_FORCE_INLINE void free_order(OrderHandle h, const L3Order& o) noexcept {
    by_ref_.erase(o.ref);
    --order_count_;
    release(h.idx);
  }

  // ---- overflow store ------------------------------------------------------------------------

  // First index in the side's store whose tick does not rank before `t`.
  [[nodiscard]] std::size_t ov_lower_bound(Side side, std::int64_t t) const noexcept {
    const auto s = static_cast<std::size_t>(side);
    const OvLevel* ov = ov_[s].get();
    std::size_t lo = 0;
    std::size_t hi = ov_n_[s];
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (ranks_before(side, ov[mid].tick, t)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }
  [[nodiscard]] const L3Level* find_level(Side side, std::int64_t t) const noexcept {
    if (in_window(t)) return &level_at(side, t - base_tick_);
    const auto s = static_cast<std::size_t>(side);
    const std::size_t i = ov_lower_bound(side, t);
    if (i < ov_n_[s] && ov_[s][i].tick == t) return &ov_[s][i].lvl;
    return nullptr;
  }

  FASTMM_NOINLINE bool add_overflow(OrderHandle h, L3Order& o, std::int64_t t) noexcept {
    const Side side = o.side;
    const auto s = static_cast<std::size_t>(side);
    OvLevel* ov = ov_[s].get();
    const std::size_t i = ov_lower_bound(side, t);
    if (i == ov_n_[s] || ov[i].tick != t) {
      if (ov_n_[s] == ov_cap_) return false;
      std::memmove(ov + i + 1, ov + i, sizeof(OvLevel) * (ov_n_[s] - i));
      ov[i] = OvLevel{t, L3Level{}};
      ++ov_n_[s];
    }
    link_back(ov[i].lvl, h, o);
    // The touch of this side left the window: move the window to it.
    if (i == 0 && (best_[s] == kNone || better_than_window(side, t))) try_recentre();
    return true;
  }

  FASTMM_NOINLINE void reduce_overflow(OrderHandle h, L3Order& o, std::int64_t t, Qty by) noexcept {
    const Side side = o.side;
    const auto s = static_cast<std::size_t>(side);
    OvLevel* ov = ov_[s].get();
    const std::size_t i = ov_lower_bound(side, t);
    FASTMM_ASSERT(i < ov_n_[s] && ov[i].tick == t);
    L3Level& lvl = ov[i].lvl;
    o.qty -= by;
    lvl.qty -= by;
    if (o.qty.is_positive()) return;
    unlink(lvl, o);
    free_order(h, o);
    if (lvl.count != 0) return;
    std::memmove(ov + i, ov + i + 1, sizeof(OvLevel) * (ov_n_[s] - i - 1));
    --ov_n_[s];
    if (i == 0 && ov_n_[s] != 0 && best_[s] == kNone) try_recentre();
  }

  // ---- recentre ----------------------------------------------------------------------------

  // Moves the window so that every touch is inside it when possible (see the header comment).
  FASTMM_NOINLINE void try_recentre() noexcept {
    std::int64_t bb = 0;
    std::int64_t ba = 0;
    const bool has_b = best_tick(Side::Buy, bb);
    const bool has_a = best_tick(Side::Sell, ba);
    if ((!has_b || in_window(bb)) && (!has_a || in_window(ba))) return;
    std::int64_t centre = 0;
    if (has_b && has_a) {
      if (ba - bb < window_ - 2) {
        centre = bb + (ba - bb) / 2;
      } else if (in_window(bb) || in_window(ba)) {
        return;  // the touches do not fit in one window: keep the one that is inside
      } else {
        centre = bb;
      }
    } else {
      centre = has_b ? bb : ba;
    }
    if (!recentre(centre - window_ / 2)) ++recentre_failures_;
  }

  bool recentre(std::int64_t new_base) noexcept {
    const std::int64_t shift = new_base - base_tick_;  // positive: the window moves up
    if (shift == 0) return true;
    // Window indices whose levels leave: [ev_lo, ev_hi).
    std::int64_t ev_lo = 0;
    std::int64_t ev_hi = window_;
    if (shift > 0 && shift < window_) ev_hi = shift;
    if (shift < 0 && -shift < window_) ev_lo = window_ + shift;
    for (std::size_t s = 0; s < 2; ++s) {
      std::size_t keep = count_bits(s, ev_lo, ev_hi);
      for (std::size_t i = 0; i < ov_n_[s]; ++i) {
        const std::int64_t rel = ov_[s][i].tick - new_base;
        if (rel < 0 || rel >= window_) ++keep;
      }
      if (keep > ov_cap_) return false;
    }
    for (std::size_t s = 0; s < 2; ++s) {
      const auto side = static_cast<Side>(s);
      L3Level* lv = levels_[s].get();
      // 1. Levels leaving the window, best-first.
      std::size_t ne = 0;
      if (side == Side::Buy) {
        for (std::int64_t i = next_set_down(s, ev_hi - 1); i != kNone && i >= ev_lo;
             i = next_set_down(s, i - 1))
          scratch_[ne++] = OvLevel{base_tick_ + i, lv[i]};
      } else {
        for (std::int64_t i = next_set_up(s, ev_lo); i != kNone && i < ev_hi;
             i = next_set_up(s, i + 1))
          scratch_[ne++] = OvLevel{base_tick_ + i, lv[i]};
      }
      // 2. Shift the window arrays.
      shift_window(s, shift);
      // 3. Overflow levels inside the new window move in; the rest merge with the evicted.
      const OvLevel* ov = ov_[s].get();
      OvLevel* out = ov_tmp_[s].get();
      std::size_t n = 0;
      std::size_t e = 0;
      for (std::size_t i = 0; i < ov_n_[s]; ++i) {
        const std::int64_t rel = ov[i].tick - new_base;
        if (rel >= 0 && rel < window_) {
          lv[rel] = ov[i].lvl;
          set_bit(side, rel);
          continue;
        }
        while (e < ne && ranks_before(side, scratch_[e].tick, ov[i].tick)) out[n++] = scratch_[e++];
        out[n++] = ov[i];
      }
      while (e < ne) out[n++] = scratch_[e++];
      ov_[s].swap(ov_tmp_[s]);
      ov_n_[s] = n;
      best_[s] = side == Side::Buy ? next_set_down(s, window_ - 1) : next_set_up(s, 0);
    }
    base_tick_ = new_base;
    ++recentres_;
    return true;
  }

  // Moves window contents by `shift` ticks (new[i] = old[i + shift]) and zeroes the rest.
  void shift_window(std::size_t s, std::int64_t shift) noexcept {
    L3Level* lv = levels_[s].get();
    std::uint64_t* b = bits_[s].get();
    const std::int64_t w = window_;
    if (shift >= w || -shift >= w) {
      std::fill_n(lv, static_cast<std::size_t>(w), L3Level{});
      std::fill_n(b, words_, std::uint64_t{0});
      return;
    }
    const auto k = static_cast<std::size_t>(shift > 0 ? shift : -shift);
    const auto rest = static_cast<std::size_t>(w) - k;
    const std::size_t q = k / 64;
    const std::size_t r = k % 64;
    const auto nw = static_cast<std::ptrdiff_t>(words_);
    if (shift > 0) {
      std::memmove(lv, lv + k, sizeof(L3Level) * rest);
      std::fill_n(lv + rest, k, L3Level{});
      for (std::ptrdiff_t j = 0; j < nw; ++j) {
        const auto src = j + static_cast<std::ptrdiff_t>(q);
        const std::uint64_t lo = src < nw ? b[src] : 0;
        const std::uint64_t hi = src + 1 < nw ? b[src + 1] : 0;
        b[j] = r == 0 ? lo : (lo >> r) | (hi << (64 - r));
      }
    } else {
      std::memmove(lv + k, lv, sizeof(L3Level) * rest);
      std::fill_n(lv, k, L3Level{});
      for (std::ptrdiff_t j = nw - 1; j >= 0; --j) {
        const auto src = j - static_cast<std::ptrdiff_t>(q);
        const std::uint64_t hi = src >= 0 ? b[src] : 0;
        const std::uint64_t lo = src - 1 >= 0 ? b[src - 1] : 0;
        b[j] = r == 0 ? hi : (hi << r) | (lo >> (64 - r));
      }
    }
  }

  Price tick_;
  std::int64_t window_;
  std::size_t words_;
  std::size_t max_orders_;
  std::size_t ov_cap_;
  std::int64_t base_tick_ = 0;
  bool centred_ = false;
  std::int64_t best_[2] = {kNone, kNone};  // window index of the best window level
  std::uint64_t seq_ = 0;
  std::size_t order_count_ = 0;
  std::uint32_t recentres_ = 0;
  std::uint32_t recentre_failures_ = 0;
  std::uint32_t free_head_ = kNullHandle;
  Timestamp last_update_{};
  std::unique_ptr<L3Level[]> levels_[2];
  std::unique_ptr<std::uint64_t[]> bits_[2];
  std::unique_ptr<OvLevel[]> ov_[2];
  std::unique_ptr<OvLevel[]> ov_tmp_[2];
  std::size_t ov_n_[2] = {0, 0};
  std::unique_ptr<L3Order[]> orders_;
  detail::L3RefIndex by_ref_;
  std::unique_ptr<OvLevel[]> scratch_;
};

static_assert(BookView<L3Book>);

}  // namespace fastmm
