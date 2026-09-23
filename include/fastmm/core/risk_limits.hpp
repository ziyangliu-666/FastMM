#pragma once
// RiskLimits: the pre-trade limits a session runs with ([risk] in the configuration). Separate
// from risk.hpp so a message can carry them (core/messages.hpp: ControlLimitsMsg, the operator's
// `limits` command) without pulling in the whole risk engine.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>
#include <type_traits>

namespace fastmm {

struct RiskLimits {
  Qty max_order_qty{};            // 0 = unlimited
  Notional max_order_notional{};  // 0 = unlimited
  Qty max_position{};             // absolute, predicted (position + same-side open); 0 = unlimited
  std::uint32_t max_open_orders = 0;  // per instrument; 0 = unlimited
  std::int64_t price_collar_bps = 0;  // vs mid; 0 = disabled
  std::int64_t fat_finger_bps = 0;    // vs last trade; 0 = disabled
  Duration stale_md{};                // reject if book older than this; 0 = disabled
  // Portfolio exposure at the last marks, over every instrument; 0 = unlimited. Gross is the sum
  // of |position|, net the signed sum: a hedged pair is small in net and large in gross.
  Notional max_gross_notional{};
  Notional max_net_notional{};
  Notional max_loss{};               // trip when net pnl <= -max_loss; 0 = disabled
  std::uint32_t orders_per_sec = 0;  // token bucket rate; 0 = unlimited
  std::uint32_t burst = 0;           // bucket capacity (defaults to orders_per_sec)
  bool stp = true;                   // self-trade prevention against our own resting orders
};
static_assert(std::is_trivially_copyable_v<RiskLimits> && sizeof(RiskLimits) == 96);

}  // namespace fastmm
