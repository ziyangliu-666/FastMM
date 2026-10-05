#pragma once
// RateLimiter (6.4/6.5): client-side accounting of venue rate limits so we refuse to send
// before the venue does. Two kinds of buckets:
//   * weight buckets  - Binance REQUEST_WEIGHT per interval (X-MBX-USED-WEIGHT-1M),
//                       Bybit per-endpoint X-Bapi-Limit / X-Bapi-Limit-Status;
//   * order-count buckets - Binance ORDERS per 10 s / per day (X-MBX-ORDER-COUNT-10S),
//                       Bybit create/cancel per second per UID.
// The venue's own counts (response headers, WS API rateLimits) correct the local estimate
// (on_headers): raised to the venue's count, and lowered to it plus what was sent in the last
// kHeaderLagNs, which the count may not include yet. Windows roll where the venue's do, on its
// clock (set_clock_offset: Binance's minute and 10 s windows start on the UTC minute and second).
// A cooldown (429 Retry-After, -1003, 10006) blocks every send until it expires, and a second one
// soon after the first waits longer each time; a hard stop (418 IP ban) blocks until explicitly
// cleared. Bulk requests (a start-up's depth snapshots and history queries, one per symbol) check
// against kBulkShare of the weight, paced over the window (kBulkPaceFloor), so the rest of the
// window stays with the orders and the bulk does not pile up at the start of each window.
// Several accounts behind one IP (an account pool) share the IP's weight: share_ip() points the
// weight buckets, the cooldown and the hard stop at one SharedRate per host (shared_rate()), so a
// 429 seen by one account pauses them all; the order-count buckets stay the account's.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace fastmm::venues {

struct SharedRate;

struct RateBucket {
  std::uint32_t limit = 0;        // 0 = unlimited
  std::int64_t window_ns = 0;     // fixed window length
  std::uint32_t used = 0;         // consumed in the current window
  std::int64_t window_start = 0;  // ns; the window rolls when now - window_start >= window_ns
  bool active = false;
  // The venue's clock minus the local one (RateLimiter::set_clock_offset): windows start where
  // now + offset is a multiple of window_ns, as the venue's do.
  std::int64_t offset = 0;

  // The start of the venue's window that holds `now`.
  [[nodiscard]] std::int64_t aligned_start(std::int64_t now) const noexcept {
    const std::int64_t r = (now + offset) % window_ns;
    return now - (r < 0 ? r + window_ns : r);
  }
  void roll(std::int64_t now) noexcept {
    if (window_ns > 0 && now - window_start >= window_ns) {
      window_start = aligned_start(now);
      used = 0;
    }
  }
  // A new clock offset: the window holding `now` starts where the venue's does. The count stays
  // (the orders in it were sent within one window length of now either way).
  void realign(std::int64_t off, std::int64_t now) noexcept {
    offset = off;
    if (window_ns <= 0) return;
    if (now - window_start >= window_ns) {
      roll(now);
    } else {
      window_start = aligned_start(now);
    }
  }
  [[nodiscard]] bool would_exceed(std::uint32_t add, double threshold) const noexcept {
    if (limit == 0) return false;
    return static_cast<double>(used + add) > static_cast<double>(limit) * threshold;
  }
  // The venue's count `count` for this window, `recent` sent in the last kHeaderLagNs: raised to
  // the count, lowered to count + recent. Ignored within `lag` of the window's start, where it may
  // be the previous window's.
  void calibrate(std::uint32_t count,
                 std::uint32_t recent,
                 std::int64_t now,
                 std::int64_t lag) noexcept {
    roll(now);
    if (window_ns > 0 && now - window_start < lag) return;
    used = std::clamp(used, count, count + recent);
  }
};

// What was sent in the last couple of seconds (RateLimiter::kHeaderLagNs), in 125 ms slots.
class RecentSpend {
 public:
  static constexpr std::size_t kSlots = 32;
  static constexpr std::int64_t kSlotNs = 125'000'000;
  void add(std::uint32_t n, std::int64_t now) noexcept {
    const std::int64_t t = now / kSlotNs;
    Slot& s = slots_[static_cast<std::size_t>(t) % kSlots];
    if (s.t != t) s = Slot{t, 0};
    s.n += n;
  }
  // Sent from `now - lag` on (whole slots: a little more, never less).
  [[nodiscard]] std::uint32_t since(std::int64_t now, std::int64_t lag) const noexcept {
    const std::int64_t from = (now - lag) / kSlotNs - 1;
    std::uint32_t sum = 0;
    for (const Slot& s : slots_) {
      if (s.t >= from && s.t <= now / kSlotNs) sum += s.n;
    }
    return sum;
  }

