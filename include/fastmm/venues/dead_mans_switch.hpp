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
//               CountdownSwitch below is the timer for it, CountdownDriver the rules around it.
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
        period_ns_(window_ms_ <= 0
                       ? 0
                       : std::max<std::int64_t>(
                             kMinPeriodNs,
                             window_ms_ * 1'000'000 * std::max<std::int64_t>(1, refresh_numerator) /
                                 std::max<std::int64_t>(1, refresh_denominator))) {}

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
  void attempted(std::int64_t now_ns) noexcept { last_attempt_ns_ = now_ns == 0 ? 1 : now_ns; }
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

// What a connector does with a countdown switch, the same for every venue that has one (Binance
// USDⓈ-M countdownCancelAll, OKX cancel-all-after). The connector sends the requests; this decides
// when, and what a lapse means.
//
//   Refresh  A refresh is a round of `parts` requests (one per symbol on USDⓈ-M, where the
//            countdown is per symbol; one on OKX, where it is per account). The switch counts as
//            pushed out only when every part of the round is confirmed, from the time the round
//            went out: the venue's timer starts when it receives the request, which is after
//            that, so the lapse is never declared later than the venue acts. A symbol whose
//            refresh fails is a symbol whose countdown still runs from the last round.
//   Lapse    The window ran out since the last confirmed round: the venue has cancelled our
//            orders, or is about to. `poll` reports it once; the connector kills the venue (the
//            engine must not put back quotes the venue just pulled). No refresh goes out after
//            that: whatever the local cancels do, the venue's own timer is left to run out, so
//            the account ends flat by the venue's hand even when the lapse was only lost replies.
//            A reconnect (reset) starts afresh.
//   Stop     At a requested shutdown the connector cancels its own orders, and the countdown is
//            stopped rather than left against an account nobody quotes. Needed once any refresh
//            went out, confirmed or not: a request whose reply was lost may have armed it.
class CountdownDriver {
 public:
  enum class Step : std::uint8_t { None, Refresh, Lapsed };

  CountdownDriver() = default;
  explicit CountdownDriver(std::int64_t window_ms,
                           std::int64_t refresh_numerator = 1,
                           std::int64_t refresh_denominator = 3) noexcept
      : clock_(window_ms, refresh_numerator, refresh_denominator) {}

  [[nodiscard]] bool enabled() const noexcept { return clock_.enabled(); }
  [[nodiscard]] std::int64_t window_ms() const noexcept { return clock_.window_ms(); }
  [[nodiscard]] bool lapsed() const noexcept { return lapsed_; }
  [[nodiscard]] bool ever_armed() const noexcept { return clock_.ever_armed(); }
  [[nodiscard]] const CountdownSwitch& clock() const noexcept { return clock_; }

  // From the housekeeping timer. Lapsed is returned once per session.
  [[nodiscard]] Step poll(std::int64_t now_ns) noexcept {
    if (!clock_.enabled() || lapsed_) return Step::None;
    if (clock_.expired(now_ns)) {
      lapsed_ = true;
      return Step::Lapsed;
    }
    return clock_.due(now_ns) ? Step::Refresh : Step::None;
  }

  // A refresh round of `parts` requests starts now; the token goes with each part's reply. Call
  // went_out() if at least one part was sent: a round nothing of which was sent is tried again on
  // the next tick.
  [[nodiscard]] std::uint32_t begin_round(std::int64_t now_ns, std::uint32_t parts) noexcept {
    ++round_;
    round_ns_ = now_ns == 0 ? 1 : now_ns;
    round_parts_ = parts;
    round_confirmed_ = 0;
    return round_;
  }
  void went_out() noexcept {
    clock_.attempted(round_ns_);
    ever_sent_ = true;
  }
  // The venue confirmed one part of round `round`. A reply to an older round, or one after the
  // lapse, changes nothing.
  void confirmed(std::uint32_t round) noexcept {
    if (round != round_ || lapsed_ || round_confirmed_ >= round_parts_) return;
    if (++round_confirmed_ == round_parts_) clock_.armed(round_ns_);
  }

  // Whether the shutdown has to stop the venue's countdown.
  [[nodiscard]] bool needs_stop() const noexcept { return clock_.enabled() && ever_sent_; }

  // The session ended (after the stop): the next connect starts from scratch.
  void reset() noexcept {
    clock_.disarm();
    lapsed_ = false;
    ever_sent_ = false;
    ++round_;
    round_parts_ = 0;
    round_confirmed_ = 0;
  }

 private:
  CountdownSwitch clock_;
  bool lapsed_ = false;
  bool ever_sent_ = false;
  std::uint32_t round_ = 0;
  std::uint32_t round_parts_ = 0;
  std::uint32_t round_confirmed_ = 0;
  std::int64_t round_ns_ = 0;
};

}  // namespace fastmm::venues
