#pragma once
// RestingOrders: the orders of every strategy attached to fastmm-gateway that can rest in one
// shared instrument ([gateway.shared]), per side, for the gateway's self-trade check: an order that
// would trade with another strategy's resting order is refused before it reaches the venue. Its
// own orders are the engine's concern ([risk] stp). A handful of entries a side, so a linear scan;
// the gateway reserves room up front, so the hot path does not allocate. Single-threaded: it
// belongs to the venue's network thread.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/strong_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fastmm {

class RestingOrders {
 public:
  void reserve(std::size_t per_side) {
    for (auto& s : sides_) s.reserve(per_side);
  }
  // `id` rests on `side` at `px` for the strategy of session `epoch` (again: its entry moves).
  void set(ClientOrderId id, Side side, Price px, std::uint16_t epoch) {
    remove(id, side);
    sides_[index(side)].push_back(Entry{id, px.raw, epoch});
  }
  void remove(ClientOrderId id, Side side) noexcept {
    auto& list = sides_[index(side)];
    for (std::size_t k = 0; k < list.size(); ++k) {
      if (list[k].id != id) continue;
      list[k] = list.back();
      list.pop_back();
      return;
    }
  }
  // Would an order of `epoch` trade with a resting order of another epoch: a buy at or above its
  // sell, a sell at or below its buy, a market order against any?
  [[nodiscard]] bool crosses(std::uint16_t epoch, Side side, Price px, bool market) const noexcept {
    const bool buy = side == Side::Buy;
    for (const Entry& e : sides_[buy ? index(Side::Sell) : index(Side::Buy)]) {
      if (e.epoch == epoch) continue;
      if (market || (buy ? px.raw >= e.px : px.raw <= e.px)) return true;
    }
    return false;
  }
  [[nodiscard]] std::size_t size(Side side) const noexcept { return sides_[index(side)].size(); }

 private:
  struct Entry {
    ClientOrderId id;
    std::int64_t px = 0;  // raw Price
    std::uint16_t epoch = 0;
  };
  static constexpr std::size_t index(Side s) noexcept { return static_cast<std::size_t>(s); }
  std::array<std::vector<Entry>, 2> sides_;
};

}  // namespace fastmm
