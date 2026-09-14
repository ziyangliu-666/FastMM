// Nasdaq codec micro-benchmarks (plan 7):
//
//   BM_Itch_DecodeAddOrder        ITCH 5.0 'A' (36 bytes) -> OrderAddL3Msg committed to an
//   EventSink BM_Itch_DecodeOrderExecuted   ITCH 5.0 'E' (31 bytes) -> OrderExecL3Msg committed to
//   an EventSink BM_MoldUdp64_FramePacket      parse_packet() + walk the messages of a 10-message
//   packet BM_SoupBin_FrameSequenced     SoupBinFramer over a Sequenced Data packet +
//   ClientSession::on_frame BM_Ouch42_EncodeEnterOrder    OrderCommand -> OUCH 4.2 Enter Order (49
//   bytes) BM_Ouch50_EncodeEnterOrder    OrderCommand -> OUCH 5.0 Enter Order (47 bytes, UserRefNum
//   lookup)
//
// Each benchmark iteration runs kBatch operations between two rdtsc readings and records the
// per-operation average in picoseconds into a LogLinearHistogram; counter p50_ns is its median in
// nanoseconds (single operations are too short to time individually with rdtsc). Draining the
// sink's ring happens outside the rdtsc window.
#include "fastmm/codecs/itch/itch_decoder.hpp"
#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/codecs/moldudp/moldudp64.hpp"
#include "fastmm/codecs/ouch/ouch42.hpp"
#include "fastmm/codecs/ouch/ouch50.hpp"
#include "fastmm/codecs/soupbin/soupbin.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/time.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <memory>

using namespace fastmm;
using namespace fastmm::codecs;

namespace {

constexpr int kBatch = 64;

const TscClock& tsc() {
  static const TscClock c = [] {
    TscClock t;
    t.calibrate(milliseconds(20));
    return t;
  }();
  return c;
}

class BatchTimer {
 public:
  void start() noexcept { t0_ = rdtsc(); }
  void stop() noexcept {
    const Cycles t1 = rdtsc();
    const auto ns = static_cast<std::uint64_t>(tsc().cycles_to_ns(t1 - t0_));
    h_.record(ns * 1000U / static_cast<std::uint64_t>(kBatch));
  }
  void report(benchmark::State& state) const {
    state.counters["p50_ns"] = static_cast<double>(h_.percentile(0.50)) / 1000.0;
    state.counters["p99_ns"] = static_cast<double>(h_.percentile(0.99)) / 1000.0;
    state.SetItemsProcessed(state.iterations() * kBatch);
  }

 private:
  Cycles t0_{};
  LogLinearHistogram h_;
};

struct NullWriter {
  bool send(std::span<const std::byte>) noexcept { return true; }
};

void drain(MsgRing& ring) noexcept {
  while (ring.try_peek() != nullptr) ring.release();
}

const Price kPrice = Price::from_decimal("189.1234").value();
const Qty kQty = Qty::from_int(300);

}  // namespace

static void itch_decode_bench(benchmark::State& state, bool executed) {
  auto dec = std::make_unique<itch::ItchDecoder>();
  dec->map_locate(7, InstrumentId{0});
  auto ring = std::make_unique<MsgRing>(1U << 16);
  venues::EventSink sink(ring.get(), venues::SinkPolicy::Drop);
  itch::ItchEncoder enc;
  std::array<std::byte, 64> msg{};
  const std::size_t n =
      executed
          ? enc.order_executed(msg, 7, 34'200'000'000'000ULL, 1001, Qty::from_int(100), 5001)
          : enc.add_order(msg, 7, 34'200'000'000'000ULL, 1001, Side::Buy, kQty, "AAPL", kPrice);
  const FrameView f{std::span<const std::byte>(msg.data(), n), n, 0};
  BatchTimer timer;
  for (auto _ : state) {
    timer.start();
    for (int i = 0; i < kBatch; ++i) benchmark::DoNotOptimize(dec->decode(f, 0, sink));
    timer.stop();
    drain(*ring);
  }
  timer.report(state);
}

static void BM_Itch_DecodeAddOrder(benchmark::State& state) {
  itch_decode_bench(state, false);
}
BENCHMARK(BM_Itch_DecodeAddOrder);

static void BM_Itch_DecodeOrderExecuted(benchmark::State& state) {
  itch_decode_bench(state, true);
}
BENCHMARK(BM_Itch_DecodeOrderExecuted);