 private:
  struct Slot {
    std::int64_t t = -1;
    std::uint32_t n = 0;
  };
  std::array<Slot, kSlots> slots_{};
};

// Why can_send refused (RateLimiter::check): the IP or account is paused (429 cooldown, 418), a
// weight window is at its share, or an order-count window is (RateCheck::window_ns says which).
enum class RateRefusal : std::uint8_t { None, Paused, Weight, Orders };
struct RateCheck {
  RateRefusal refusal = RateRefusal::None;
  std::int64_t window_ns = 0;  // Orders: the window at its threshold
  [[nodiscard]] bool ok() const noexcept { return refusal == RateRefusal::None; }
};

class RateLimiter {
 public:
  static constexpr std::size_t kMaxBuckets = 4;
  // The part of each weight window a bulk request may fill (can_send's `share`): with 170
  // symbols' snapshots and history queries sent as fast as they fit, the other half of the window
  // still takes a cancel or a REST order.
  static constexpr double kBulkShare = 0.5;
  // A pause asked (429) within this long after the last one ended is the same incident going on:
  // each such pause waits kBackoffBaseNs << n on top of Retry-After, up to kBackoffMaxNs. The
  // venue's Retry-After names a second; a client back at once earns the next 429, then the ban.
  static constexpr std::int64_t kStreakWindowNs = 60'000'000'000;
  static constexpr std::int64_t kBackoffBaseNs = 1'000'000'000;
  static constexpr std::int64_t kBackoffMaxNs = 120'000'000'000;
  // A bulk request's share grows with the time into the window, from this part at its start to
  // all of kBulkShare at its end: a backlog of history queries waiting for room no longer spends
  // the share the moment the window opens, while the orders are busiest there.
  static constexpr double kBulkPaceFloor = 0.2;
  // How late a venue's count (on_headers) may be: what was sent in this long before it arrived
  // may be missing from it.
  static constexpr std::int64_t kHeaderLagNs = 2'000'000'000;

  explicit RateLimiter(double threshold = 0.9) noexcept : threshold_(threshold) {}

  // The IP's weight, cooldown and hard stop live in `ip` from now on (null: this limiter's own).
  void share_ip(std::shared_ptr<SharedRate> ip) noexcept;

  // Buckets are added at startup from exchangeInfo.rateLimits / the Bybit rate limit table.
  bool add_weight_bucket(std::uint32_t limit, std::int64_t window_ns) noexcept {
    if (ip_) return ip_add_weight_bucket(limit, window_ns);
    return add(weight_, limit, window_ns, offset_);
  }
  bool add_order_bucket(std::uint32_t limit, std::int64_t window_ns) noexcept {
    return add(orders_, limit, window_ns, offset_);
  }
  // The venue's clock minus the local (now) clock, ns: every window, the IP's included, rolls
  // where the venue's does from now on. 0 (the default): windows of the local clock.
  void set_clock_offset(std::int64_t venue_minus_local_ns, std::int64_t now) noexcept;
  void set_threshold(double t) noexcept { threshold_ = std::clamp(t, 0.0, 1.0); }
  // The share of each limit can_send fills before it refuses.
  [[nodiscard]] double threshold() const noexcept { return threshold_; }
  // The count a bucket of `limit` admits under that share: can_send refuses the order or request
  // that would take `used` past it (RateBucket::would_exceed).
  [[nodiscard]] std::int64_t cap_of(std::uint32_t limit) const noexcept {
    return static_cast<std::int64_t>(std::floor(static_cast<double>(limit) * threshold_));
  }

  // True if a request of `weight` (and one order if is_order) fits under threshold * limit
  // in every bucket and no cooldown/hard stop is active. `share` < 1 (kBulkShare) leaves that
  // part of each weight window to other requests.
  [[nodiscard]] bool can_send(std::uint32_t weight,
                              std::int64_t now,
                              bool is_order = false,
                              double share = 1.0) noexcept {
    return check(weight, now, is_order, share).ok();
  }
  // can_send, saying which limit refused.
  [[nodiscard]] RateCheck check(std::uint32_t weight,
                                std::int64_t now,
                                bool is_order = false,
                                double share = 1.0) noexcept {
    const RateRefusal w = ip_ ? ip_weight_fits(weight, now, share)
                              : weight_fits(weight, now, threshold_ * share, share < 1.0);
    if (w != RateRefusal::None) return {w, 0};
    if (is_order) {
      for (RateBucket& b : orders_) {
        if (!b.active) continue;
        b.roll(now);
        if (b.would_exceed(1, threshold_)) return {RateRefusal::Orders, b.window_ns};
      }
    }
    return {};
  }

