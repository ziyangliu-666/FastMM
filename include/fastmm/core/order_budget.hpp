#pragma once
// OrderBudget (ctx.order_budget): what the engine's own rate limit and a venue's limits still
// admit now. The [risk] orders_per_sec bucket is the engine's; the venue's windows come from its
// connector's rate limiter (venues/rate_limiter.hpp), the limits exchangeInfo declared and the
// counts the venue's own response headers report. The connector publishes them from its thread
// through a seqlock; the engine reads the latest publication. Nothing of it is journaled, so a
// replay sees no venue budget.
#include <algorithm>
#include <cstdint>
#include <limits>

namespace fastmm {

// One fixed window of a venue's limit. `limit` 0: the venue declares no such window.
struct RateWindow {
  std::int64_t window_ms = 0;
  std::int64_t used = 0;
  std::int64_t limit = 0;
  [[nodiscard]] bool known() const noexcept { return limit > 0; }
  // What the window still admits; unlimited where the venue declares none.
  [[nodiscard]] std::int64_t remaining() const noexcept {
    return known() ? std::max<std::int64_t>(0, limit - used)
                   : std::numeric_limits<std::int64_t>::max();
  }
};

struct OrderBudget {
  static constexpr std::int64_t kUnlimited = std::numeric_limits<std::int64_t>::max();
  // Orders the [risk] orders_per_sec bucket admits now (RiskHeadroom::order_tokens); kUnlimited
  // with the limit off.
  std::int64_t local_tokens = kUnlimited;
  // The venue's order counts by window: Binance ORDERS per 10 s, 1 minute and 1 day as
  // exchangeInfo lists them, each the count the venue last reported (X-MBX-ORDER-COUNT-*) plus
  // the orders sent since.
  RateWindow orders_10s;
  RateWindow orders_1m;
  RateWindow orders_1d;
  // Request weight over the venue's shortest window (Binance REQUEST_WEIGHT per minute: 6000 on
  // Spot, 2400 on USDⓈ-M), from X-MBX-USED-WEIGHT-1M plus the requests sent since.
  RateWindow weight;
  // The connector sends nothing: the venue asked for a pause (429 Retry-After, -1003) or REST is
  // stopped (418).
  bool venue_paused = false;
  // A connector published its budget. False in a backtest, a replay and behind a gateway: every
  // venue field is then unknown and reads as unlimited.
  bool venue_known = false;

  // Orders every known limit admits now.
  [[nodiscard]] std::int64_t orders_remaining() const noexcept {
    if (venue_paused) return 0;
    return std::min(
        {local_tokens, orders_10s.remaining(), orders_1m.remaining(), orders_1d.remaining()});
  }
};

}  // namespace fastmm
