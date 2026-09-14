// FIX 4.4 codec: parse and decode an ExecutionReport fill, build a NewOrderSingle, frame a stream.
// Run with --benchmark_repetitions=9 --benchmark_report_aggregates_only=true for medians (p50).
#include "fastmm/codecs/fix/fix.hpp"
#include "fastmm/core/msg_ring.hpp"

#include <benchmark/benchmark.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::fix;

namespace {

constexpr std::int64_t kNow = 1'789'374'615'250'000'000;
std::int64_t fixed_clock(void*) noexcept {
  return kNow;
}
void discard(void*, std::span<const char>) noexcept {}

FixSessionConfig bench_config() {
  FixSessionConfig c;
  c.sender_comp_id = "CLIENT";
  c.target_comp_id = "VENUE";
  c.store_max_messages = 16;
  c.store_max_bytes = 1U << 12;
  return c;
}

// A realistic partial-fill ExecutionReport (24 fields); ExecID varies so dedup never triggers.
std::vector<std::string> fill_reports(std::size_t count) {
  std::vector<std::string> out;
  char buf[512];
  for (std::size_t i = 0; i < count; ++i) {
    FixBuilder b{std::span<char>(buf)};
    const std::string exec_id = "EX" + std::to_string(100000 + i);
    b.begin_header(msg::kExecutionReport, "VENUE", "CLIENT", 1000 + i, kNow)
        .field(tag::kOrderID, "8123456789")
        .field(tag::kClOrdID, "fm0001000000c8")
        .field(tag::kExecID, exec_id)
        .field_char(tag::kExecType, exec_type::kTrade)
        .field_char(tag::kOrdStatus, ord_status::kPartiallyFilled)
        .field(tag::kSymbol, "BTCUSDT")
        .field_char(tag::kSide, kSideBuy)
        .field_decimal(tag::kOrderQty, Qty::from_decimal("0.5").value())
        .field_char(tag::kOrdType, kOrdTypeLimit)
        .field_decimal(tag::kPrice, Price::from_decimal("70000.5").value())
        .field_decimal(tag::kLastQty, Qty::from_decimal("0.125").value())
        .field_decimal(tag::kLastPx, Price::from_decimal("70000.5").value())
        .field_decimal(tag::kLeavesQty, Qty::from_decimal("0.375").value())
        .field_decimal(tag::kCumQty, Qty::from_decimal("0.125").value())
        .field_decimal(tag::kAvgPx, Price::from_decimal("70000.5").value())
        .field_timestamp(tag::kTransactTime, kNow)
        .field_int(tag::kLastLiquidityInd, kLiquidityAdded);
    const std::size_t n = b.finish();
    out.emplace_back(buf, n);
  }
  return out;
}

}  // namespace

static void BM_Fix_ParseExecReportFill(benchmark::State& state) {
  const std::vector<std::string> msgs = fill_reports(1);
  auto view = std::make_unique<FixView>();
  for (auto _ : state) {
    benchmark::DoNotOptimize(view->parse(msgs[0]));
    benchmark::DoNotOptimize(view->get_qty(tag::kLastQty));
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(msgs[0].size()));
}
BENCHMARK(BM_Fix_ParseExecReportFill);

static void BM_Fix_DecodeExecReportFill(benchmark::State& state) {
  const std::vector<std::string> msgs = fill_reports(1024);
  FixSymbolTable symbols;
  symbols.add("BTCUSDT", InstrumentId{0});
  auto decoder = std::make_unique<FixDecoder>(symbols, VenueId{0});
  MsgRing ring(1U << 16);
  venues::EventSink sink(&ring, venues::SinkPolicy::Drop);
  std::size_t k = 0;
  for (auto _ : state) {
    const std::string& m = msgs[k++ & 1023U];
    const FrameView frame{std::as_bytes(std::span<const char>(m.data(), m.size())), m.size(), 0};
    benchmark::DoNotOptimize(decoder->decode(frame, kNow, sink));
    if (ring.try_peek() != nullptr) ring.release();
  }
}
BENCHMARK(BM_Fix_DecodeExecReportFill);

static void BM_Fix_BuildNewOrderSingle(benchmark::State& state) {
  FixSymbolTable symbols;
  symbols.add("BTCUSDT", InstrumentId{0});
  auto session = std::make_unique<FixSession>(bench_config(), &discard, nullptr);
  session->set_clock(&fixed_clock, nullptr);
  auto encoder = std::make_unique<FixEncoder>(*session, symbols);
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.tif = TimeInForce::Gtc;
  n.price = Price::from_decimal("70000.5").value();
  n.qty = Qty::from_decimal("0.001").value();
  std::byte out[512];
  std::uint32_t seq = 0;
  venues::OrderCommand cmd = *venues::OrderCommand::from(n.hdr);
  for (auto _ : state) {
    cmd.cl_ord_id = make_cl_ord_id(1, ++seq);
    benchmark::DoNotOptimize(encoder->encode(cmd, out));
  }
}
BENCHMARK(BM_Fix_BuildNewOrderSingle);

static void BM_Fix_FramerNext(benchmark::State& state) {
  const std::vector<std::string> msgs = fill_reports(1);
  FixFramer framer;
  const auto bytes = std::as_bytes(std::span<const char>(msgs[0].data(), msgs[0].size()));
  for (auto _ : state) benchmark::DoNotOptimize(framer.next(bytes));
}
BENCHMARK(BM_Fix_FramerNext);
