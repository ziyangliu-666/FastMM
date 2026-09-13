#pragma once
// Time primitives. Timestamp is ns since the Unix epoch; Cycles is a raw TSC reading.
//
// TscClock converts rdtsc to wall-clock ns with a 32.32 fixed-point multiplier calibrated
// against CLOCK_REALTIME, refreshed at run time through a Seqlocked<TscCalibration> without
// letting time go backwards. On machines without constant_tsc it transparently falls back to
// clock_gettime (detected once from /proc/cpuinfo, see src/core/time.cpp). SimClock is the
// deterministic replacement for backtests/replay: same interface, time only moves when told.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/seqlock.hpp"

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

// Clock readings used by TscCalibrator. Plain function pointers so tests can inject a
// deterministic clock; system_clock_readings() reads rdtsc, CLOCK_REALTIME and CLOCK_MONOTONIC_RAW.
struct ClockReadings {
  std::uint64_t (*tsc)() noexcept = nullptr;
  std::int64_t (*realtime_ns)() noexcept = nullptr;
  std::int64_t (*monotonic_raw_ns)() noexcept = nullptr;
  bool check_invariant_tsc = true;  // false for injected clocks
};
[[nodiscard]] ClockReadings system_clock_readings() noexcept;

// One simultaneous reading of the three clocks (the TSC is the midpoint of a tight bracket).
struct TscAnchor {
  std::uint64_t tsc = 0;
  std::int64_t realtime_ns = 0;
  std::int64_t raw_ns = 0;
};

struct TscRecalibration {
  TscCalibration calibration;     // to publish
  std::int64_t drift_ns = 0;      // CLOCK_REALTIME at the new anchor minus the old mapping's value
  std::int64_t host_step_ns = 0;  // realtime change minus monotonic-raw change over the baseline
  double elapsed_s = 0.0;         // baseline length
  double rate_change_ppm = 0.0;
  bool ok = false;  // false: no TSC, or the baseline since the previous anchor is too short
};

// Long-baseline TSC calibration for the calibrator (control) thread. start() measures an initial
// rate over a short spin. update() takes only a fresh anchor and derives the rate from the whole
// interval since the previous anchor, against CLOCK_MONOTONIC_RAW:
//  * anchor jitter (clock_gettime latency spikes of tens of microseconds on a busy or virtualised
//    host) costs a few ppm over a 10 s baseline instead of thousands of ppm over a 50 ms window,
//    which used to drift the mapping by milliseconds between recalibrations and force steps;
//  * NTP slews and host wall-clock steps (WSL2 resynchronising with Windows) cannot distort the
//    rate. A wall-clock step is reported separately in host_step_ns; the published anchor still
//    follows CLOCK_REALTIME, so TscClock steps with the host when it has to.
class TscCalibrator {
 public:
  static constexpr Duration kMinBaseline = milliseconds(500);

  explicit TscCalibrator(ClockReadings readings = system_clock_readings()) noexcept
      : r_(readings) {}
  [[nodiscard]] TscCalibration start(Duration window = milliseconds(50)) noexcept;
  [[nodiscard]] TscRecalibration update() noexcept;
  [[nodiscard]] const TscCalibration& current() const noexcept { return current_; }

 private:
  [[nodiscard]] TscAnchor sample() const noexcept;
  ClockReadings r_;
  TscAnchor last_{};
  TscCalibration current_{};
};

// Outcome of TscClock::refresh() / reanchor().
enum class TscRefresh : std::uint8_t {
  None = 0,    // no new calibration published (the common case)
  Adopted,     // the clock had no TSC mapping yet: took the published one as is
  Reanchored,  // continuous: same time at the refresh point, new rate from there on
  Stepped,     // old mapping disagreed with the measurement by more than the threshold
  Rejected,    // the published calibration is unusable (no TSC); kept the current mapping
};
[[nodiscard]] constexpr const char* to_string(TscRefresh r) noexcept {
  switch (r) {
    case TscRefresh::None:
      return "none";
    case TscRefresh::Adopted:
      return "adopted";
    case TscRefresh::Reanchored:
      return "reanchored";
    case TscRefresh::Stepped:
      return "stepped";
    case TscRefresh::Rejected:
      return "rejected";
  }
  return "?";
}

