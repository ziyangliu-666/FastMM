// fastmm-gateway's self-trade check on a shared instrument: one order against the resting orders
// of every attached strategy on the other side (RestingOrders::crosses), none of which it crosses,
// so the whole list is scanned. The argument is the number of resting orders on that side.
#include "fastmm/core/self_trade.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

using namespace fastmm;

static void BM_SelfTrade_Check(benchmark::State& state) {
  const auto n = static_cast<std::uint32_t>(state.range(0));
  RestingOrders r;
  r.reserve(64);
  for (std::uint32_t i = 0; i < n; ++i) {
    const auto epoch = static_cast<std::uint16_t>(1 + (i & 3U));
    r.set(make_cl_ord_id(epoch, i), Side::Sell, Price::from_int(101 + i), epoch);
  }
  // Bids below every ask, rotated so the compiler cannot fold the checks.
  const Price bids[4] = {
      Price::from_int(96), Price::from_int(97), Price::from_int(98), Price::from_int(99)};
  std::size_t k = 0;
  for (auto _ : state) {
    const bool x = r.crosses(9, Side::Buy, bids[k & 3U], false);
    k += 1 + static_cast<std::size_t>(x);
  }
  benchmark::DoNotOptimize(k);
}
BENCHMARK(BM_SelfTrade_Check)->Arg(0)->Arg(2)->Arg(8)->Arg(32);
