#pragma once
// Client-side model of Deribit's credit-based rate limits
// (https://docs.deribit.com/articles/rate-limits, checked 2026-09-14): every request costs credits
// from a per-sub-account pool that refills continuously ("leaky bucket") up to a maximum; "If a
// request arrives when no credits remain, we immediately send a too_many_requests (code 10028) ...
// and terminate the session."
//
//   non-matching engine requests  cost 500, pool 50,000, refill 10,000/s (20 req/s, burst 100)
//   public/private subscribe      cost 3,000, pool 30,000 (~3.3 req/s, burst 10)
//   matching engine (orders)      per tier, updated hourly: Tier 4 (default here) 5 req/s, burst
//   20;
//                                 the account's real figures are in private/get_account_summary
//                                 `limits` (not queried: configure matching_engine_rate / _burst).
//
// CreditBucket counts in credits so every bucket above is one instance: a request-per-second
// limit with a burst maps to cost 1000, pool burst * 1000, refill rate * 1000 per second.
// Allocation-free, noexcept, reactor thread only.
#include <algorithm>
#include <cstdint>

namespace fastmm::venues::deribit {

class CreditBucket {
 public:
  constexpr CreditBucket() noexcept = default;
  constexpr CreditBucket(std::int64_t max_credits, std::int64_t refill_per_second) noexcept
      : max_(max_credits), refill_per_s_(refill_per_second), credits_(max_credits) {}

  // Requests/second + burst in the matching-engine table's terms.
  [[nodiscard]] static constexpr CreditBucket from_rate(std::int64_t requests_per_second,
                                                        std::int64_t burst) noexcept {
    return {burst * kRequestCost, requests_per_second * kRequestCost};
  }
  static constexpr std::int64_t kRequestCost = 1000;

  [[nodiscard]] bool enabled() const noexcept { return max_ > 0; }

  // Credits after refilling up to `now_ns` (steady clock).
  std::int64_t available(std::int64_t now_ns) noexcept {
    refill(now_ns);
    return credits_;
  }
  // Takes `cost` credits if available; a disabled bucket always succeeds.
  bool try_consume(std::int64_t cost, std::int64_t now_ns) noexcept {
    if (!enabled()) return true;
    refill(now_ns);
    if (credits_ < cost) return false;
    credits_ -= cost;
    return true;
  }
  // Unconditional accounting (cancels are never refused locally); may go negative.
  void consume(std::int64_t cost, std::int64_t now_ns) noexcept {
    if (!enabled()) return;
    refill(now_ns);
    credits_ -= cost;
  }
  // The venue said too_many_requests: assume the pool is empty.
  void drain(std::int64_t now_ns) noexcept {
    refill(now_ns);
    credits_ = 0;
  }

  [[nodiscard]] std::int64_t max_credits() const noexcept { return max_; }
  [[nodiscard]] std::int64_t refill_per_second() const noexcept { return refill_per_s_; }

 private:
  void refill(std::int64_t now_ns) noexcept {
    if (last_ns_ == 0 || now_ns < last_ns_) {
      last_ns_ = now_ns;
      return;
    }
    if (refill_per_s_ <= 0) return;
    // Whole credits only; the remainder of the interval is kept by advancing last_ns_ exactly.
    const std::int64_t ns_per_credit =
        std::max<std::int64_t>(1, 1'000'000'000 / std::max<std::int64_t>(1, refill_per_s_));
    const std::int64_t add = (now_ns - last_ns_) / ns_per_credit;
    if (add <= 0) return;
    credits_ = std::min(max_, credits_ + add);
    last_ns_ += add * ns_per_credit;
    if (credits_ == max_) last_ns_ = now_ns;
  }

  std::int64_t max_ = 0;
  std::int64_t refill_per_s_ = 0;
  std::int64_t credits_ = 0;
  std::int64_t last_ns_ = 0;
};

}  // namespace fastmm::venues::deribit
