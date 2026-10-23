#include "fastmm/core/oms.hpp"

#include <benchmark/benchmark.h>

#include <cstdio>

using namespace fastmm;

// submit -> ack -> fill (full lifecycle, three messages).
static void BM_Oms_Lifecycle(benchmark::State& state) {
  Oms oms;
  NewOrderRequest r{};
  r.instrument = InstrumentId{0};
  r.side = Side::Buy;
  r.price = Price::from_int(100);
  r.qty = Qty::from_int(1);
  OrderAckMsg a{};
  init_header(a, EventType::OrderAck);
  a.venue_order_id = "123456789";
  OrderFillMsg f{};
  init_header(f, EventType::OrderFill);
  f.qty = Qty::from_int(1);
  f.cum_qty = Qty::from_int(1);
  std::uint64_t n = 0;
  for (auto _ : state) {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = oms.submit(r, id, Timestamp{});
    benchmark::DoNotOptimize(h);
    a.cl_ord_id = id;
    benchmark::DoNotOptimize(oms.on_ack(a));
    f.cl_ord_id = id;
    f.exec_id.clear();
    char buf[16];
    const int len = std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(n++));
    f.exec_id.assign(std::string_view(buf, static_cast<std::size_t>(len)));
    benchmark::DoNotOptimize(oms.on_fill(f));
  }
}
BENCHMARK(BM_Oms_Lifecycle);

static void BM_Oms_Submit(benchmark::State& state) {
  Oms oms;
  NewOrderRequest r{};
  r.instrument = InstrumentId{0};
  r.side = Side::Buy;
  r.price = Price::from_int(100);
  r.qty = Qty::from_int(1);
  OrderRejectMsg rej{};
  init_header(rej, EventType::OrderReject);
  for (auto _ : state) {
    const ClientOrderId id = oms.next_cl_ord_id();
    auto h = oms.submit(r, id, Timestamp{});
    benchmark::DoNotOptimize(h);
    rej.cl_ord_id = id;
    oms.on_reject(rej);
  }
}
BENCHMARK(BM_Oms_Submit);

// A quoting book: 100 instruments with four levels a side working (800 live orders), and the best
// bid of one instrument amended away from the touch and back, each replace acknowledged. Moving the
// best order away makes the OMS find the instrument's new best own price.
static void BM_Oms_ReplaceBest_800Live(benchmark::State& state) {
  Oms oms;
  constexpr std::uint32_t kInstruments = 100;
  constexpr int kLevels = 4;
  OrderAckMsg a{};
  init_header(a, EventType::OrderAck);
  a.venue_order_id = "1";
  Handle<Order> best{};
  for (std::uint32_t i = 0; i < kInstruments; ++i) {
    for (const Side side : {Side::Buy, Side::Sell}) {
      for (int l = 0; l < kLevels; ++l) {
        NewOrderRequest r{};
        r.instrument = InstrumentId{i};
        r.side = side;
        r.price = Price::from_int(side == Side::Buy ? 100 - l : 101 + l);
        r.qty = Qty::from_int(1);
        const ClientOrderId id = oms.next_cl_ord_id();
        auto h = oms.submit(r, id, Timestamp{});
        a.cl_ord_id = id;
        static_cast<void>(oms.on_ack(a));
        if (i == 0 && side == Side::Buy && l == 0) best = *h;
      }
    }
  }
  const ClientOrderId id = oms.get(best).cl_ord_id;
  a.cl_ord_id = id;
  bool away = false;
  for (auto _ : state) {
    away = !away;
    static_cast<void>(
        oms.request_replace(best, id, Price::from_int(away ? 90 : 100), Qty::from_int(1)));
    benchmark::DoNotOptimize(oms.on_ack(a));
    benchmark::DoNotOptimize(oms.best_own_px(InstrumentId{0}, Side::Buy));
  }
}
BENCHMARK(BM_Oms_ReplaceBest_800Live);