// TSC reading -> wall ns under `c`. Signed, so readings taken before the anchor (e.g. a
// network-thread receive stamp converted after a re-anchor) map to earlier times.
[[nodiscard]] FASTMM_FORCE_INLINE std::int64_t tsc_to_ns(const TscCalibration& c,
                                                         std::uint64_t tsc) noexcept {
  const auto dc = static_cast<std::int64_t>(tsc - c.tsc0);
  return c.ns0 + static_cast<std::int64_t>(
                     (static_cast<Int128>(dc) * static_cast<Int128>(c.ns_per_cycle_q32)) >> 32);
}

// Periodic recalibration (see docs/architecture.md, "Clock calibration"): a calibrator thread
// measures with calibrate_tsc() and publishes into a Seqlocked<TscCalibration>; every TscClock
// user keeps its own copy and calls refresh() from its loop. A clock is owned by one thread:
// refresh() and the time accessors must not race.
class TscClock {
 public:
  static constexpr Duration kDefaultStepThreshold = milliseconds(1);
  static constexpr Duration kDefaultSlewHorizon = seconds(10);
  // Upper bound on the rate adjustment used to absorb an offset (a clock servo, not a jump).
  static constexpr std::int64_t kMaxSlewPpm = 500;

  TscClock() noexcept = default;
  explicit TscClock(const TscCalibration& c) noexcept : calib_(c) {}

  // Blocking; call once at startup, before the clock is shared with another thread.
  void calibrate(Duration window = milliseconds(50)) noexcept { calib_ = calibrate_tsc(window); }
  void set_calibration(const TscCalibration& c) noexcept { calib_ = c; }
  [[nodiscard]] const TscCalibration& calibration() const noexcept { return calib_; }

  // Subscribes to published calibrations. The next refresh() takes whatever is published
  // (even the version that was current at attach time); after that only new versions.
  // `slew_horizon` should match the calibrator's period: each re-anchor absorbs the measured
  // offset over that horizon (see reanchor()); zero disables slewing.
  void attach_calibration_source(const Seqlocked<TscCalibration>* src,
                                 Duration step_threshold = kDefaultStepThreshold,
                                 Duration slew_horizon = kDefaultSlewHorizon) noexcept {
    src_ = src;
    seen_version_ = kNoVersion;
    step_threshold_ns_ = step_threshold.ns;
    slew_horizon_ns_ = slew_horizon.ns > 0 ? slew_horizon.ns : 0;
  }
  [[nodiscard]] const Seqlocked<TscCalibration>* calibration_source() const noexcept {
    return src_;
  }

  // Cheap when nothing changed (one atomic load); copies and re-anchors on a new version.
  // A write in progress is skipped and picked up by a later call.
  FASTMM_FORCE_INLINE TscRefresh refresh() noexcept {
    if (FASTMM_LIKELY(src_ == nullptr || src_->version() == seen_version_)) return TscRefresh::None;
    return refresh_slow();
  }

