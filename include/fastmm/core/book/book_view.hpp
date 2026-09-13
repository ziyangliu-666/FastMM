#pragma once
// BookView: the read interface every book implementation (L2Book, L3Book::to_l2 view, sim
// book) exposes to strategies and risk. Level lives in messages.hpp because market-data
// messages carry it.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/time.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace fastmm {

// Fixed-size result of top<N>().
template <std::size_t N>
struct TopLevels {
  Level levels[N];
  std::uint32_t count = 0;
  [[nodiscard]] const Level& operator[](std::size_t i) const noexcept { return levels[i]; }
  [[nodiscard]] const Level* begin() const noexcept { return levels; }
  [[nodiscard]] const Level* end() const noexcept { return levels + count; }
};

template <class B>
concept BookView = requires(const B& b, Side s, std::size_t i, Qty q, Price p) {
  { b.best_bid() } noexcept -> std::same_as<Level>;
  { b.best_ask() } noexcept -> std::same_as<Level>;
  { b.mid() } noexcept -> std::same_as<Price>;
  { b.spread() } noexcept -> std::same_as<Price>;
  { b.level(s, i) } noexcept -> std::same_as<Level>;
  { b.depth(s) } noexcept -> std::same_as<std::size_t>;
  { b.qty_at_or_better(s, p) } noexcept -> std::same_as<Qty>;
  { b.price_for_qty(s, q) } noexcept -> std::same_as<Price>;
  { b.is_valid() } noexcept -> std::same_as<bool>;
  { b.seq() } noexcept -> std::same_as<std::uint64_t>;
  { b.last_update() } noexcept -> std::same_as<Timestamp>;
  { b.template top<4>(s) } noexcept -> std::same_as<TopLevels<4>>;
};

// true if `p` is at least as good as `q` from the point of view of side s.
[[nodiscard]] constexpr bool at_or_better(Side s, Price p, Price q) noexcept {
  return s == Side::Buy ? p >= q : p <= q;
}
[[nodiscard]] constexpr bool better(Side s, Price p, Price q) noexcept {
  return s == Side::Buy ? p > q : p < q;
}

}  // namespace fastmm
