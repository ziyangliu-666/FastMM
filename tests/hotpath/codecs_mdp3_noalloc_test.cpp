// CME MDP 3.0 hot paths allocate nothing after construction: packet framing and flyweights,
// Mdp3Decoder, A/B arbitration with duplicates and reordering, a packet gap declared by the gap
// timer, buffering while resyncing and the snapshot recovery with replay.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/codecs/mdp3/mdp3_encoder.hpp"
#include "fastmm/codecs/mdp3/mdp3_feed.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/venues/event_sink.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs::mdp3;
using fastmm::test::NoAllocScope;

namespace {

constexpr std::int32_t kSecurityId = 42;

struct Script {
  std::vector<std::vector<std::byte>> incremental;  // index = MsgSeqNum - 1
  std::vector<std::byte> snapshot;                  // loop of one packet, taken after packet 150
};

// 200 book packets of a random walk on a 10-deep book, and the snapshot after packet 150.
Script build_script() {
  Script s;
  MbpPublisher pub(kSecurityId, kMaxMbpDepth);
  Xoshiro256ss rng(7);
  std::int64_t mid = 10'000;
  std::array<MbpEntry, 2 * MbpPublisher::kMaxEntriesPerUpdate> entries{};
  schema::MatchEventIndicator mei{};
  mei.set_end_of_event(true);
  for (std::uint32_t seq = 1; seq <= 200; ++seq) {
    mid += rng.between(-1, 1);
    std::array<Level, kMaxMbpDepth> bids{};
    std::array<Level, kMaxMbpDepth> asks{};
    for (std::size_t k = 0; k < kMaxMbpDepth; ++k) {
      const auto off = static_cast<std::int64_t>(k);
      bids[k] = Level{Price::from_int(mid - 1 - off), Qty::from_int(rng.between(1, 50))};
      asks[k] = Level{Price::from_int(mid + 1 + off), Qty::from_int(rng.between(1, 50))};
    }
    std::size_t n = pub.update(Side::Buy, bids, entries);
    n += pub.update(Side::Sell, asks, std::span<MbpEntry>(entries).subspan(n));
    std::vector<std::byte> buf(kMaxPacketBytes);
    PacketBuilder p(buf);
    p.begin(seq, seq);
    REQUIRE(encode_book46(p, seq, mei, std::span<const MbpEntry>(entries.data(), n)));
    buf.resize(p.size());
    s.incremental.push_back(std::move(buf));
    if (seq == 150) {
      std::vector<std::byte> snap(kMaxPacketBytes);
      PacketBuilder sp(snap);
      sp.begin(1, seq);
      SnapshotSpec spec;
      spec.last_msg_seq_num_processed = seq;
      spec.tot_num_reports = 1;
      spec.security_id = kSecurityId;
      spec.rpt_seq = pub.rpt_seq();
      spec.transact_time = seq;
      REQUIRE(encode_snapshot52(sp, spec, pub.levels(Side::Buy), pub.levels(Side::Sell)));
      snap.resize(sp.size());
      s.snapshot = std::move(snap);
    }
  }
  return s;
}

}  // namespace

TEST_CASE("codecs.mdp3.noalloc: decode, A/B arbitration, gap and snapshot recovery") {
  const Script script = build_script();
  auto ring = std::make_unique<MsgRing>(std::size_t{1} << 22);
  venues::EventSink sink(ring.get(), venues::SinkPolicy::Drop);
  Mdp3FeedConfig cfg;
  cfg.recovery_packets = 256;
  cfg.gap_timeout_ns = 1'000;
  auto feed = std::make_unique<Mdp3Feed>(cfg);
  REQUIRE(feed->decoder().add_instrument(kSecurityId) == 0);

  const auto deliver = [&](FeedLine line, std::uint32_t seq, std::int64_t now) noexcept {
    static_cast<void>(feed->on_incremental(line, script.incremental[seq - 1], now, sink));
    while (ring->try_peek() != nullptr) ring->release();
  };

  std::uint64_t allocations = 0;
  {
    NoAllocScope guard;
    std::int64_t now = 1'000'000;
    for (std::uint32_t seq = 1; seq <= 100; ++seq) {  // A and B copies
      deliver(FeedLine::A, seq, now);
      deliver(FeedLine::B, seq, now);
      now += 10;
    }
    for (std::uint32_t seq = 101; seq <= 110; seq += 2) {  // pairs swapped on A
      deliver(FeedLine::A, seq + 1, now);
      deliver(FeedLine::A, seq, now);
    }
    for (std::uint32_t seq = 116; seq <= 150; ++seq)
      deliver(FeedLine::B, seq, now);  // 111-115 lost
    now += 10'000;
    feed->on_timer(now, sink);  // declares the gap: Resyncing, 116-150 processed and buffered
    static_cast<void>(feed->on_snapshot(script.snapshot, now, sink));
    while (ring->try_peek() != nullptr) ring->release();
    for (std::uint32_t seq = 151; seq <= 200; ++seq) deliver(FeedLine::A, seq, now);
    allocations = guard.allocations_so_far();
  }
  CHECK(allocations == 0);
  CHECK(feed->state() == ConnState::Live);
  CHECK(feed->stats().gaps == 1);
  CHECK(feed->stats().duplicates == 100);
  CHECK(feed->stats().snapshots_used == 1);
  CHECK(feed->next_expected_seq() == 201);
  CHECK(feed->decoder().recovering_count() == 0);
}
