#pragma once
// Time primitives. Timestamp is ns since the Unix epoch; Cycles is a raw TSC reading.
//
// TscClock converts rdtsc to wall-clock ns with a 32.32 fixed-point multiplier calibrated
// against CLOCK_REALTIME. On machines without constant_tsc it transparently falls back to
// clock_gettime (detected once from /proc/cpuinfo, see src/core/time.cpp). SimClock is the
// deterministic replacement for backtests/replay: same interface, time only moves when told.
#include "fastmm/core/config_macros.hpp"

#include <x86intrin.h>

#include <compare>
#include <cstdint>

namespace fastmm {

struct Duration {
  std::int64_t ns = 0;
  constexpr auto operator<=>(const Duration&) const noexcept = default;
  friend constexpr Duration operator+(Duration a, Duration b) noexcept { return {a.ns + b.ns}; }
  friend constexpr Duration operator-(Duration a, Duration b) noexcept { return {a.ns - b.ns}; }
  friend constexpr Duration operator*(Duration a, std::int64_t k) noexcept { return {a.ns * k}; }
  friend constexpr Duration operator/(Duration a, std::int64_t k) noexcept { return {a.ns / k}; }
  [[nodiscard]] constexpr std::int64_t millis() const noexcept { return ns / 1'000'000; }
  [[nodiscard]] constexpr std::int64_t micros() const noexcept { return ns / 1'000; }
};
[[nodiscard]] constexpr Duration nanoseconds(std::int64_t v) noexcept {
  return {v};
}
[[nodiscard]] constexpr Duration microseconds(std::int64_t v) noexcept {
  return {v * 1'000};
}
[[nodiscard]] constexpr Duration milliseconds(std::int64_t v) noexcept {
  return {v * 1'000'000};
}
[[nodiscard]] constexpr Duration seconds(std::int64_t v) noexcept {
  return {v * 1'000'000'000};
}

struct Timestamp {
  std::int64_t ns = 0;
  constexpr auto operator<=>(const Timestamp&) const noexcept = default;
  friend constexpr Duration operator-(Timestamp a, Timestamp b) noexcept { return {a.ns - b.ns}; }
  friend constexpr Timestamp operator+(Timestamp a, Duration d) noexcept { return {a.ns + d.ns}; }
  friend constexpr Timestamp operator-(Timestamp a, Duration d) noexcept { return {a.ns - d.ns}; }
  constexpr Timestamp& operator+=(Duration d) noexcept {
    ns += d.ns;
    return *this;
  }
  [[nodiscard]] constexpr bool valid() const noexcept { return ns != 0; }
  [[nodiscard]] static constexpr Timestamp max() noexcept { return {INT64_MAX}; }
};

struct Cycles {
  std::uint64_t v = 0;
  constexpr auto operator<=>(const Cycles&) const noexcept = default;
  friend constexpr std::uint64_t operator-(Cycles a, Cycles b) noexcept { return a.v - b.v; }
};

// Serialising read (rdtscp) is what latency hops want: it waits for prior loads to retire.
FASTMM_FORCE_INLINE Cycles rdtscp() noexcept {
  unsigned aux = 0;
  return Cycles{__rdtscp(&aux)};
}
FASTMM_FORCE_INLINE Cycles rdtsc() noexcept {
  return Cycles{__rdtsc()};
}

// CLOCK_REALTIME via vDSO (~20 ns). Implemented in src/core/time.cpp.
[[nodiscard]] Timestamp wall_now() noexcept;
[[nodiscard]] Timestamp steady_now() noexcept;
// true if /proc/cpuinfo advertises constant_tsc + nonstop_tsc (cached after first call).
[[nodiscard]] bool has_invariant_tsc() noexcept;

struct TscCalibration {
  std::uint64_t tsc0 = 0;              // TSC reading at the anchor
  std::int64_t ns0 = 0;                // wall time at the anchor
  std::uint64_t ns_per_cycle_q32 = 0;  // 32.32 fixed point
  double ghz = 0.0;                    // diagnostics only
  bool use_tsc = false;
};

// Measures the TSC frequency against CLOCK_REALTIME over `window` (spins). src/core/time.cpp
[[nodiscard]] TscCalibration calibrate_tsc(Duration window = milliseconds(50)) noexcept;

class TscClock {
 public:
  TscClock() noexcept = default;
  explicit TscClock(const TscCalibration& c) noexcept : calib_(c) {}

  // Blocking; call once at startup (or from the control thread to refresh).
  void calibrate(Duration window = milliseconds(50)) noexcept { calib_ = calibrate_tsc(window); }
  void set_calibration(const TscCalibration& c) noexcept { calib_ = c; }
  [[nodiscard]] const TscCalibration& calibration() const noexcept { return calib_; }

  FASTMM_FORCE_INLINE Timestamp now() const noexcept {
    if (FASTMM_LIKELY(calib_.use_tsc)) return to_timestamp(rdtsc());
    return wall_now();
  }
  FASTMM_FORCE_INLINE Cycles cycles() const noexcept {
    if (FASTMM_LIKELY(calib_.use_tsc)) return rdtscp();
    return Cycles{static_cast<std::uint64_t>(wall_now().ns)};
  }
  [[nodiscard]] FASTMM_FORCE_INLINE Timestamp to_timestamp(Cycles c) const noexcept {
    if (FASTMM_LIKELY(calib_.use_tsc)) {
      const std::uint64_t dc = c.v - calib_.tsc0;
      const auto dns =
          static_cast<std::int64_t>((static_cast<Uint128>(dc) * calib_.ns_per_cycle_q32) >> 32);
      return Timestamp{calib_.ns0 + dns};
    }
    return Timestamp{static_cast<std::int64_t>(c.v)};
  }
  [[nodiscard]] FASTMM_FORCE_INLINE std::int64_t cycles_to_ns(std::uint64_t dc) const noexcept {
    if (FASTMM_LIKELY(calib_.use_tsc)) {
      return static_cast<std::int64_t>((static_cast<Uint128>(dc) * calib_.ns_per_cycle_q32) >> 32);
    }
    return static_cast<std::int64_t>(dc);
  }

 private:
  TscCalibration calib_{};
};

// Deterministic clock for sim/backtest/replay. 1 cycle == 1 ns.
class SimClock {
 public:
  SimClock() noexcept = default;
  explicit SimClock(Timestamp start) noexcept : now_(start) {}
  [[nodiscard]] Timestamp now() const noexcept { return now_; }
  [[nodiscard]] Cycles cycles() const noexcept {
    return Cycles{static_cast<std::uint64_t>(now_.ns)};
  }
  [[nodiscard]] Timestamp to_timestamp(Cycles c) const noexcept {
    return Timestamp{static_cast<std::int64_t>(c.v)};
  }
  [[nodiscard]] std::int64_t cycles_to_ns(std::uint64_t dc) const noexcept {
    return static_cast<std::int64_t>(dc);
  }
  void set(Timestamp t) noexcept {
    FASTMM_ASSERT(t >= now_);
    now_ = t;
  }
  void advance(Duration d) noexcept { now_.ns += d.ns; }

 private:
  Timestamp now_{};
};

template <class C>
concept ClockLike = requires(const C& c, Cycles cy, std::uint64_t dc) {
  { c.now() } noexcept -> std::same_as<Timestamp>;
  { c.cycles() } noexcept -> std::same_as<Cycles>;
  { c.to_timestamp(cy) } noexcept -> std::same_as<Timestamp>;
  { c.cycles_to_ns(dc) } noexcept -> std::same_as<std::int64_t>;
};
static_assert(ClockLike<TscClock> && ClockLike<SimClock>);

}  // namespace fastmm
