#pragma once
// RiskLimits: the pre-trade limits a session runs with ([risk] in the configuration), and
// RiskHeadroom, what they still admit (RiskEngine::headroom, StrategyContext::risk_headroom).
// Separate from risk.hpp so a message can carry them (core/messages.hpp: ControlLimitsMsg, the
// operator's `limits` command) without pulling in the whole risk engine.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>
#include <limits>
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
  // Feed-lag gate (core/venue_health.hpp): while a venue's market data arrives more than this
  // much later than its baseline, its quotes are pulled and orders that could rest there without
  // reducing the position are refused (FeedLag); 0 = off.
  std::uint32_t max_feed_lag_ms = 0;

  // A limit that reads the portfolio totals ([accounting] converts them).
  [[nodiscard]] constexpr bool reads_totals() const noexcept {
    return max_loss.is_positive() || max_gross_notional.is_positive() ||
           max_net_notional.is_positive();
  }
};
static_assert(std::is_trivially_copyable_v<RiskLimits> && sizeof(RiskLimits) == 96);

// How much more each limit admits for one instrument now: the largest order (or number of orders)
// that passes that limit's check at this instant, the others aside. What check_new would use next:
// a request of exactly a room passes that check and one lot more is refused. kUnlimited where the
// limit is off. Notionals are in the reporting currency ([accounting]) where the check converts.
struct RiskHeadroom {
  static constexpr std::int64_t kUnlimited = std::numeric_limits<std::int64_t>::max();
  std::int64_t order_tokens = kUnlimited;  // orders the rate limiter admits now (orders_per_sec)
  std::int64_t open_orders = kUnlimited;   // more open orders on the instrument (max_open_orders)
  Qty max_order_qty = Qty::max();          // per order (max_order_qty)
  Notional max_order_notional = Notional::max();  // per order (max_order_notional)
  // Largest buy / sell passing max_position (position plus open orders on that side), rounded
  // down to the lot.
  Qty buy_qty = Qty::max();
  Qty sell_qty = Qty::max();
  // Exposure an order that does not reduce its position may add: max_gross_notional minus the
  // gross exposure, and per direction the net room of max_net_notional.
  Notional gross_notional = Notional::max();
  Notional net_buy_notional = Notional::max();
  Notional net_sell_notional = Notional::max();
  // max_loss plus the net PnL it is measured against; the kill switch trips at zero or below.
  Notional loss_budget = Notional::max();
};

}  // namespace fastmm
