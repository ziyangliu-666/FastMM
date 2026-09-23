// Shared benchmark main: pins the process to a core (--cpu=N or FASTMM_BENCH_CPU) before
// running so results are stable, then delegates to Google Benchmark.
//
// Benchmarks that need more than one core declare it with FASTMM_BENCH_NEEDS_CORES (bench_pin.hpp).
// `--fastmm-multicore-filter` prints them as a Google Benchmark filter and exits; scripts/bench.sh
// excludes them from the pinned pass and runs them unpinned in a second pass.
#include "bench_pin.hpp"

#include <benchmark/benchmark.h>

#include <sched.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fastmm::bench {
namespace {
std::vector<const char*>& multicore_names() {
  static std::vector<const char*> v;
  return v;
}
}  // namespace

void register_multicore(const char* name, int cores) {
  if (cores > 1) multicore_names().push_back(name);
}

int affinity_cores() noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) return 0;
  return CPU_COUNT(&set);
}

}  // namespace fastmm::bench

namespace {

void pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0)
    std::fprintf(stderr, "warning: cannot pin to cpu %d\n", cpu);
}

// "BM_A|BM_B" for the benchmarks that need more than one core; empty when there are none.
std::string multicore_filter() {
  std::string out;
  for (const char* n : fastmm::bench::multicore_names()) {
    if (!out.empty()) out += '|';
    out += n;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--fastmm-multicore-filter") == 0) {
      const std::string f = multicore_filter();
      if (!f.empty()) std::printf("%s\n", f.c_str());
      return 0;
    }
  }
  int cpu = -1;
  if (const char* e = std::getenv("FASTMM_BENCH_CPU")) cpu = std::atoi(e);
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--cpu=", 6) == 0) {
      cpu = std::atoi(argv[i] + 6);
      for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
      --argc;
      break;
    }
  }
  if (cpu >= 0) pin_to_cpu(cpu);
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
