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
