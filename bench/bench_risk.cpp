// Pre-trade risk checks.
//
//   BM_Risk_CheckNew_Pass    dependent latency: the next intent index is derived from the previous
//                            answer, so the checks cannot overlap in the pipeline the way an
//                            independent loop lets them.
//   BM_Risk_CheckNew_Killed  throughput of the kill-switch early out: the intent never changes and
//                            nothing carries a dependency, so the figure is one check per
//                            <time>, not the latency of a check.
#include "fastmm/core/risk.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>

using namespace fastmm;

static void BM_Risk_CheckNew_Pass(benchmark::State& state) {
  Instrument inst{};
  inst.id = InstrumentId{0};
  inst.flags = Instrument::kEnabled;
  inst.tick = Price::from_decimal("0.01").value();
  inst.lot = Qty::from_decimal("0.001").value();
  inst.min_qty = inst.lot;
  inst.min_notional = Notional::from_int(5);
  RiskLimits l;
  l.max_order_qty = Qty::from_int(10);
  l.max_order_notional = Notional::from_int(100000);
  l.max_position = Qty::from_int(20);
  l.max_open_orders = 8;
  l.price_collar_bps = 100;
  l.fat_finger_bps = 500;
  l.stale_md = seconds(1);
  l.orders_per_sec = 1'000'000'000;
  const Timestamp now{seconds(1).ns};
  RiskEngine risk(l, now);
  risk.on_book(inst.id, Price::from_int(100), now);
  risk.on_trade(inst.id, Price::from_int(100));
  Position pos{};
  RiskInputs in{};
  in.now = now;
  in.position = &pos;
  in.open_same_side = Qty::from_int(1);
  in.open_orders = 2;
  in.best_own_opposite = Price::from_decimal("100.5").value();
  // Rotate through a few intents so the compiler cannot fold the checks.
  OrderIntent intents[4] = {};
  for (int i = 0; i < 4; ++i) {
    intents[i].instrument = InstrumentId{0};
    intents[i].side = (i & 1) ? Side::Sell : Side::Buy;
    intents[i].price =
        Price::from_raw(Price::from_int(100).raw + ((i & 1) ? 1 : -1) * (50 + i) * inst.tick.raw);
    intents[i].qty = Qty::from_raw(Qty::from_int(1).raw + i * inst.lot.raw);
  }
  std::size_t k = 0;
  for (auto _ : state) {
    const RejectReason r = risk.check_new(intents[k & 3], inst, in);
    // The next intent depends on this answer, so one check must finish before the next starts.
    // r is None (0) on this path, so the intents rotate exactly as they did without the chain.
    k += 1 + static_cast<std::size_t>(r);
  }
  benchmark::DoNotOptimize(k);  // outside the loop: in it, it would add a store to the chain
}
BENCHMARK(BM_Risk_CheckNew_Pass);

static void BM_Risk_CheckNew_Killed(benchmark::State& state) {
  Instrument inst{};
  inst.id = InstrumentId{0};
  RiskEngine risk;
  risk.trip();
  RiskInputs in{};
  OrderIntent o{};
  o.instrument = InstrumentId{0};
  for (auto _ : state) {
    benchmark::DoNotOptimize(risk.check_new(o, inst, in));
  }
}
BENCHMARK(BM_Risk_CheckNew_Killed);
