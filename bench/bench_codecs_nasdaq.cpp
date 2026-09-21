// Nasdaq codec micro-benchmarks (plan 7):
//
//   BM_Itch_DecodeAddOrder       ITCH 'A' (36 bytes) -> OrderAddL3Msg committed to an EventSink
//   BM_Itch_DecodeOrderExecuted  ITCH 'E' (31 bytes) -> OrderExecL3Msg committed to an EventSink
//   BM_ItchL2Bridge_Message      ItchL2Bridge per ITCH message (L3 update, trades, one
//                                BookDeltaMsg per 8-message datagram)
//   BM_MoldUdp64_FramePacket     parse_packet() + walk the messages of a 10-message packet
//   BM_SoupBin_FrameSequenced    SoupBinFramer over a Sequenced Data packet + on_frame()
//   BM_Ouch42_EncodeEnterOrder   OrderCommand -> OUCH 4.2 Enter Order (49 bytes)
//   BM_Ouch50_EncodeEnterOrder   OrderCommand -> OUCH 5.0 Enter Order (47 bytes, UserRefNum
//                                lookup)
//
// Each benchmark iteration runs kBatch operations between two rdtsc readings and records the
// per-operation average in picoseconds into a LogLinearHistogram; counter p50_ns is its median in
// nanoseconds (single operations are too short to time individually with rdtsc). Draining the
// sink's ring happens outside the rdtsc window.
#include "fastmm/codecs/itch/itch_decoder.hpp"
#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/codecs/itch/itch_l2_bridge.hpp"
#include "fastmm/codecs/moldudp/moldudp64.hpp"
#include "fastmm/codecs/ouch/ouch42.hpp"
#include "fastmm/codecs/ouch/ouch50.hpp"
#include "fastmm/codecs/soupbin/soupbin.hpp"
#include "fastmm/codecs/soupbin/soupbin_session.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/time.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

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

namespace {

// A replayable ITCH stream for one instrument: orders arrive within 50 cents of $100 (1 % stub
// quotes at $1 and $900), rest, get executed (E and C), cancelled (X, D) and replaced (U), and
// the stream ends by deleting everything left, so the book is empty again at the end.
struct ItchStream {
  std::vector<std::array<std::byte, 40>> msgs;
  std::vector<std::uint8_t> len;
};

ItchStream make_itch_stream(std::size_t n) {
  ItchStream s;
  itch::ItchEncoder enc;
  Xoshiro256ss rng(11);
  struct Live {
    std::uint64_t ref;
    std::int64_t qty;
    Side side;
  };
  std::vector<Live> live;
  std::uint64_t next_ref = 1;
  std::uint64_t match = 1;
  std::array<std::byte, 64> buf{};
  auto push = [&](std::size_t len) {
    std::array<std::byte, 40> m{};
    std::memcpy(m.data(), buf.data(), len);
    s.msgs.push_back(m);
    s.len.push_back(static_cast<std::uint8_t>(len));
  };
  auto price = [&](Side side) {
    const std::uint64_t r = rng.uniform(100);
    std::int64_t cents = 0;
    if (r == 0) {
      cents = side == Side::Buy ? 100 : 90'000;
    } else {
      const auto off = static_cast<std::int64_t>(rng.uniform(50));
      cents = side == Side::Buy ? 10'000 - off : 10'001 + off;
    }
    return Price::from_raw(cents * 1'000'000);
  };
  while (s.msgs.size() < n) {
    const std::uint64_t op = live.size() < 2'000 ? 0 : live.size() > 6'000 ? 4 : rng.uniform(6);
    if (op <= 1) {
      const Side side = rng.uniform(2) == 0 ? Side::Buy : Side::Sell;
      const auto q = static_cast<std::int64_t>(1 + rng.uniform(20)) * 100;
      push(enc.add_order(buf, 7, 1, next_ref, side, Qty::from_int(q), "BENCH", price(side)));
      live.push_back({next_ref++, q, side});
      continue;
    }
    const std::size_t k = rng.uniform(live.size());
    Live& o = live[k];
    if (op == 2 || op == 3) {  // execute 100 shares (E or C)
      const std::int64_t q = std::min<std::int64_t>(100, o.qty);
      if (op == 2) {
        push(enc.order_executed(buf, 7, 1, o.ref, Qty::from_int(q), match++));
      } else {
        push(enc.order_executed_with_price(
            buf, 7, 1, o.ref, Qty::from_int(q), match++, kPrice, rng.uniform(2) == 0));
      }
      o.qty -= q;
    } else if (op == 4) {
      push(enc.order_delete(buf, 7, 1, o.ref));
      o.qty = 0;
    } else {
      push(enc.order_replace(buf, 7, 1, o.ref, next_ref, Qty::from_int(o.qty), price(o.side)));
      o.ref = next_ref++;
    }
    if (o.qty == 0) {
      live[k] = live.back();
      live.pop_back();
    }
  }
  for (const Live& o : live) push(enc.order_delete(buf, 7, 1, o.ref));
  return s;
}

}  // namespace

static void BM_ItchL2Bridge_Message(benchmark::State& state) {
  static const ItchStream stream = make_itch_stream(1U << 18);
  auto ring = std::make_unique<MsgRing>(1U << 22);
  venues::EventSink sink(ring.get(), venues::SinkPolicy::Drop);
  itch::ItchL2BridgeConfig cfg;
  cfg.book = L3BookConfig{.price_window_ticks = 1U << 16, .max_orders = 1U << 16};
  auto bridge = std::make_unique<itch::ItchL2Bridge>(sink, cfg);
  bridge->add_instrument("BENCH", InstrumentId{0});
  bridge->map_locate(7, InstrumentId{0});
  bridge->mark_complete(InstrumentId{0});
  drain(*ring);
  constexpr int kPerDatagram = 8;
  const std::size_t n = stream.msgs.size();
  std::size_t i = 0;
  std::uint64_t seq = 0;
  BatchTimer timer;
  for (auto _ : state) {
    timer.start();
    const itch::DatagramStamp stamp{rdtsc(), Timestamp{1}};
    for (int j = 0; j < kBatch; ++j) {
      bridge->on_itch_message(++seq, {stream.msgs[i].data(), stream.len[i]}, stamp);
      if (++i == n) i = 0;
      if (j % kPerDatagram == kPerDatagram - 1) bridge->end_datagram();
    }
    timer.stop();
    drain(*ring);
  }
  timer.report(state);
  state.counters["deltas_per_msg"] =
      static_cast<double>(bridge->stats().deltas) / static_cast<double>(bridge->stats().messages);
  if (bridge->stats().book_errors != 0) state.SkipWithError("book errors");
}
BENCHMARK(BM_ItchL2Bridge_Message);

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
