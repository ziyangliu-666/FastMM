#pragma once
// RateLimiter (6.4/6.5): client-side accounting of venue rate limits so we refuse to send
// before the venue does. Two kinds of buckets:
//   * weight buckets  - Binance REQUEST_WEIGHT per interval (X-MBX-USED-WEIGHT-1M),
//                       Bybit per-endpoint X-Bapi-Limit / X-Bapi-Limit-Status;
//   * order-count buckets - Binance ORDERS per 10 s / per day (X-MBX-ORDER-COUNT-10S),
//                       Bybit create/cancel per second per UID.
// The venue's own headers are authoritative: on_headers() overwrites the local estimate.
// A cooldown (429 Retry-After, -1003, 10006) blocks every send until it expires; a hard
// stop (418 IP ban) blocks until explicitly cleared.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace fastmm::venues {

struct RateBucket {
  std::uint32_t limit = 0;        // 0 = unlimited
  std::int64_t window_ns = 0;     // fixed window length
  std::uint32_t used = 0;         // consumed in the current window
  std::int64_t window_start = 0;  // ns; the window rolls when now - window_start >= window_ns
  bool active = false;

  void roll(std::int64_t now) noexcept {
    if (window_ns > 0 && now - window_start >= window_ns) {
      // Fixed windows aligned to the epoch of the first use (Binance aligns to the minute;
      // we cannot see its clock, so the local window is conservative: it never resets late).
      const std::int64_t elapsed = now - window_start;
      window_start += (elapsed / window_ns) * window_ns;
      used = 0;
    }
  }
  [[nodiscard]] bool would_exceed(std::uint32_t add, double threshold) const noexcept {
    if (limit == 0) return false;
    return static_cast<double>(used + add) > static_cast<double>(limit) * threshold;
  }
};

class RateLimiter {
 public:
  static constexpr std::size_t kMaxBuckets = 4;

  explicit RateLimiter(double threshold = 0.9) noexcept : threshold_(threshold) {}

  // Buckets are added at startup from exchangeInfo.rateLimits / the Bybit rate limit table.
  bool add_weight_bucket(std::uint32_t limit, std::int64_t window_ns) noexcept {
    return add(weight_, limit, window_ns);
  }
  bool add_order_bucket(std::uint32_t limit, std::int64_t window_ns) noexcept {
    return add(orders_, limit, window_ns);
  }
  void set_threshold(double t) noexcept { threshold_ = std::clamp(t, 0.0, 1.0); }

  // True if a request of `weight` (and one order if is_order) fits under threshold * limit
  // in every bucket and no cooldown/hard stop is active.
  [[nodiscard]] bool can_send(std::uint32_t weight,
                              std::int64_t now,
                              bool is_order = false) noexcept {
    if (hard_stopped_ || now < cooldown_until_) return false;
    for (RateBucket& b : weight_) {
      if (!b.active) continue;
      b.roll(now);
      if (b.would_exceed(weight, threshold_)) return false;
    }
    if (is_order) {
      for (RateBucket& b : orders_) {
        if (!b.active) continue;
        b.roll(now);
        if (b.would_exceed(1, threshold_)) return false;
      }
    }
    return true;
  }

  // Local accounting after a send.
  void on_sent(std::uint32_t weight, std::int64_t now, bool is_order = false) noexcept {
    for (RateBucket& b : weight_) {
      if (!b.active) continue;
      b.roll(now);
      b.used += weight;
    }
    if (is_order) {
      for (RateBucket& b : orders_) {
        if (!b.active) continue;
        b.roll(now);
        b.used += 1;
      }
    }
  }

  // Authoritative counters from response headers. `bucket` selects the window (index in
  // insertion order); negative values are ignored (header absent).
  void on_headers(std::int64_t used_weight,
                  std::int64_t order_count,
                  std::int64_t now,
                  std::size_t weight_bucket = 0,
                  std::size_t order_bucket = 0) noexcept {
    if (used_weight >= 0 && weight_bucket < weight_.size() && weight_[weight_bucket].active) {
      RateBucket& b = weight_[weight_bucket];
      b.roll(now);
      b.used = static_cast<std::uint32_t>(used_weight);
    }
    if (order_count >= 0 && order_bucket < orders_.size() && orders_[order_bucket].active) {
      RateBucket& b = orders_[order_bucket];
      b.roll(now);
      b.used = static_cast<std::uint32_t>(order_count);
    }
  }
  // Bybit style: remaining requests for this endpoint (X-Bapi-Limit-Status) and the limit.
  void on_remaining(std::int64_t limit, std::int64_t remaining, std::int64_t now) noexcept {
    if (limit < 0 || remaining < 0 || weight_.empty() || !weight_[0].active) return;
    RateBucket& b = weight_[0];
    b.roll(now);
    b.limit = static_cast<std::uint32_t>(limit);
    b.used = static_cast<std::uint32_t>(std::max<std::int64_t>(0, limit - remaining));
  }

  // 429 / -1003 / 10006: block sends until now + retry_after.
  void cooldown(std::int64_t retry_after_ns, std::int64_t now) noexcept {
    cooldown_until_ = std::max(cooldown_until_, now + retry_after_ns);
    ++cooldowns_;
  }
  [[nodiscard]] std::int64_t cooldown_until() const noexcept { return cooldown_until_; }
  [[nodiscard]] bool in_cooldown(std::int64_t now) const noexcept { return now < cooldown_until_; }

  // 418: the IP is banned; stop REST entirely until an operator clears it.
  void hard_stop() noexcept { hard_stopped_ = true; }
  void clear_hard_stop() noexcept { hard_stopped_ = false; }
  [[nodiscard]] bool hard_stopped() const noexcept { return hard_stopped_; }

  [[nodiscard]] std::uint64_t cooldowns() const noexcept { return cooldowns_; }
  [[nodiscard]] const RateBucket* weight_bucket(std::size_t i) const noexcept {
    return i < weight_.size() && weight_[i].active ? &weight_[i] : nullptr;
  }
  [[nodiscard]] const RateBucket* order_bucket(std::size_t i) const noexcept {
    return i < orders_.size() && orders_[i].active ? &orders_[i] : nullptr;
  }

 private:
  static bool add(std::array<RateBucket, kMaxBuckets>& arr,
                  std::uint32_t limit,
                  std::int64_t window_ns) noexcept {
    for (RateBucket& b : arr) {
      if (b.active) continue;
      b = RateBucket{limit, window_ns, 0, 0, true};
      return true;
    }
    return false;
  }

  double threshold_;
  std::array<RateBucket, kMaxBuckets> weight_{};
  std::array<RateBucket, kMaxBuckets> orders_{};
  std::int64_t cooldown_until_ = 0;
  std::uint64_t cooldowns_ = 0;
  bool hard_stopped_ = false;
};

}  // namespace fastmm::venues
