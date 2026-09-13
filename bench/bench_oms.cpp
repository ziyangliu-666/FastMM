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
