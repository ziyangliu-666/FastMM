#include "fastmm/core/time.hpp"

#include <time.h>

#include <cstdio>
#include <cstring>

namespace fastmm {

Timestamp wall_now() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return Timestamp{static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec};
}

Timestamp steady_now() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return Timestamp{static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec};
}

namespace {

bool detect_invariant_tsc() noexcept {
  // Both flags are required: constant_tsc (rate independent of P-state) and nonstop_tsc
  // (keeps counting in deep C-states). Falls back to false on any read failure.
  std::FILE* f = std::fopen("/proc/cpuinfo", "r");
  if (f == nullptr) return false;
  bool constant = false;
  bool nonstop = false;
  char line[4096];
  while (std::fgets(line, sizeof line, f) != nullptr) {
    if (std::strncmp(line, "flags", 5) != 0) continue;
    constant = std::strstr(line, " constant_tsc") != nullptr;
    nonstop = std::strstr(line, " nonstop_tsc") != nullptr;
    break;
  }
  std::fclose(f);
  return constant && nonstop;
}

}  // namespace

bool has_invariant_tsc() noexcept {
  static const bool cached = detect_invariant_tsc();
  return cached;
}

namespace {

std::uint64_t read_tsc() noexcept {
  return __rdtsc();
}
std::int64_t read_clock_ns(clockid_t id) noexcept {
  timespec ts{};
  clock_gettime(id, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}
std::int64_t read_realtime_ns() noexcept {
  return read_clock_ns(CLOCK_REALTIME);
}
std::int64_t read_monotonic_raw_ns() noexcept {
  return read_clock_ns(CLOCK_MONOTONIC_RAW);
}

// Rate over [a, b] against the monotonic-raw clock, anchored at b on CLOCK_REALTIME.
bool rate_between(const TscAnchor& a, const TscAnchor& b, TscCalibration& out) noexcept {
  const std::int64_t dn = b.raw_ns - a.raw_ns;
  if (b.tsc <= a.tsc || dn <= 0) return false;
  const std::uint64_t dc = b.tsc - a.tsc;
  out.tsc0 = b.tsc;
  out.ns0 = b.realtime_ns;
  out.ns_per_cycle_q32 =
      static_cast<std::uint64_t>((static_cast<Uint128>(static_cast<std::uint64_t>(dn)) << 32) / dc);
  out.ghz = static_cast<double>(dc) / static_cast<double>(dn);
  out.use_tsc = out.ns_per_cycle_q32 != 0;
  return out.use_tsc;
}

}  // namespace

ClockReadings system_clock_readings() noexcept {
  return ClockReadings{&read_tsc, &read_realtime_ns, &read_monotonic_raw_ns, true};
}

TscAnchor TscCalibrator::sample() const noexcept {
  TscAnchor best{};
  std::uint64_t best_span = UINT64_MAX;
  for (int i = 0; i < 32; ++i) {
    const std::uint64_t a = r_.tsc();
    const std::int64_t wall = r_.realtime_ns();
    const std::int64_t raw = r_.monotonic_raw_ns();
    const std::uint64_t b = r_.tsc();
    if (b - a < best_span) {
      best_span = b - a;
      best = TscAnchor{a + (b - a) / 2, wall, raw};
    }
  }
  return best;
}

TscCalibration TscCalibrator::start(Duration window) noexcept {
  current_ = TscCalibration{};
  if (r_.check_invariant_tsc && !has_invariant_tsc()) return current_;
  const TscAnchor a0 = sample();
  const std::int64_t end = a0.raw_ns + window.ns;
  while (r_.monotonic_raw_ns() < end) {
    static_cast<void>(r_.tsc());
    __builtin_ia32_pause();
  }
  const TscAnchor a1 = sample();
  TscCalibration c{};
  if (rate_between(a0, a1, c)) {
    current_ = c;
    last_ = a1;
  }
  return current_;
}

TscRecalibration TscCalibrator::update() noexcept {
  TscRecalibration r;
  if (!current_.use_tsc) return r;
  const TscAnchor a = sample();
  const std::int64_t baseline = a.raw_ns - last_.raw_ns;
  if (baseline < kMinBaseline.ns) return r;
  TscCalibration fresh{};
  if (!rate_between(last_, a, fresh)) return r;
  r.drift_ns = a.realtime_ns - tsc_to_ns(current_, a.tsc);
  r.host_step_ns = (a.realtime_ns - last_.realtime_ns) - baseline;
  r.elapsed_s = static_cast<double>(baseline) / 1e9;
  r.rate_change_ppm = (static_cast<double>(fresh.ns_per_cycle_q32) -
                       static_cast<double>(current_.ns_per_cycle_q32)) /
                      static_cast<double>(current_.ns_per_cycle_q32) * 1e6;
  r.calibration = fresh;
  r.ok = true;
  current_ = fresh;
  last_ = a;
  return r;
}

TscCalibration calibrate_tsc(Duration window) noexcept {
  TscCalibration c{};
  if (!has_invariant_tsc()) {
    c.use_tsc = false;
    return c;
  }
  // Anchor: take the (wall, tsc) pair with the smallest rdtsc-clock_gettime-rdtsc bracket.
  auto sample = [](std::uint64_t& tsc, std::int64_t& ns) {
    std::uint64_t best_span = UINT64_MAX;
    for (int i = 0; i < 32; ++i) {
      const std::uint64_t a = rdtsc().v;
      const std::int64_t w = wall_now().ns;
      const std::uint64_t b = rdtsc().v;
      if (b - a < best_span) {
        best_span = b - a;
        tsc = a + (b - a) / 2;
        ns = w;
      }
    }
  };
  std::uint64_t t0 = 0;
  std::int64_t n0 = 0;
  sample(t0, n0);
  const std::int64_t end = n0 + window.ns;
  while (wall_now().ns < end) {
    __builtin_ia32_pause();
  }
  std::uint64_t t1 = 0;
  std::int64_t n1 = 0;
  sample(t1, n1);
  const std::uint64_t dc = t1 - t0;
  const std::int64_t dn = n1 - n0;
  if (dc == 0 || dn <= 0) {
    c.use_tsc = false;
    return c;
  }
  c.tsc0 = t1;
  c.ns0 = n1;
  c.ns_per_cycle_q32 =
      static_cast<std::uint64_t>((static_cast<Uint128>(static_cast<std::uint64_t>(dn)) << 32) / dc);
  c.ghz = static_cast<double>(dc) / static_cast<double>(dn);
  c.use_tsc = true;
  return c;
}

}  // namespace fastmm
