// Strategy quoting functions in isolation (no engine, no book).
//
//   BM_BasicMM_ComputeQuotes   BasicMM::compute_quotes for two levels per side, inventory skew on,
//                              cycling through a small set of mids and positions (long, flat,
//                              short, at the cap) so both the skew and the inventory cap branches
//                              run.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>

using namespace fastmm;

namespace {
struct NoCtx {
  TimerId every(Duration, std::uint64_t) { return TimerId{1}; }
};
}  // namespace

static void BM_BasicMM_ComputeQuotes(benchmark::State& state) {
  BasicMM s;
  const auto err = s.configure({{"half_spread_bps", "1.25"},
                                {"skew_bps_per_unit", "0.5"},
                                {"quote_qty", "0.002"},
                                {"max_inventory", "0.01"},
                                {"levels", "2"},
                                {"level_step_ticks", "2"}});
  if (err) {
    state.SkipWithError(err->c_str());
    return;
  }
  NoCtx ctx;
  s.on_start(ctx);
  Instrument inst{};
  inst.tick = Price::from_decimal("0.01").value();
  inst.lot = Qty::from_decimal("0.00001").value();
  const std::array<Price, 4> mids = {Price::from_decimal("60000.005").value(),
                                     Price::from_decimal("60000.125").value(),
                                     Price::from_decimal("59999.995").value(),
                                     Price::from_decimal("60001.5").value()};
  const std::array<Qty, 4> positions = {Qty{},
                                        Qty::from_decimal("0.004").value(),
                                        Qty::from_decimal("-0.006").value(),
                                        Qty::from_decimal("0.01").value()};
  std::size_t k = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(s.compute_quotes(mids[k & 3], positions[(k >> 2) & 3], inst));
    ++k;
  }
}
BENCHMARK(BM_BasicMM_ComputeQuotes);