  // Local accounting after a send.
  void on_sent(std::uint32_t weight, std::int64_t now, bool is_order = false) noexcept {
    if (ip_) {
      ip_on_sent(weight, now);
    } else {
      spend(weight_, weight, now);
      recent_weight_.add(weight, now);
    }
    if (is_order) {
      spend(orders_, 1, now);
      recent_orders_.add(1, now);
    }
  }

  // The venue's counts from a response (RateBucket::calibrate). `bucket` selects the window
  // (index in insertion order); negative values are ignored (header absent).
  void on_headers(std::int64_t used_weight,
                  std::int64_t order_count,
                  std::int64_t now,
                  std::size_t weight_bucket = 0,
                  std::size_t order_bucket = 0) noexcept {
    if (used_weight >= 0 && ip_) {
      ip_on_headers(used_weight, now, weight_bucket);
    } else if (used_weight >= 0 && weight_bucket < weight_.size() &&
               weight_[weight_bucket].active) {
      weight_[weight_bucket].calibrate(static_cast<std::uint32_t>(used_weight),
                                       recent_weight_.since(now, kHeaderLagNs),
                                       now,
                                       kHeaderLagNs);
    }
    if (order_count >= 0 && order_bucket < orders_.size() && orders_[order_bucket].active) {
      orders_[order_bucket].calibrate(static_cast<std::uint32_t>(order_count),
                                      recent_orders_.since(now, kHeaderLagNs),
                                      now,
                                      kHeaderLagNs);
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

  // 429 / -1003 / 10006: block sends until now + retry_after, or longer when the last pause
  // ended less than kStreakWindowNs ago (the streak). A pause asked while one is on only extends
  // it: the answers to the requests already in flight are one incident.
  void cooldown(std::int64_t retry_after_ns, std::int64_t now) noexcept {
    if (ip_) {
      ++cooldowns_;  // this account's count of the pauses it was asked for
      return ip_cooldown(retry_after_ns, now);
    }
    if (now >= cooldown_until_) {
      streak_ = cooldowns_ > 0 && now - cooldown_until_ <= kStreakWindowNs ? streak_ + 1 : 0;
    }
    const std::int64_t backoff =
        streak_ == 0 ? 0 : std::min(kBackoffMaxNs, kBackoffBaseNs << std::min(streak_, 20U));
    cooldown_until_ = std::max(cooldown_until_, now + std::max(retry_after_ns, backoff));
    ++cooldowns_;
  }
  [[nodiscard]] std::int64_t cooldown_until() const noexcept;
  // Pauses in the current streak after the first.
  [[nodiscard]] std::uint32_t streak() const noexcept;
  [[nodiscard]] bool in_cooldown(std::int64_t now) const noexcept { return now < cooldown_until(); }

  // 418: the IP is banned; stop REST entirely until an operator clears it.
  void hard_stop() noexcept;
  void clear_hard_stop() noexcept;
  [[nodiscard]] bool hard_stopped() const noexcept;

  [[nodiscard]] std::uint64_t cooldowns() const noexcept { return cooldowns_; }
  // Copies of the i-th bucket (insertion order), empty past the last; a shared IP's is copied
  // under its lock, since the other accounts' threads write it.
  [[nodiscard]] std::optional<RateBucket> weight_bucket(std::size_t i) const noexcept;
  [[nodiscard]] std::optional<RateBucket> order_bucket(std::size_t i) const noexcept {
    if (i < orders_.size() && orders_[i].active) return orders_[i];
    return std::nullopt;
  }

 private:
  static bool add(std::array<RateBucket, kMaxBuckets>& arr,
                  std::uint32_t limit,
                  std::int64_t window_ns,
                  std::int64_t offset) noexcept {
    for (RateBucket& b : arr) {
      if (b.active) continue;
      b = RateBucket{limit, window_ns, 0, 0, true, offset};
      return true;
    }
    return false;
  }

  static void spend(std::array<RateBucket, kMaxBuckets>& arr,
                    std::uint32_t n,
                    std::int64_t now) noexcept {
    for (RateBucket& b : arr) {
      if (!b.active) continue;
      b.roll(now);
      b.used += n;
    }
  }
  // This limiter's own weight: no cooldown or hard stop, and `weight` under threshold * limit;
  // `paced` (a bulk request) under the part of it the time into the window allows
  // (kBulkPaceFloor). A shared IP calls it on SharedRate::r under the lock.
  RateRefusal weight_fits(std::uint32_t weight,
                          std::int64_t now,
                          double threshold,
                          bool paced) noexcept {
    if (hard_stopped_ || now < cooldown_until_) return RateRefusal::Paused;
    for (RateBucket& b : weight_) {
      if (!b.active) continue;
      b.roll(now);
      double t = threshold;
      if (paced && b.window_ns > 0) {
        const double into =
            static_cast<double>(now - b.window_start) / static_cast<double>(b.window_ns);
        t *= std::clamp(into, kBulkPaceFloor, 1.0);
      }
      if (b.would_exceed(weight, t)) return RateRefusal::Weight;
    }
    return RateRefusal::None;
  }
  // Adds a weight bucket unless one of the same limit and window is there: each account that
  // shares an IP adds the same buckets, in the same order (on_headers' index).
  bool add_weight_bucket_once(std::uint32_t limit, std::int64_t window_ns) noexcept {
    for (const RateBucket& b : weight_) {
      if (b.active && b.limit == limit && b.window_ns == window_ns) return true;
    }
    return add(weight_, limit, window_ns, offset_);
  }

  // The IP-wide parts, under SharedRate's mutex (defined below).
  RateRefusal ip_weight_fits(std::uint32_t weight, std::int64_t now, double share) noexcept;
  void ip_on_sent(std::uint32_t weight, std::int64_t now) noexcept;
  void ip_on_headers(std::int64_t used, std::int64_t now, std::size_t bucket) noexcept;
  bool ip_add_weight_bucket(std::uint32_t limit, std::int64_t window_ns) noexcept;
  void ip_cooldown(std::int64_t retry_after_ns, std::int64_t now) noexcept;

  double threshold_;
  std::shared_ptr<SharedRate> ip_;
  std::array<RateBucket, kMaxBuckets> weight_{};
  std::array<RateBucket, kMaxBuckets> orders_{};
  RecentSpend recent_weight_;  // this limiter's own weight (a shared IP's lives in its limiter)
  RecentSpend recent_orders_;
  std::int64_t offset_ = 0;  // set_clock_offset
  std::int64_t cooldown_until_ = 0;
  std::uint64_t cooldowns_ = 0;
  std::uint32_t streak_ = 0;
  bool hard_stopped_ = false;
};

// One IP's request weight, shared by every account that sends from it (RateLimiter::share_ip).
struct SharedRate {
  std::mutex m;
  RateLimiter r{1.0};  // its weight buckets, cooldown and hard stop; never shared itself
};

// The SharedRate of `key` (a REST host), made on first use; process-wide.
inline std::shared_ptr<SharedRate> shared_rate(std::string_view key) {
  static std::mutex m;
  static std::map<std::string, std::shared_ptr<SharedRate>, std::less<>> by_key;
  const std::lock_guard<std::mutex> lock(m);
  auto it = by_key.find(key);
  if (it == by_key.end())
    it = by_key.emplace(std::string(key), std::make_shared<SharedRate>()).first;
  return it->second;
}

inline void RateLimiter::share_ip(std::shared_ptr<SharedRate> ip) noexcept {
  if (ip) {
    const std::lock_guard<std::mutex> lock(ip->m);
    for (const RateBucket& b : weight_) {
      if (b.active) static_cast<void>(ip->r.add_weight_bucket_once(b.limit, b.window_ns));
    }
  }
  ip_ = std::move(ip);
}
inline RateRefusal RateLimiter::ip_weight_fits(std::uint32_t weight,
                                               std::int64_t now,
                                               double share) noexcept {
  const std::lock_guard<std::mutex> lock(ip_->m);
  return ip_->r.weight_fits(weight, now, threshold_ * share, share < 1.0);
}
inline void RateLimiter::ip_on_sent(std::uint32_t weight, std::int64_t now) noexcept {
  const std::lock_guard<std::mutex> lock(ip_->m);
  spend(ip_->r.weight_, weight, now);
  ip_->r.recent_weight_.add(weight, now);
}
inline void RateLimiter::ip_on_headers(std::int64_t used,
                                       std::int64_t now,
                                       std::size_t bucket) noexcept {
  // The IP's count when the venue answered one account: every account's requests of the last
  // kHeaderLagNs may be missing from it, so it lowers the shared estimate only to the count plus
  // those (an older answer lowering it further would let their concurrent burst through).
  const std::lock_guard<std::mutex> lock(ip_->m);
  auto& w = ip_->r.weight_;
  if (bucket >= w.size() || !w[bucket].active) return;
  w[bucket].calibrate(static_cast<std::uint32_t>(used),
                      ip_->r.recent_weight_.since(now, kHeaderLagNs),
                      now,
                      kHeaderLagNs);
}
inline void RateLimiter::set_clock_offset(std::int64_t venue_minus_local_ns,
                                          std::int64_t now) noexcept {
  offset_ = venue_minus_local_ns;
  for (RateBucket& b : orders_) {
    if (b.active) b.realign(offset_, now);
  }
  if (!ip_) {
    for (RateBucket& b : weight_) {
      if (b.active) b.realign(offset_, now);
    }
    return;
  }
  const std::lock_guard<std::mutex> lock(ip_->m);
  ip_->r.offset_ = offset_;
  for (RateBucket& b : ip_->r.weight_) {
    if (b.active) b.realign(offset_, now);
  }
}
inline bool RateLimiter::ip_add_weight_bucket(std::uint32_t limit,
                                              std::int64_t window_ns) noexcept {
  const std::lock_guard<std::mutex> lock(ip_->m);
  return ip_->r.add_weight_bucket_once(limit, window_ns);
}
inline void RateLimiter::ip_cooldown(std::int64_t retry_after_ns, std::int64_t now) noexcept {
  const std::lock_guard<std::mutex> lock(ip_->m);
  ip_->r.cooldown(retry_after_ns, now);
}
inline std::int64_t RateLimiter::cooldown_until() const noexcept {
  if (!ip_) return cooldown_until_;
  const std::lock_guard<std::mutex> lock(ip_->m);
  return ip_->r.cooldown_until_;
}
inline void RateLimiter::hard_stop() noexcept {
  if (!ip_) {
    hard_stopped_ = true;
    return;
  }
  const std::lock_guard<std::mutex> lock(ip_->m);
  ip_->r.hard_stopped_ = true;
}
inline void RateLimiter::clear_hard_stop() noexcept {
  if (!ip_) {
    hard_stopped_ = false;
    return;
  }
  const std::lock_guard<std::mutex> lock(ip_->m);
  ip_->r.hard_stopped_ = false;
}
inline std::optional<RateBucket> RateLimiter::weight_bucket(std::size_t i) const noexcept {
  if (!ip_) {
    if (i < weight_.size() && weight_[i].active) return weight_[i];
    return std::nullopt;
  }
  const std::lock_guard<std::mutex> lock(ip_->m);
  return ip_->r.weight_bucket(i);
}
inline std::uint32_t RateLimiter::streak() const noexcept {
  if (!ip_) return streak_;
  const std::lock_guard<std::mutex> lock(ip_->m);
  return ip_->r.streak_;
}
inline bool RateLimiter::hard_stopped() const noexcept {
  if (!ip_) return hard_stopped_;
  const std::lock_guard<std::mutex> lock(ip_->m);
  return ip_->r.hard_stopped_;
}

// A blocking start-up request (reference data, account settings, one per symbol) waits for its
// weight to fit the bulk share instead of spending what the connections about to open will need;
// at most `max_wait_ns`, since nothing trades yet and a window is a minute.
template <class Sleep>
void wait_for_weight(RateLimiter& r,
                     std::uint32_t weight,
                     std::int64_t now_ns,
                     std::int64_t max_wait_ns,
                     Sleep&& sleep_ns) noexcept {
  const std::int64_t until = now_ns + max_wait_ns;
  std::int64_t now = now_ns;
  while (now < until && !r.can_send(weight, now, false, RateLimiter::kBulkShare)) {
    constexpr std::int64_t kStep = 100'000'000;
    sleep_ns(kStep);
    now += kStep;
  }
}

}  // namespace fastmm::venues
