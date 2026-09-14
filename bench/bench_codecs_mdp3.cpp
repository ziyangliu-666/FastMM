// CME MDP 3.0 codec micro-benchmarks: decoding an MDIncrementalRefreshBook46 packet with four
// entries into the market-data ring (one BookDeltaMsg), and the per-packet cost of Mdp3Feed's
// A/B arbitration (first copy processed, second copy dropped).
#include "fastmm/codecs/mdp3/mdp3_encoder.hpp"
#include "fastmm/codecs/mdp3/mdp3_feed.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::mdp3;

namespace {

constexpr std::int32_t kSecurityId = 1234;
// First NoMDEntries entry of a Book46 packet, and RptSeq inside an entry.
constexpr std::size_t kEntryOffset =
    kPacketHeaderSize + kMessagePrefixSize + schema::MDIncrementalRefreshBook46::kBlockLength + 3;
constexpr std::size_t kRptOffset = 16;

Price price_ticks(std::int64_t ticks) {
  return Price::from_raw(ticks * 25'000'000);  // 0.25 tick
}

struct Ring {
  std::unique_ptr<MsgRing> ring = std::make_unique<MsgRing>(std::size_t{1} << 20);
  venues::EventSink sink{ring.get(), venues::SinkPolicy::Drop};
  void drain() noexcept {
    while (ring->try_peek() != nullptr) ring->release();
  }
};

// Two bid and two offer levels (levels 1 and 2 of each side).
std::vector<std::byte> book_packet(std::uint32_t seq,
                                   schema::MDUpdateAction action,
                                   std::uint32_t first_rpt) {
  std::vector<std::byte> buf(kMaxPacketBytes);
  PacketBuilder p(buf);
  p.begin(seq, 1);
  std::array<MbpEntry, 4> e{};
  for (std::size_t i = 0; i < e.size(); ++i) {
    const bool bid = i < 2;
    const auto k = static_cast<std::int64_t>(i % 2);
    e[i].security_id = kSecurityId;
    e[i].rpt_seq = first_rpt + static_cast<std::uint32_t>(i);
    e[i].action = action;
    e[i].type = bid ? schema::MDEntryTypeBook::Bid : schema::MDEntryTypeBook::Offer;
    e[i].level = static_cast<std::uint8_t>(k + 1);
    e[i].price = bid ? price_ticks(18000 - k) : price_ticks(18001 + k);
    e[i].qty = 10 + static_cast<std::int32_t>(i);
  }
  schema::MatchEventIndicator mei{};
  mei.set_end_of_event(true);
  encode_book46(p, 1, mei, e);
  buf.resize(p.size());
  return buf;
}

void set_rpt(std::vector<std::byte>& pkt, std::uint32_t first) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    sbe::store_le<std::uint32_t>(pkt.data() + kEntryOffset + 32 * i + kRptOffset,
                                 first + static_cast<std::uint32_t>(i));
  }
}

}  // namespace

// Decoder only (no sequencing): four Change entries on an established book -> one BookDeltaMsg.
static void BM_Mdp3_DecodeBook46_4Entries(benchmark::State& state) {
  Ring r;
  auto dec = std::make_unique<Mdp3Decoder>();
  dec->add_instrument(kSecurityId);
  const auto init = book_packet(1, schema::MDUpdateAction::New, 1);
  dec->decode_packet(init, 1, r.sink);
  r.drain();
  auto pkt = book_packet(2, schema::MDUpdateAction::Change, 5);
  std::uint32_t rpt = 5;
  for (auto _ : state) {
    set_rpt(pkt, rpt);
    rpt += 4;
    benchmark::DoNotOptimize(dec->decode_packet(pkt, 1, r.sink));
    r.drain();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Mdp3_DecodeBook46_4Entries);

// Arbitration: every heartbeat packet arrives on A then B. Reported per packet copy.
static void BM_Mdp3_ArbitrationAB_Heartbeat(benchmark::State& state) {
  Ring r;
  Mdp3FeedConfig cfg;
  cfg.recovery_packets = 16;
  auto feed = std::make_unique<Mdp3Feed>(cfg);
  std::vector<std::byte> hb(64);
  PacketBuilder p(hb);
  p.begin(1, 1);
  encode_heartbeat12(p);
  hb.resize(p.size());
  std::uint32_t seq = 1;
  for (auto _ : state) {
    sbe::store_le<std::uint32_t>(hb.data(), seq++);
    benchmark::DoNotOptimize(feed->on_incremental(FeedLine::A, hb, 1, r.sink));
    benchmark::DoNotOptimize(feed->on_incremental(FeedLine::B, hb, 2, r.sink));
  }
  r.drain();
  state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_Mdp3_ArbitrationAB_Heartbeat);

// Full path: the four-entry Book46 packet on A (decoded) and its copy on B (dropped).
static void BM_Mdp3_FeedAB_Book46_4Entries(benchmark::State& state) {
  Ring r;
  Mdp3FeedConfig cfg;
  cfg.recovery_packets = 16;
  auto feed = std::make_unique<Mdp3Feed>(cfg);
  feed->decoder().add_instrument(kSecurityId);
  const auto init = book_packet(1, schema::MDUpdateAction::New, 1);
  feed->on_incremental(FeedLine::A, init, 1, r.sink);
  r.drain();
  auto pkt = book_packet(2, schema::MDUpdateAction::Change, 5);
  std::uint32_t seq = 2;
  std::uint32_t rpt = 5;
  for (auto _ : state) {
    sbe::store_le<std::uint32_t>(pkt.data(), seq++);
    set_rpt(pkt, rpt);
    rpt += 4;
    benchmark::DoNotOptimize(feed->on_incremental(FeedLine::A, pkt, 1, r.sink));
    benchmark::DoNotOptimize(feed->on_incremental(FeedLine::B, pkt, 2, r.sink));
    r.drain();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Mdp3_FeedAB_Book46_4Entries);
