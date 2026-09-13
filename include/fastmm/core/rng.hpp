#pragma once
// xoshiro256** - the engine's only source of randomness; seeded from config and written to
// the journal header so replays are bit-exact.
#include "fastmm/core/config_macros.hpp"

#include <cstdint>

namespace fastmm {

class Xoshiro256ss {
 public:
  explicit constexpr Xoshiro256ss(std::uint64_t seed = 0x9E3779B97F4A7C15ULL) noexcept {
    reseed(seed);
  }

  constexpr void reseed(std::uint64_t seed) noexcept {
    // splitmix64 expands the seed so that seed 0 does not produce the all-zero state.
    std::uint64_t x = seed;
    for (auto& w : s_) {
      x += 0x9E3779B97F4A7C15ULL;
      std::uint64_t z = x;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      w = z ^ (z >> 31);
    }
  }

  constexpr std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }
  constexpr std::uint64_t operator()() noexcept { return next(); }

  // Unbiased integer in [0, n) (Lemire's multiply-shift with rejection).
  constexpr std::uint64_t uniform(std::uint64_t n) noexcept {
    if (n == 0) return 0;
    for (;;) {
      const std::uint64_t x = next();
      const Uint128 m = static_cast<Uint128>(x) * n;
      const auto l = static_cast<std::uint64_t>(m);
      if (l >= (0 - n) % n) return static_cast<std::uint64_t>(m >> 64);
    }
  }
  // Integer in [lo, hi] inclusive.
  constexpr std::int64_t between(std::int64_t lo, std::int64_t hi) noexcept {
    return lo + static_cast<std::int64_t>(uniform(static_cast<std::uint64_t>(hi - lo) + 1));
  }
  // Double in [0, 1). Sim/backtest only (hot path is integer).
  double uniform01() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

  // std::uniform_random_bit_generator interface
  using result_type = std::uint64_t;
  static constexpr result_type min() noexcept { return 0; }
  static constexpr result_type max() noexcept { return UINT64_MAX; }

 private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
    return (x << k) | (x >> (64 - k));
  }
  std::uint64_t s_[4] = {};
};

}  // namespace fastmm