static void BM_MoldUdp64_FramePacket(benchmark::State& state) {
  itch::ItchEncoder enc;
  std::array<std::byte, 64> msg{};
  const std::size_t n = enc.add_order(msg, 7, 1, 1001, Side::Buy, kQty, "AAPL", kPrice);
  std::array<std::byte, 1500> dgram{};
  moldudp::PacketBuilder b(dgram, moldudp::make_session("BENCH"), 1);
  for (int i = 0; i < 10; ++i) b.add(std::span<const std::byte>(msg.data(), n));
  const std::size_t len = b.finish();
  const std::span<const std::byte> packet(dgram.data(), len);
  BatchTimer timer;
  for (auto _ : state) {
    timer.start();
    std::size_t bytes = 0;
    for (int i = 0; i < kBatch; ++i) {
      moldudp::PacketView p;
      if (!moldudp::parse_packet(packet, p)) continue;
      moldudp::MessageIterator it(p);
      std::uint64_t seq = 0;
      std::span<const std::byte> m;
      while (it.next(seq, m)) bytes += m.size();
    }
    timer.stop();
    benchmark::DoNotOptimize(bytes);
  }
  timer.report(state);
  state.counters["msgs_per_packet"] = 10;
}
BENCHMARK(BM_MoldUdp64_FramePacket);

static void BM_SoupBin_FrameSequenced(benchmark::State& state) {
  NullWriter w;
  soupbin::ClientConfig cfg;
  cfg.username = "u";
  cfg.password = "p";
  soupbin::ClientSession<NullWriter> client(w, cfg);
  std::array<std::byte, 64> login{};
  const std::size_t ln = soupbin::write_login_accepted(login, "S1", 1);
  soupbin::SoupBinFramer f;
  client.login(0);
  client.on_frame(f.next(std::span<const std::byte>(login.data(), ln)));
  std::array<std::byte, 64> msg{};
  itch::ItchEncoder enc;
  const std::size_t mn = enc.order_executed(msg, 7, 1, 1001, Qty::from_int(100), 5001);
  std::array<std::byte, 128> pkt{};
  const std::size_t pn =
      soupbin::write_packet(pkt, soupbin::PacketType::SequencedData, {msg.data(), mn});
  const std::span<const std::byte> stream(pkt.data(), pn);
  BatchTimer timer;
  for (auto _ : state) {
    timer.start();
    for (int i = 0; i < kBatch; ++i) {
      const FrameView v = f.next(stream);
      benchmark::DoNotOptimize(client.on_frame(v));
    }
    timer.stop();
  }
  timer.report(state);
}
BENCHMARK(BM_SoupBin_FrameSequenced);

static venues::OrderCommand bench_order() {
  venues::OrderCommand c;
  c.kind = venues::OrderCommandKind::New;
  c.instrument = InstrumentId{0};
  c.cl_ord_id = make_cl_ord_id(1, 42);
  c.side = Side::Buy;
  c.price = kPrice;
  c.qty = kQty;
  return c;
}

static void BM_Ouch42_EncodeEnterOrder(benchmark::State& state) {
  auto enc = std::make_unique<ouch42::OuchEncoder>();
  enc->add_symbol("AAPL", InstrumentId{0});
  const venues::OrderCommand cmd = bench_order();
  std::array<std::byte, 128> out{};
  BatchTimer timer;
  for (auto _ : state) {
    timer.start();
    for (int i = 0; i < kBatch; ++i) benchmark::DoNotOptimize(enc->encode(cmd, out));
    timer.stop();
  }
  timer.report(state);
}
BENCHMARK(BM_Ouch42_EncodeEnterOrder);

static void BM_Ouch50_EncodeEnterOrder(benchmark::State& state) {
  auto ids = std::make_unique<ouch50::UserRefMap>();
  auto enc = std::make_unique<ouch50::OuchEncoder>(*ids);
  enc->add_symbol("AAPL", InstrumentId{0});
  const venues::OrderCommand cmd = bench_order();  // same id: UserRefNum found, not re-assigned
  std::array<std::byte, 128> out{};
  BatchTimer timer;
  for (auto _ : state) {
    timer.start();
    for (int i = 0; i < kBatch; ++i) benchmark::DoNotOptimize(enc->encode(cmd, out));
    timer.stop();
  }
  timer.report(state);
}
BENCHMARK(BM_Ouch50_EncodeEnterOrder);
