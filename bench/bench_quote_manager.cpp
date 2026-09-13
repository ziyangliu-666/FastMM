// QuoteManager reconcile: 3 levels per side resting, one level moves (6 orders, 1 change).
#include "fastmm/core/quote_manager.hpp"

#include <benchmark/benchmark.h>

using namespace fastmm;

namespace {
struct Placer {
  Oms& oms;
  bool operator()(QuoteAction& a) {
    if (a.kind == QuoteActionKind::New) {
      NewOrderRequest r{};
      r.instrument = a.instrument;
      r.side = a.side;
      r.price = a.price;
      r.qty = a.qty;
      r.user_tag = QuoteManager::make_tag(a.side, a.level);
      const ClientOrderId id = oms.next_cl_ord_id();
      auto h = oms.submit(r, id, {});
      if (!h) return false;
      a.handle = *h;
      a.cl_ord_id = id;
      OrderAckMsg m{};
      init_header(m, EventType::OrderAck);
      m.cl_ord_id = id;
      oms.on_ack(m);  // ack immediately so the order is working
      return true;
    }
    if (a.kind == QuoteActionKind::Replace) {
      const ClientOrderId nid = oms.next_cl_ord_id();
      if (!oms.request_replace(a.handle, nid, a.price, a.qty)) return false;
      OrderAckMsg m{};
      init_header(m, EventType::OrderAck);
      m.cl_ord_id = nid;
      oms.on_ack(m);
      return true;
    }
    return oms.request_cancel(a.handle).has_value();
  }
};
}  // namespace

static void BM_QuoteManager_Reconcile_6Orders1Change(benchmark::State& state) {
  Instrument inst{};
  inst.id = InstrumentId{0};
  inst.tick = Price::from_decimal("0.01").value();
  inst.lot = Qty::from_decimal("0.001").value();
  QuoteParams p;
  p.supports_replace = true;
  p.min_requote_interval = Duration{};
  QuoteManager qm(p);
  Oms oms;
  Placer place{oms};
  DesiredQuotes d;
  for (int i = 1; i <= 3; ++i) {
    d.bids.push_back(
        {Price::from_raw(Price::from_int(100).raw - i * inst.tick.raw), Qty::from_int(1)});
    d.asks.push_back(
        {Price::from_raw(Price::from_int(100).raw + i * inst.tick.raw), Qty::from_int(1)});
  }
  qm.reconcile(inst, d, oms, {}, place);
  bool flip = false;
  for (auto _ : state) {
    d.bids[2].price = Price::from_raw(Price::from_int(100).raw - (flip ? 5 : 3) * inst.tick.raw);
    flip = !flip;
    benchmark::DoNotOptimize(qm.reconcile(inst, d, oms, {}, place));
  }
}
BENCHMARK(BM_QuoteManager_Reconcile_6Orders1Change);

static void BM_QuoteManager_Reconcile_NoChange(benchmark::State& state) {
  Instrument inst{};
  inst.id = InstrumentId{0};
  inst.tick = Price::from_decimal("0.01").value();
  inst.lot = Qty::from_decimal("0.001").value();
  QuoteManager qm;
  Oms oms;
  Placer place{oms};
  DesiredQuotes d;
  for (int i = 1; i <= 3; ++i) {
    d.bids.push_back(
        {Price::from_raw(Price::from_int(100).raw - i * inst.tick.raw), Qty::from_int(1)});
    d.asks.push_back(
        {Price::from_raw(Price::from_int(100).raw + i * inst.tick.raw), Qty::from_int(1)});
  }
  qm.reconcile(inst, d, oms, {}, place);
  for (auto _ : state) {
    benchmark::DoNotOptimize(qm.reconcile(inst, d, oms, {}, place));
  }
}
BENCHMARK(BM_QuoteManager_Reconcile_NoChange);
