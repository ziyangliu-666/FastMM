#pragma once
// The Bybit v5 product category one connector instance trades (`[venues.<name>] category`).
// Every v5 request that takes `category` gets this one, and the private topics are filtered by it.
//
//   spot    spot pairs
//   linear  USDT- and USDC-margined perpetuals (contractType LinearPerpetual), one-way position
//           mode only
#include <cstdint>
#include <optional>
#include <string_view>

namespace fastmm::venues::bybit {

enum class BybitCategory : std::uint8_t { Spot = 0, Linear = 1 };

[[nodiscard]] constexpr std::string_view to_string(BybitCategory c) noexcept {
  return c == BybitCategory::Linear ? "linear" : "spot";
}

[[nodiscard]] constexpr std::optional<BybitCategory> parse_category(std::string_view s) noexcept {
  if (s.empty() || s == "spot") return BybitCategory::Spot;
  if (s == "linear") return BybitCategory::Linear;
  return std::nullopt;
}

// POST /v5/order/disconnected-cancel-all `product`, and the private topic that makes DCP fire
// for it (https://bybit-exchange.github.io/docs/v5/order/dcp and .../websocket/private/dcp).
[[nodiscard]] constexpr std::string_view dcp_product(BybitCategory c) noexcept {
  return c == BybitCategory::Linear ? "DERIVATIVES" : "SPOT";
}
[[nodiscard]] constexpr std::string_view dcp_topic(BybitCategory c) noexcept {
  return c == BybitCategory::Linear ? "dcp.future" : "dcp.spot";
}

}  // namespace fastmm::venues::bybit
