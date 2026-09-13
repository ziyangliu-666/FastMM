#include "fastmm/core/latency.hpp"
#include "fastmm/core/time.hpp"

#include <benchmark/benchmark.h>

#include <memory>

using namespace fastmm;

static void BM_Histogram_Record(benchmark::State& state) {
  // Heap-allocated so the object escapes and ClobberMemory() forces the stores.
  auto h = std::make_unique<LogLinearHistogram>();
  benchmark::DoNotOptimize(h.get());
  std::uint64_t v = 1;
  for (auto _ : state) {
    v = v * 6364136223846793005ULL + 1;
    h->record(v >> 44);
    benchmark::ClobberMemory();
  }
  benchmark::DoNotOptimize(h->count());
}
BENCHMARK(BM_Histogram_Record);

static void BM_Histogram_Percentile(benchmark::State& state) {
  LogLinearHistogram h;
  for (std::uint64_t i = 0; i < 100'000; ++i) h.record((i * 7919) % 50'000);
  for (auto _ : state) {
    benchmark::DoNotOptimize(h.percentile(0.99));
  }
}
BENCHMARK(BM_Histogram_Percentile);

static void BM_TscClock_Now(benchmark::State& state) {
  TscClock clk;
  clk.calibrate(milliseconds(10));
  for (auto _ : state) {
    benchmark::DoNotOptimize(clk.now());
  }
}
BENCHMARK(BM_TscClock_Now);

static void BM_WallClock_Now(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(wall_now());
  }
}
BENCHMARK(BM_WallClock_Now);
