#pragma once
// FeeModel: maker/taker fees in centi-basis-points (integer), applied to the fill notional.
// Negative maker fee == rebate. Config doubles (bps) are converted exactly once.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"

#include <cstdint>

namespace fastmm::sim {

struct FeeModel {
  std::int64_t maker_cbps = 0;  // 1 cbps == 0.01 bps
  std::int64_t taker_cbps = 0;

  [[nodiscard]] static FeeModel from_bps(double maker_bps, double taker_bps) noexcept {
    FeeModel f;
    f.maker_cbps = static_cast<std::int64_t>(maker_bps * 100.0 + (maker_bps < 0 ? -0.5 : 0.5));
    f.taker_cbps = static_cast<std::int64_t>(taker_bps * 100.0 + (taker_bps < 0 ? -0.5 : 0.5));
    return f;
  }
  [[nodiscard]] constexpr Notional fee(Notional notional, Liquidity l) const noexcept {
    const std::int64_t cbps = l == Liquidity::Taker ? taker_cbps : maker_cbps;
    return Notional::from_raw(
        static_cast<std::int64_t>(static_cast<Int128>(notional.raw) * cbps / 1'000'000));
  }
  [[nodiscard]] constexpr Notional fee(Price px, Qty qty, Liquidity l) const noexcept {
    return fee(mul(px, qty), l);
  }
};

}  // namespace fastmm::sim
