// Fixed-point decimal parse/format and 128-bit multiply (the venue-string hot path).
#include "fastmm/core/fixed_point.hpp"

#include <benchmark/benchmark.h>

#include <string_view>

using namespace fastmm;

static void BM_Fixed_FromDecimal(benchmark::State& state) {
  static constexpr std::string_view kInputs[4] = {
      "50000.12345678", "0.00012345", "12345678.9", "1"};
  std::size_t k = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(Price::from_decimal(kInputs[k & 3]));
    ++k;
  }
}
BENCHMARK(BM_Fixed_FromDecimal);

static void BM_Fixed_ToDecimal(benchmark::State& state) {
  Price p = Price::from_decimal("50000.12345678").value();
  char buf[kMaxDecimalChars];
  benchmark::DoNotOptimize(&buf);
  for (auto _ : state) {
    benchmark::DoNotOptimize(p.to_decimal(buf));
    benchmark::ClobberMemory();
    p.raw += 1;
  }
}
BENCHMARK(BM_Fixed_ToDecimal);

static void BM_Fixed_Mul(benchmark::State& state) {
  Price p = Price::from_decimal("50000.5").value();
  const Qty q = Qty::from_decimal("0.00123").value();
  for (auto _ : state) {
    benchmark::DoNotOptimize(mul(p, q));
    p.raw += 1;
  }
}
BENCHMARK(BM_Fixed_Mul);

static void BM_Fixed_RoundToTick(benchmark::State& state) {
  Price p = Price::from_decimal("50000.123").value();
  const Price tick = Price::from_decimal("0.01").value();
  for (auto _ : state) {
    benchmark::DoNotOptimize(round_to_tick(p, tick, Side::Sell));
    p.raw += 7;
  }
}
BENCHMARK(BM_Fixed_RoundToTick);
