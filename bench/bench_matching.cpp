// MatchingEngine micro-benchmarks: submit/cancel mix (resting book maintenance), taker
// sweeps against a deep book, and a generator-driven mixed workload.
#include "fastmm/core/rng.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;

namespace {
const Price kTick = Price::from_decimal("0.01").value();
Price px(std::int64_t ticks) {
  return Price::from_raw(ticks * kTick.raw);
}
NewOrder limit(std::uint64_t id, Side s, std::int64_t ticks, std::int64_t units) {
  NewOrder o;
  o.account = 0;
  o.cl_ord_id = ClientOrderId{id};
  o.instrument = InstrumentId{0};
  o.side = s;
  o.type = OrderType::Limit;
  o.tif = TimeInForce::Gtc;
  o.price = px(ticks);
  o.qty = Qty::from_int(units);
  return o;
}
}  // namespace

// Submit a passive limit order and cancel it again: two operations per iteration.
static void BM_Matching_SubmitCancel(benchmark::State& state) {
  auto me = std::make_unique<MatchingEngine>(1);
  for (std::uint64_t i = 1; i <= 200; ++i) {  // 100 levels per side of resting depth
    me->submit(limit(i, Side::Buy, 10000 - static_cast<std::int64_t>(i), 1), Timestamp{1});
    me->submit(limit(1000 + i, Side::Sell, 10001 + static_cast<std::int64_t>(i), 1), Timestamp{1});
  }
  Xoshiro256ss rng(1);
  std::uint64_t id = 1'000'000;
  Timestamp now{2};
  for (auto _ : state) {
    const Side s = rng.uniform(2) == 0 ? Side::Buy : Side::Sell;
    const std::int64_t off = rng.between(0, 30);
    const std::int64_t ticks = s == Side::Buy ? 10000 - off : 10001 + off;
    const NewOrder o = limit(++id, s, ticks, 1);
    now.ns += 1;
    benchmark::DoNotOptimize(me->submit(o, now));
    benchmark::DoNotOptimize(me->cancel(0, o.cl_ord_id, now));
  }
  state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Matching_SubmitCancel);

// Aggressive limit sweeping `levels` resting levels of 1 unit each; the book is rebuilt
// outside the timed region.
static void BM_Matching_Sweep(benchmark::State& state) {
  const std::int64_t levels = state.range(0);
  auto me = std::make_unique<MatchingEngine>(1);
  std::uint64_t id = 1;
  Timestamp now{1};
  for (auto _ : state) {
    state.PauseTiming();
    for (std::int64_t k = 0; k < levels; ++k) {
      me->submit(limit(id++, Side::Sell, 10001 + k, 1), now);
    }
    state.ResumeTiming();
    now.ns += 1;
    NewOrder taker = limit(id++, Side::Buy, 10001 + levels, levels);
    benchmark::DoNotOptimize(me->submit(taker, now));
  }
  state.SetItemsProcessed(state.iterations() * levels);
  state.counters["fills_per_op"] = static_cast<double>(levels);
}
BENCHMARK(BM_Matching_Sweep)->Arg(1)->Arg(10)->Arg(100);

// Market-order sweep through one deep level (FIFO of `orders` makers).
static void BM_Matching_MarketSweepFifo(benchmark::State& state) {
  const std::int64_t orders = state.range(0);
  auto me = std::make_unique<MatchingEngine>(1);
  std::uint64_t id = 1;
  Timestamp now{1};
  for (auto _ : state) {
    state.PauseTiming();
    for (std::int64_t k = 0; k < orders; ++k) me->submit(limit(id++, Side::Sell, 10001, 1), now);
    state.ResumeTiming();
    NewOrder taker = limit(id++, Side::Buy, 0, orders);
    taker.type = OrderType::Market;
    benchmark::DoNotOptimize(me->submit(taker, now));
  }
  state.SetItemsProcessed(state.iterations() * orders);
}
BENCHMARK(BM_Matching_MarketSweepFifo)->Arg(10)->Arg(100);

// Generator-driven mixed workload: one generator action per iteration (limit / cancel /
// market / mid step) against a live book.
static void BM_Matching_GeneratorMix(benchmark::State& state) {
  auto me = std::make_unique<MatchingEngine>(1);
  MarketGeneratorParams p;
  p.limit_rate_per_s = 500;
  p.market_rate_per_s = 30;
  const Timestamp start{seconds(1'700'000'000).ns};
  MarketGenerator gen(p, 42, InstrumentId{0}, start);
  gen.seed_book(*me, 20, start);
  for (auto _ : state) gen.step(*me);
  state.SetItemsProcessed(state.iterations());
  state.counters["resting"] = static_cast<double>(me->open_orders());
}
BENCHMARK(BM_Matching_GeneratorMix);
