#pragma once
// How many cores a benchmark needs, declared by the benchmark itself.
//
// scripts/bench.sh pins benchmarks to one core so the numbers are stable. A benchmark that runs a
// second thread and waits for it (bench_reactor's threaded round trip) then measures the scheduler
// time-slicing two runnable threads on one core, not the code: pinned, BM_ReactorEchoThread reports
// 8 ms per round trip against 10 us unpinned.
//
// A benchmark that needs more than one core declares it with FASTMM_BENCH_NEEDS_CORES. The shared
// bench main then
//   * prints those names, as a Google Benchmark filter, for `--fastmm-multicore-filter`, which
//     scripts/bench.sh uses to run them in a second, unpinned pass; and
//   * lets the benchmark check its own affinity mask with fastmm::bench::affinity_cores(), so a run
//     that was pinned anyway fails instead of publishing a number that measures the scheduler.
#include <cstddef>

namespace fastmm::bench {

// Number of cores in this process's CPU affinity mask (0 if it cannot be read).
[[nodiscard]] int affinity_cores() noexcept;

void register_multicore(const char* name, int cores);

struct MulticoreRegistrar {
  MulticoreRegistrar(const char* name, int cores) { register_multicore(name, cores); }
};

}  // namespace fastmm::bench

// Declares that `name` (a benchmark function name; all its arguments are covered) needs `cores`
// cores. Place it next to the BENCHMARK() registration.
#define FASTMM_BENCH_NEEDS_CORES(name, cores)                              \
  static const ::fastmm::bench::MulticoreRegistrar name##_needs_cores_reg { \
    #name, (cores)                                                         \
  }
