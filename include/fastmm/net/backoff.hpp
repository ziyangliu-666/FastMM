#pragma once
// Exponential backoff with jitter for reconnects: delay_n = cap_n * (1 - jitter) +
// U(0, cap_n * jitter) where cap_n = min(max_ms, base_ms * 2^n). jitter = 1.0 is the
// "full jitter" scheme (recommended: spreads a thundering herd of reconnects after a venue
// outage), jitter = 0 is deterministic (useful in tests).
//
// Without a seed every object draws its own: connections that all lost the same venue spread out
// only when their jitter differs, and a shared seed made them retry in step.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>

namespace fastmm::net {

// A seed no other backoff of this process, nor of another started at another time, draws.
inline std::uint64_t backoff_seed() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  const auto now =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  // splitmix64 of the time and a per-process sequence number.
  std::uint64_t z = now + 0x9E3779B97F4A7C15ULL * (counter.fetch_add(1) + 1);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

struct BackoffConfig {
  std::uint32_t base_ms = 250;
  std::uint32_t max_ms = 30'000;
  double jitter = 1.0;  // fraction of the capped delay that is randomised, in [0, 1]
};

class ExponentialBackoff {
 public:
  explicit ExponentialBackoff(BackoffConfig cfg = {}) noexcept
      : ExponentialBackoff(cfg, backoff_seed()) {}
  ExponentialBackoff(BackoffConfig cfg, std::uint64_t seed) noexcept : cfg_(cfg), rng_(seed) {
    cfg_.jitter = std::clamp(cfg_.jitter, 0.0, 1.0);
    if (cfg_.max_ms < cfg_.base_ms) cfg_.max_ms = cfg_.base_ms;
  }

  // Delay for the current attempt in milliseconds; advances the attempt counter.
  std::uint32_t next_ms() noexcept {
    const std::uint32_t cap = capped_delay(attempt_);
    if (attempt_ < 31) ++attempt_;
    const double fixed = static_cast<double>(cap) * (1.0 - cfg_.jitter);
    const double random_span = static_cast<double>(cap) * cfg_.jitter;
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    const double delay = fixed + random_span * dist(rng_);
    return static_cast<std::uint32_t>(std::llround(delay));
  }

  void reset() noexcept { attempt_ = 0; }
  std::uint32_t attempt() const noexcept { return attempt_; }
  const BackoffConfig& config() const noexcept { return cfg_; }

  // Upper bound of the delay for attempt n (before jitter) - exposed for tests/metrics.
  std::uint32_t capped_delay(std::uint32_t attempt) const noexcept {
    const std::uint64_t raw = static_cast<std::uint64_t>(cfg_.base_ms)
                              << std::min<std::uint32_t>(attempt, 31);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(raw, cfg_.max_ms));
  }

 private:
  BackoffConfig cfg_;
  std::mt19937_64 rng_;
  std::uint32_t attempt_ = 0;
};

}  // namespace fastmm::net
