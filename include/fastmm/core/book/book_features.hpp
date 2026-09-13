#pragma once
// Integer-only book features for strategies: imbalance and microprice.
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/fixed_point.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm {

// Dimensionless ratio in 1e-8 fixed point (1.0 == 100000000).
using Ratio = Fixed<struct RatioTag>;

// (bid_qty - ask_qty) / (bid_qty + ask_qty) over the top N levels, in [-1, 1]. Zero when
// both sides are empty.
template <BookView B>
[[nodiscard]] Ratio imbalance(const B& book, std::size_t n) noexcept {
  Qty b{};
  Qty a{};
  for (std::size_t i = 0; i < n; ++i) {
    b += book.level(Side::Buy, i).qty;
    a += book.level(Side::Sell, i).qty;
  }
  const std::int64_t denom = b.raw + a.raw;
  if (denom == 0) return Ratio{};
  return Ratio::from_raw(
      static_cast<std::int64_t>(static_cast<Int128>(b.raw - a.raw) * kFixedScale / denom));
}

// Qty-weighted mid: (ask_px * bid_qty + bid_px * ask_qty) / (bid_qty + ask_qty).
// Falls back to mid() when a side is empty.
template <BookView B>
[[nodiscard]] Price microprice(const B& book) noexcept {
  const Level bb = book.best_bid();
  const Level ba = book.best_ask();
  const std::int64_t denom = bb.qty.raw + ba.qty.raw;
  if (denom == 0 || bb.price.is_zero() || ba.price.is_zero()) return book.mid();
  const Int128 num = static_cast<Int128>(ba.price.raw) * bb.qty.raw +
                     static_cast<Int128>(bb.price.raw) * ba.qty.raw;
  return Price::from_raw(static_cast<std::int64_t>(num / denom));
}

// Mid of the N-level qty-weighted average prices; a smoother "fair" reference.
template <BookView B>
[[nodiscard]] Price weighted_mid(const B& book, std::size_t n) noexcept {
  Int128 num[2] = {0, 0};
  std::int64_t den[2] = {0, 0};
  for (std::size_t i = 0; i < n; ++i) {
    const Level b = book.level(Side::Buy, i);
    const Level a = book.level(Side::Sell, i);
    num[0] += static_cast<Int128>(b.price.raw) * b.qty.raw;
    den[0] += b.qty.raw;
    num[1] += static_cast<Int128>(a.price.raw) * a.qty.raw;
    den[1] += a.qty.raw;
  }
  if (den[0] == 0 || den[1] == 0) return book.mid();
  const auto wb = static_cast<std::int64_t>(num[0] / den[0]);
  const auto wa = static_cast<std::int64_t>(num[1] / den[1]);
  return Price::from_raw((wb + wa) / 2);
}

}  // namespace fastmm
