#pragma once
// LevelSpill: room for the side of a depth update that changes more price levels than a
// BookDeltaMsg carries (kMaxBookLevelsPerMsg a side).
//
// Binance's diff streams (Spot and USDⓈ-M `@depth@100ms`) have no bound on the levels one update
// changes: in a fast market a single 100 ms update of USDⓈ-M BTCUSDT changed 1035 bids and 1224
// asks (2026-09-28). Refusing it breaks the U/u (pu) chain, and the next update resyncs the book,
// pulling quotes until a REST snapshot arrives. The parsers read such a side into the spill and
// keep the kMaxBookLevelsPerMsg levels nearest the touch (bids: the highest prices, asks: the
// lowest), in the order the venue sent them.
//
// Why a book of depth D <= kMaxBookLevelsPerMsg / 2 (the engine's L2Book<256>) ends the same: a
// dropped level p lies behind kMaxBookLevelsPerMsg kept levels of its side. If p was in the book,
// at most D - 1 levels were ahead of it, so at least kMaxBookLevelsPerMsg - D + 1 >= D kept levels
// are new ones, which are non-empty (a venue deletes only levels it had, and every level ahead of
// p was in the book). After the update p is behind at least D levels: the book drops it anyway.
// A deeper book (the gateway's AccountBook, 1024) can keep a stale level behind its 512th.
#include "fastmm/core/messages.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>

namespace fastmm::venues {

class LevelSpill {
 public:
  // Levels one side may carry before the update is refused (Overflow) after all.
  static constexpr std::uint32_t kCapacity = 16384;

  LevelSpill()
      : levels_(std::make_unique<Level[]>(kCapacity)),
        prices_(std::make_unique<std::int64_t[]>(kCapacity)) {}

  [[nodiscard]] Level* data() noexcept { return levels_.get(); }

  // The price of the max-th level nearest the touch among the first `n` (> max, max > 0) spilled.
  [[nodiscard]] std::int64_t nearest_bound(std::uint32_t n, bool bids, std::uint32_t max) noexcept {
    std::int64_t* const p = prices_.get();
    for (std::uint32_t i = 0; i < n; ++i) p[i] = levels_[i].price.raw;
    std::int64_t* const nth = p + (max - 1);
    if (bids) {
      std::nth_element(p, nth, p + n, std::greater<>{});
    } else {
      std::nth_element(p, nth, p + n);
    }
    return *nth;
  }
  [[nodiscard]] static constexpr bool within(std::int64_t px, std::int64_t bound, bool bids) {
    return bids ? px >= bound : px <= bound;
  }

  // Copies the `max` of the first `n` spilled levels nearest the touch into `out`, in their order,
  // and returns how many it copied (max, or n if that is smaller).
  std::uint32_t keep_nearest(std::uint32_t n, bool bids, Level* out, std::uint32_t max) noexcept {
    if (n <= max) {
      std::copy_n(levels_.get(), n, out);
      return n;
    }
    const std::int64_t bound = nearest_bound(n, bids, max);
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < n && kept < max; ++i) {
      if (within(levels_[i].price.raw, bound, bids)) out[kept++] = levels_[i];
    }
    return kept;
  }

 private:
  std::unique_ptr<Level[]> levels_;
  std::unique_ptr<std::int64_t[]> prices_;
};

}  // namespace fastmm::venues
