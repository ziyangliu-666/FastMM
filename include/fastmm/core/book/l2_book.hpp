#pragma once
// L2Book<kMaxDepth>: price-aggregated book for crypto feeds (5.4).
//
// Each side is a sorted StaticVector<Level> with the BEST level at back(): top-of-book is
// O(1), and the common near-top insert/erase only memmoves a handful of 16-byte levels.
// Bids are stored ascending by price, asks descending, so both sides share one code path
// keyed on "distance from best". Lookups scan the top 16 linearly and bisect the rest.
//
// Depth is capped at kMaxDepth: levels worse than the worst stored one are dropped and the
// side is flagged truncated() until the next snapshot (crypto venues resend snapshots on
// resync; we deliberately do not try to repair). Crossed books are tolerated (Binance can
// cross for ~100 ms between updates); the engine polls crossed_for(now) against a grace.
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace fastmm {

template <std::size_t kMaxDepth = 256>
class L2Book {
  static_assert(kMaxDepth >= 2);

 public:
  static constexpr std::size_t kDepth = kMaxDepth;
  static constexpr std::size_t kLinearScan = 16;

  L2Book() noexcept = default;

  // ---- mutation --------------------------------------------------------------------------

  void clear() noexcept {
    bids_.clear();
    asks_.clear();
    truncated_[0] = truncated_[1] = false;
    have_snapshot_ = false;
    crossed_since_ = Timestamp{};
    seq_ = 0;
  }

  // Sets/updates/deletes (qty == 0) one level. Returns false if the level was dropped
  // because the side is full and the price is worse than everything stored.
  FASTMM_FORCE_INLINE bool apply_level(Side side, Price price, Qty qty) noexcept {
    auto& v = levels(side);
    // Position such that v[i].price is the first level at-or-better than `price` when
    // scanning from the worst (front) - i.e. lower_bound in "goodness" order.
    std::size_t i = v.size();
    bool found = false;
    // Linear scan from the best (back) over the top kLinearScan levels.
    const std::size_t stop = v.size() > kLinearScan ? v.size() - kLinearScan : 0;
    while (i > stop) {
      const Price p = v[i - 1].price;
      if (p == price) {
        found = true;
        --i;
        break;
      }
      if (better(side, price, p)) break;  // insert after v[i-1]
      --i;
    }
    if (!found && i == stop && stop > 0) {
      // Bisect the remainder [0, stop): find first index whose price is at-or-better.
      std::size_t lo = 0;
      std::size_t hi = stop;
      while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (better(side, price, v[mid].price)) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      i = lo;
      found = i < v.size() && v[i].price == price;
    }
    if (found) {
      if (qty.is_zero()) {
        v.erase_at(i);
      } else {
        v[i].qty = qty;
      }
      return true;
    }
    if (qty.is_zero()) return true;  // delete of an unknown level: no-op
    if (FASTMM_UNLIKELY(v.full())) {
      truncated_[static_cast<std::size_t>(side)] = true;
      if (i == 0) return false;  // worse than everything we hold
      v.erase_front();           // drop the worst level to make room
      --i;
    }
    v.insert_at(i, Level{price, qty});
    return true;
  }

  void apply_snapshot(std::span<const Level> bids,
                      std::span<const Level> asks,
                      std::uint64_t seq,
                      Timestamp ts) noexcept {
    clear();
    for (const Level& l : bids) apply_level(Side::Buy, l.price, l.qty);
    for (const Level& l : asks) apply_level(Side::Sell, l.price, l.qty);
    have_snapshot_ = true;
    seq_ = seq;
    last_update_ = ts;
    update_crossed(ts);
  }

  // Applies a BookDeltaMsg (or snapshot with kSnapshot). Uses last_update_id as seq and
  // exch_ts (falling back to recv_ts) as the update time.
  void apply_delta(const BookDeltaMsg& d) noexcept {
    const Timestamp ts = d.hdr.exch_ts.valid() ? d.hdr.exch_ts : d.hdr.recv_ts;
    if (d.is_snapshot()) {
      apply_snapshot(d.bids(), d.asks(), d.last_update_id, ts);
      return;
    }
    for (const Level& l : d.bids()) apply_level(Side::Buy, l.price, l.qty);
    for (const Level& l : d.asks()) apply_level(Side::Sell, l.price, l.qty);
    seq_ = d.last_update_id;
    last_update_ = ts;
    update_crossed(ts);
  }

  void set_seq(std::uint64_t s) noexcept { seq_ = s; }
  void set_last_update(Timestamp t) noexcept { last_update_ = t; }
  void mark_snapshot() noexcept { have_snapshot_ = true; }

  // ---- BookView --------------------------------------------------------------------------

  [[nodiscard]] FASTMM_FORCE_INLINE Level best_bid() const noexcept {
    return bids_.empty() ? Level{} : bids_.back();
  }
  [[nodiscard]] FASTMM_FORCE_INLINE Level best_ask() const noexcept {
    return asks_.empty() ? Level{} : asks_.back();
  }
  [[nodiscard]] Price mid() const noexcept {
    if (bids_.empty() || asks_.empty()) return Price{};
    return Price::from_raw((bids_.back().price.raw + asks_.back().price.raw) / 2);
  }
  [[nodiscard]] Price spread() const noexcept {
    if (bids_.empty() || asks_.empty()) return Price{};
    return asks_.back().price - bids_.back().price;
  }
  // i == 0 is best. Level{} if out of range.
  [[nodiscard]] Level level(Side side, std::size_t i) const noexcept {
    const auto& v = levels(side);
    return i < v.size() ? v[v.size() - 1 - i] : Level{};
  }
  [[nodiscard]] std::size_t depth(Side side) const noexcept { return levels(side).size(); }
  [[nodiscard]] std::size_t depth() const noexcept { return bids_.size() + asks_.size(); }

  // Total quantity on `side` at prices at-or-better than `limit`.
  [[nodiscard]] Qty qty_at_or_better(Side side, Price limit) const noexcept {
    const auto& v = levels(side);
    Qty sum{};
    for (std::size_t i = v.size(); i > 0; --i) {
      const Level& l = v[i - 1];
      if (!at_or_better(side, l.price, limit)) break;
      sum += l.qty;
    }
    return sum;
  }
  // Price of the level at which cumulative quantity from the best reaches `qty`;
  // Price{} if the side is too thin.
  [[nodiscard]] Price price_for_qty(Side side, Qty qty) const noexcept {
    const auto& v = levels(side);
    Qty cum{};
    for (std::size_t i = v.size(); i > 0; --i) {
      cum += v[i - 1].qty;
      if (cum >= qty) return v[i - 1].price;
    }
    return Price{};
  }
  // Visits levels best-first. F(const Level&) may return bool (false stops) or void.
  template <class F>
  void for_each_level(Side side, F&& f) const noexcept {
    const auto& v = levels(side);
    for (std::size_t i = v.size(); i > 0; --i) {
      if constexpr (std::is_same_v<std::invoke_result_t<F, const Level&>, bool>) {
        if (!f(v[i - 1])) return;
      } else {
        f(v[i - 1]);
      }
    }
  }
  template <std::size_t N>
  [[nodiscard]] TopLevels<N> top(Side side) const noexcept {
    TopLevels<N> out;
    const auto& v = levels(side);
    const std::size_t n = v.size() < N ? v.size() : N;
    for (std::size_t i = 0; i < n; ++i) out.levels[i] = v[v.size() - 1 - i];
    out.count = static_cast<std::uint32_t>(n);
    return out;
  }

  [[nodiscard]] bool crossed() const noexcept {
    return !bids_.empty() && !asks_.empty() && bids_.back().price >= asks_.back().price;
  }
  // How long the book has been crossed as of `now` (zero if not crossed).
  [[nodiscard]] Duration crossed_for(Timestamp now) const noexcept {
    return crossed_since_.valid() ? now - crossed_since_ : Duration{};
  }
  [[nodiscard]] bool truncated(Side side) const noexcept {
    return truncated_[static_cast<std::size_t>(side)];
  }
  [[nodiscard]] bool truncated() const noexcept { return truncated_[0] || truncated_[1]; }
  [[nodiscard]] bool has_snapshot() const noexcept { return have_snapshot_; }
  [[nodiscard]] bool is_valid() const noexcept {
    return have_snapshot_ && !bids_.empty() && !asks_.empty() && !crossed();
  }
  [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
  [[nodiscard]] Timestamp last_update() const noexcept { return last_update_; }

  // Raw access (worst..best order) for tests/tools.
  [[nodiscard]] const StaticVector<Level, kMaxDepth>& raw(Side side) const noexcept {
    return levels(side);
  }

 private:
  FASTMM_FORCE_INLINE StaticVector<Level, kMaxDepth>& levels(Side s) noexcept {
    return s == Side::Buy ? bids_ : asks_;
  }
  FASTMM_FORCE_INLINE const StaticVector<Level, kMaxDepth>& levels(Side s) const noexcept {
    return s == Side::Buy ? bids_ : asks_;
  }
  void update_crossed(Timestamp ts) noexcept {
    if (crossed()) {
      if (!crossed_since_.valid()) crossed_since_ = ts;
    } else {
      crossed_since_ = Timestamp{};
    }
  }

  StaticVector<Level, kMaxDepth> bids_;
  StaticVector<Level, kMaxDepth> asks_;
  std::uint64_t seq_ = 0;
  Timestamp last_update_{};
  Timestamp crossed_since_{};
  bool truncated_[2] = {false, false};
  bool have_snapshot_ = false;
};

static_assert(BookView<L2Book<256>>);

}  // namespace fastmm
