#pragma once
// SimBook: the price-level index of the simulated exchange (8.1). Each side is a sorted
// StaticVector<PriceLevel> with the BEST level at back() (same layout as L2Book), and each
// level heads an intrusive FIFO of SimOrders that live in the MatchingEngine's Pool. The
// book never touches the orders themselves: it only knows head/tail handles, so the
// price-time priority invariant is "first in the level's list == first to trade".
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fastmm::sim {

using AccountId = std::uint16_t;
inline constexpr AccountId kGeneratorAccount = 0;  // synthetic counter-party flow
inline constexpr AccountId kStrategyAccount = 1;   // the engine under test
inline constexpr std::size_t kMaxAccounts = 16;
inline constexpr std::size_t kMaxSimOrders = 1U << 16;
inline constexpr std::size_t kMaxSimLevels = 2048;

// One resting or in-flight order at the simulated venue (72 bytes).
struct SimOrder {
  std::uint64_t order_id;   // venue order id, monotonic per engine
  ClientOrderId cl_ord_id;  // unique per (account, cl_ord_id)
  Price price;              // zero for market orders
  Qty qty;                  // original quantity
  Qty cum_qty;              // executed so far
  Timestamp created;        // venue arrival time
  InstrumentId instrument;  // 4
  std::uint32_t next;       // FIFO link within the price level (kNullHandle == end)
  std::uint32_t prev;       // FIFO link within the price level
  AccountId account;        // 2
  Side side;                // 1
  OrderType type;           // 1
  TimeInForce tif;          // 1
  std::uint8_t pad_[3];     // -> 72

  [[nodiscard]] constexpr Qty leaves() const noexcept { return qty - cum_qty; }
};
static_assert(sizeof(SimOrder) == 72 && std::is_trivially_copyable_v<SimOrder>);

struct PriceLevel {
  Price price;
  Qty qty;              // sum of leaves of the orders in the FIFO
  std::uint32_t head;   // first (oldest) order, kNullHandle if empty
  std::uint32_t tail;   // last (newest) order
  std::uint32_t count;  // orders in the FIFO
  std::uint32_t pad_;
};
static_assert(sizeof(PriceLevel) == 32 && std::is_trivially_copyable_v<PriceLevel>);

class SimBook {
 public:
  using Levels = StaticVector<PriceLevel, kMaxSimLevels>;

  SimBook() noexcept = default;

  [[nodiscard]] Levels& levels(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
  [[nodiscard]] const Levels& levels(Side s) const noexcept {
    return s == Side::Buy ? bids_ : asks_;
  }
  [[nodiscard]] PriceLevel* best(Side s) noexcept {
    Levels& v = levels(s);
    return v.empty() ? nullptr : &v.back();
  }
  [[nodiscard]] const PriceLevel* best(Side s) const noexcept {
    const Levels& v = levels(s);
    return v.empty() ? nullptr : &v.back();
  }
  [[nodiscard]] Level best_level(Side s) const noexcept {
    const PriceLevel* l = best(s);
    return l == nullptr ? Level{} : Level{l->price, l->qty};
  }

  // Index of the first level at-or-better than `price` scanning from the worst (front).
  [[nodiscard]] std::size_t lower_bound(Side s, Price price) const noexcept {
    const Levels& v = levels(s);
    std::size_t lo = 0;
    std::size_t hi = v.size();
    // Near-top hint: most inserts land within a few levels of the best.
    if (hi > 0 && better(s, price, v[hi - 1].price)) return hi;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      if (better(s, price, v[mid].price)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }
  [[nodiscard]] PriceLevel* find(Side s, Price price) noexcept {
    Levels& v = levels(s);
    const std::size_t i = lower_bound(s, price);
    return (i < v.size() && v[i].price == price) ? &v[i] : nullptr;
  }
  [[nodiscard]] const PriceLevel* find(Side s, Price price) const noexcept {
    const Levels& v = levels(s);
    const std::size_t i = lower_bound(s, price);
    return (i < v.size() && v[i].price == price) ? &v[i] : nullptr;
  }
  // Returns the level for `price`, creating an empty one if needed; nullptr if the side is full.
  [[nodiscard]] PriceLevel* find_or_insert(Side s, Price price) noexcept {
    Levels& v = levels(s);
    const std::size_t i = lower_bound(s, price);
    if (i < v.size() && v[i].price == price) return &v[i];
    if (!v.insert_at(i, PriceLevel{price, Qty{}, kNullHandle, kNullHandle, 0, 0})) return nullptr;
    return &v[i];
  }
  void erase(Side s, const PriceLevel* l) noexcept {
    Levels& v = levels(s);
    FASTMM_ASSERT(l >= v.begin() && l < v.end());
    v.erase_at(static_cast<std::size_t>(l - v.begin()));
  }
  // Quantity resting at prices at-or-better than `limit` on side s (FOK pre-check).
  [[nodiscard]] Qty qty_at_or_better(Side s, Price limit, bool is_market) const noexcept {
    const Levels& v = levels(s);
    Qty sum{};
    for (std::size_t i = v.size(); i > 0; --i) {
      const PriceLevel& l = v[i - 1];
      if (!is_market && !at_or_better(s, l.price, limit)) break;
      sum += l.qty;
    }
    return sum;
  }
  // Best-first copy of up to `max` levels.
  std::size_t snapshot(Side s, Level* out, std::size_t max) const noexcept {
    const Levels& v = levels(s);
    const std::size_t n = v.size() < max ? v.size() : max;
    for (std::size_t i = 0; i < n; ++i) {
      const PriceLevel& l = v[v.size() - 1 - i];
      out[i] = Level{l.price, l.qty};
    }
    return n;
  }
  [[nodiscard]] std::size_t depth(Side s) const noexcept { return levels(s).size(); }
  [[nodiscard]] std::uint64_t update_id() const noexcept { return update_id_; }
  std::uint64_t bump_update_id() noexcept { return ++update_id_; }
  void clear() noexcept {
    bids_.clear();
    asks_.clear();
  }

 private:
  Levels bids_;  // ascending price, best (highest) at back
  Levels asks_;  // descending price, best (lowest) at back
  std::uint64_t update_id_ = 0;
};

}  // namespace fastmm::sim
