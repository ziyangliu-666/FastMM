// Fixed-point decimal parse/format and 128-bit multiply (the venue-string hot path).
//
// BM_Fixed_Mul and BM_Fixed_RoundToTick are dependent latency: the next input carries a bit of the
// previous result, so the operations cannot overlap. BM_Fixed_FromDecimal and BM_Fixed_ToDecimal
// are throughput: real code parses and formats independent fields, and nothing here chains one call
// to the next, so they say how many the pipeline retires per unit time, not what one costs.
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
    const Notional n = mul(p, q);
    p.raw += 1 + (n.raw & 1);  // the next multiply waits for this one
  }
  benchmark::DoNotOptimize(p);  // outside the loop: in it, it would add a store to the chain
}
BENCHMARK(BM_Fixed_Mul);

static void BM_Fixed_RoundToTick(benchmark::State& state) {
  Price p = Price::from_decimal("50000.123").value();
  const Price tick = Price::from_decimal("0.01").value();
  for (auto _ : state) {
    const Price r = round_to_tick(p, tick, Side::Sell);
    // The next rounding waits for this one. The bit has to come from high up: the result is a
    // multiple of the tick, gcc knows it, and with (r.raw & 1) it folded the whole loop away.
    p.raw += 7 + ((r.raw >> 20) & 1);
  }
  benchmark::DoNotOptimize(p);
}
BENCHMARK(BM_Fixed_RoundToTick);