  // Switches to `fresh` at TSC reading `at` (refresh() passes rdtsc()). If both mappings use
  // the TSC and agree at `at` within the step threshold, the clock re-anchors at the old
  // mapping's time for `at`, so time never jumps, and runs at the fresh rate plus a bounded
  // correction that absorbs the measured offset over the slew horizon: offset * rate / horizon,
  // so after `horizon` ns the mapping agrees with `fresh` (clamped to kMaxSlewPpm). Without the
  // correction the offset would accumulate across re-anchors until it forced a step. If the
  // offset exceeds the threshold the clock steps to `fresh` instead.
  TscRefresh reanchor(const TscCalibration& fresh, Cycles at) noexcept {
    if (!fresh.use_tsc || fresh.ns_per_cycle_q32 == 0) {
      if (calib_.use_tsc) return TscRefresh::Rejected;
      calib_ = fresh;
      return TscRefresh::Adopted;
    }
    if (!calib_.use_tsc) {
      calib_ = fresh;
      return TscRefresh::Adopted;
    }
    const std::int64_t old_ns = tsc_to_ns(calib_, at.v);
    last_offset_ns_ = tsc_to_ns(fresh, at.v) - old_ns;
    if (last_offset_ns_ > step_threshold_ns_ || last_offset_ns_ < -step_threshold_ns_) {
      calib_ = fresh;
      ++steps_;
      return TscRefresh::Stepped;
    }
    std::uint64_t rate = fresh.ns_per_cycle_q32;
    slew_ppb_ = 0;
    if (slew_horizon_ns_ > 0 && last_offset_ns_ != 0) {
      const Int128 max_adj = static_cast<Int128>(rate) * kMaxSlewPpm / 1'000'000;
      Int128 adj = static_cast<Int128>(last_offset_ns_) * static_cast<Int128>(rate) /
                   static_cast<Int128>(slew_horizon_ns_);
      if (adj > max_adj) adj = max_adj;
      if (adj < -max_adj) adj = -max_adj;
      rate = static_cast<std::uint64_t>(static_cast<Int128>(rate) + adj);
      slew_ppb_ = static_cast<std::int64_t>(adj * 1'000'000'000 /
                                            static_cast<Int128>(fresh.ns_per_cycle_q32));
    }
    calib_.tsc0 = at.v;
    calib_.ns0 = old_ns;
    calib_.ns_per_cycle_q32 = rate;
    calib_.ghz = fresh.ghz;
    ++reanchors_;
    return TscRefresh::Reanchored;
  }

  // Diagnostics (owner thread, or after the owner stopped).
  [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }
  [[nodiscard]] std::uint64_t reanchors() const noexcept { return reanchors_; }
  // fresh - old at the last re-anchor or step, in ns.
  [[nodiscard]] std::int64_t last_offset_ns() const noexcept { return last_offset_ns_; }
  [[nodiscard]] Duration step_threshold() const noexcept { return Duration{step_threshold_ns_}; }
  [[nodiscard]] Duration slew_horizon() const noexcept { return Duration{slew_horizon_ns_}; }
  // Rate correction applied at the last re-anchor, in parts per billion of the fresh rate.
  [[nodiscard]] std::int64_t slew_ppb() const noexcept { return slew_ppb_; }

  FASTMM_FORCE_INLINE Timestamp now() const noexcept {
    if (FASTMM_LIKELY(calib_.use_tsc)) return to_timestamp(rdtsc());
    return wall_now();
  }
  FASTMM_FORCE_INLINE Cycles cycles() const noexcept {
    if (FASTMM_LIKELY(calib_.use_tsc)) return rdtscp();
    return Cycles{static_cast<std::uint64_t>(wall_now().ns)};
  }
  [[nodiscard]] FASTMM_FORCE_INLINE Timestamp to_timestamp(Cycles c) const noexcept {
    if (FASTMM_LIKELY(calib_.use_tsc)) return Timestamp{tsc_to_ns(calib_, c.v)};
    return Timestamp{static_cast<std::int64_t>(c.v)};
  }
  [[nodiscard]] FASTMM_FORCE_INLINE std::int64_t cycles_to_ns(std::uint64_t dc) const noexcept {
    if (FASTMM_LIKELY(calib_.use_tsc)) {
      return static_cast<std::int64_t>((static_cast<Uint128>(dc) * calib_.ns_per_cycle_q32) >> 32);
    }
    return static_cast<std::int64_t>(dc);
  }

 private:
  static constexpr std::uint32_t kNoVersion = UINT32_MAX;

  FASTMM_NOINLINE TscRefresh refresh_slow() noexcept {
    TscCalibration fresh;
    std::uint32_t version = 0;
    if (!src_->try_load(fresh, version)) return TscRefresh::None;
    seen_version_ = version;
    return reanchor(fresh, rdtsc());
  }

  TscCalibration calib_{};
  const Seqlocked<TscCalibration>* src_ = nullptr;
  std::uint32_t seen_version_ = kNoVersion;
  std::int64_t step_threshold_ns_ = kDefaultStepThreshold.ns;
  std::int64_t slew_horizon_ns_ =
      0;  // 0 until attach_calibration_source(): reanchor() alone does not slew
  std::int64_t slew_ppb_ = 0;
  std::int64_t last_offset_ns_ = 0;
  std::uint64_t steps_ = 0;
  std::uint64_t reanchors_ = 0;
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
