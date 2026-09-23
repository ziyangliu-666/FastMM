#pragma once
// Venue-side dead man's switch: the venue cancels the account's resting orders when the
// connector stops talking to it, whatever the reason. `cancel_on_order_channel_loss` is not
// this — that one is the connector issuing a REST cancel-all, and it needs a live process and a
// working network. A SIGKILL, an OOM kill, a kernel panic or a host that loses power leaves the
// quotes resting. Only the venue can clear them then.
//
// The venues offer two shapes, and only the first needs anything here:
//
//   countdown   The connector arms a timer ("cancel everything unless you hear from me within
//               N ms") and keeps pushing it out. Binance USDⓈ-M countdownCancelAll. The window
//               is the exposure after a hard kill, and every refresh costs rate limit, so the
//               refresh runs at a fraction of the window rather than at its edge.
//               CountdownSwitch below is the timer for it.
//   on-disconnect  The venue watches its own socket and cancels when it dies. Deribit
//               private/enable_cancel_on_disconnect, Bybit's Disconnect-Cancel-All. Armed once
//               per connection or once per account; there is nothing to refresh, so these are
//               wired in the connectors directly.
//
// Binance Spot has neither, as of the 2026-09 API docs: neither rest-api.md nor
// web-socket-api.md has a countdown, a session auto-cancel or a cancel-on-disconnect, and the
// FIX session's "countdown" is a maintenance logout that leaves orders alone. The only
// venue-side primitive is the manual openOrders.cancelAll. Spot quotes survive a hard kill.
#include <algorithm>
#include <cstdint>

namespace fastmm::venues {

// Refresh clock for a countdown dead man's switch. Holds no venue detail: the connector asks
// `due()` from its housekeeping timer, sends its own request, reports `attempted()`, and reports
// `armed()` when the venue answers.
//
// The two clocks are separate on purpose. `attempted` paces retries, so a refresh that never
// reaches the venue is tried again next period instead of every tick. `armed` is what the window
// is measured from, so a request that goes out and comes back an error does not pretend the
// countdown was pushed out. If this process is what broke, neither clock moves, the venue's
// countdown runs out and it cancels — which is the point.
class CountdownSwitch {
 public:
  CountdownSwitch() = default;
  // `window_ms` is what the venue is told; 0 disables the switch. `refresh_numerator` /
  // `refresh_denominator` is the fraction of the window at which the refresh goes out
  // (1/3 by default: two refreshes may be lost before the venue acts).
  explicit CountdownSwitch(std::int64_t window_ms,
                           std::int64_t refresh_numerator = 1,
                           std::int64_t refresh_denominator = 3) noexcept
      : window_ms_(std::max<std::int64_t>(0, window_ms)),
        period_ns_(window_ms_ <= 0 ? 0
                                   : std::max<std::int64_t>(kMinPeriodNs,
                                                            window_ms_ * 1'000'000 *
                                                                std::max<std::int64_t>(
                                                                    1, refresh_numerator) /
                                                                std::max<std::int64_t>(
                                                                    1, refresh_denominator))) {}

  // A refresh sent more often than this is wasted rate limit whatever the window is.
  static constexpr std::int64_t kMinPeriodNs = 500'000'000;

  [[nodiscard]] bool enabled() const noexcept { return window_ms_ > 0; }
  [[nodiscard]] std::int64_t window_ms() const noexcept { return window_ms_; }
  [[nodiscard]] std::int64_t refresh_period_ns() const noexcept { return period_ns_; }

  // True when a refresh has to go out now. Also true for the first arm, and true again a refresh
  // period after an attempt that the venue did not confirm, so a failure is retried.
  [[nodiscard]] bool due(std::int64_t now_ns) const noexcept {
    return enabled() && (last_attempt_ns_ == 0 || now_ns - last_attempt_ns_ >= period_ns_);
  }
  // A refresh was written to the venue. Only stops due() from firing again immediately; it does
  // not mean the countdown is running.
  void attempted(std::int64_t now_ns) noexcept {
    last_attempt_ns_ = now_ns == 0 ? 1 : now_ns;
  }
  // The venue accepted the refresh: the countdown is running from here.
  void armed(std::int64_t now_ns) noexcept { last_armed_ns_ = now_ns == 0 ? 1 : now_ns; }

  // The window has run out since the last refresh the venue confirmed: it has cancelled our
  // orders, or is about to. This process is still alive (it is asking), so something between it
  // and the venue is broken. It must not simply put back the quotes the venue just pulled —
  // that is a quoter racing a kill switch it cannot see. A switch that was up and has lapsed is
  // a kill. One that was never up is not: the connector is loud about those failures instead,
  // because a venue that refuses the request outright must not stop the session dead.
  [[nodiscard]] bool expired(std::int64_t now_ns) const noexcept {
    return enabled() && last_armed_ns_ != 0 && now_ns - last_armed_ns_ >= window_ms_ * 1'000'000;
  }
  [[nodiscard]] bool ever_armed() const noexcept { return last_armed_ns_ != 0; }

  // The session ended, or the kill already fired: the next due() starts from scratch.
  void disarm() noexcept {
    last_attempt_ns_ = 0;
    last_armed_ns_ = 0;
  }

 private:
  std::int64_t window_ms_ = 0;
  std::int64_t period_ns_ = 0;
  std::int64_t last_attempt_ns_ = 0;
  std::int64_t last_armed_ns_ = 0;
};

}  // namespace fastmm::venues
