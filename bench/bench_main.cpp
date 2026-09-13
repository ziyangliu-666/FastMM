// Shared benchmark main: pins the process to a core (--cpu=N or FASTMM_BENCH_CPU) before
// running so results are stable, then delegates to Google Benchmark.
#include <benchmark/benchmark.h>

#include <sched.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0)
    std::fprintf(stderr, "warning: cannot pin to cpu %d\n", cpu);
}
}  // namespace

int main(int argc, char** argv) {
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
