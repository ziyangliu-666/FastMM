// Mdp3Feed: A/B arbitration, reordering, gap detection, snapshot recovery with replay, per
// instrument RptSeq recovery, late join and channel reset, on packets built by MbpPublisher.
#include "mdp3_test_util.hpp"

#include "fastmm/core/rng.hpp"

#include <algorithm>
#include <array>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs::mdp3;
using namespace fastmm::codecs::mdp3::test;
using venues::ParseStatus;

namespace {

constexpr std::int32_t kSid0 = 100;

std::vector<Level> ladder(Side side, std::int64_t best, std::size_t n, std::int64_t qty) {
  std::vector<Level> v;
  for (std::size_t k = 0; k < n; ++k) {
    const auto off = static_cast<std::int64_t>(k);
    v.push_back(Level{Price::from_int(side == Side::Buy ? best - off : best + off),
                      Qty::from_int(qty + off)});
  }
  return v;
}

// Publishes MBP packets for a few instruments.
struct Channel {
  std::vector<MbpPublisher> pubs;
  std::uint32_t seq = 0;

  explicit Channel(std::size_t n, std::uint8_t depth = 5) {
    for (std::size_t i = 0; i < n; ++i)
      pubs.emplace_back(kSid0 + static_cast<std::int32_t>(i), depth);
  }

  Bytes publish(std::size_t inst, const std::vector<Level>& bids, const std::vector<Level>& asks) {
    std::array<MbpEntry, 2 * MbpPublisher::kMaxEntriesPerUpdate> e{};
    std::size_t n = pubs[inst].update(Side::Buy, bids, e);
    n += pubs[inst].update(Side::Sell, asks, std::span<MbpEntry>(e).subspan(n));
    ++seq;
    return make_packet(seq, [&](PacketBuilder& p) {
      if (n == 0) {
        REQUIRE(encode_heartbeat12(p));
      } else {
        REQUIRE(encode_book46(p, seq, end_of_event(), std::span<const MbpEntry>(e.data(), n)));
      }
    });
  }
  Bytes walk(std::size_t inst, Xoshiro256ss& rng) {
    const std::int64_t best = 1000 + static_cast<std::int64_t>(inst) * 100 + rng.between(-2, 2);
    const std::int64_t spread = 1 + rng.between(0, 1);
    return publish(inst,
                   ladder(Side::Buy, best, 5, rng.between(1, 9)),
                   ladder(Side::Sell, best + spread, 5, rng.between(1, 9)));
  }
  Bytes snapshot(std::uint32_t snap_seq, std::size_t inst, std::uint32_t tot) const {
    return make_packet(snap_seq, [&](PacketBuilder& p) {
      SnapshotSpec s;
      s.last_msg_seq_num_processed = seq;
      s.tot_num_reports = tot;
      s.security_id = pubs[inst].security_id();
      s.rpt_seq = pubs[inst].rpt_seq();
      s.transact_time = seq;
      REQUIRE(encode_snapshot52(p, s, pubs[inst].levels(Side::Buy), pubs[inst].levels(Side::Sell)));
    });
  }
  Bytes channel_reset() {
    ++seq;
    for (auto& p : pubs) p.reset();
    return make_packet(seq, [&](PacketBuilder& p) { REQUIRE(encode_channel_reset4(p, seq, 1)); });
  }
};

struct Receiver {
  std::unique_ptr<Mdp3Feed> feed;
  RecordingRing ring;
  EngineBooks books;

  Receiver(std::size_t n, const Mdp3FeedConfig& cfg, std::uint8_t depth = 5)
      : feed(std::make_unique<Mdp3Feed>(cfg)), books(n) {
    for (std::size_t i = 0; i < n; ++i) {
      REQUIRE(feed->decoder().add_instrument(kSid0 + static_cast<std::int32_t>(i), depth) ==
              static_cast<std::int32_t>(i));
    }
  }
  ParseStatus inc(FeedLine line, const Bytes& p, std::int64_t rx = 1) {
    const ParseStatus st = feed->on_incremental(line, p, rx, ring.sink);
    books.apply(ring.drain());
    return st;
  }
  ParseStatus snap(const Bytes& p, std::int64_t rx = 1) {
    const ParseStatus st = feed->on_snapshot(p, rx, ring.sink);
    books.apply(ring.drain());
    return st;
  }
  void timer(std::int64_t now) {
    feed->on_timer(now, ring.sink);
    books.apply(ring.drain());
  }
  // Decoder and engine books both equal the publisher's view of instrument i.
  [[nodiscard]] bool matches(const Channel& ch, std::size_t i) const {
    const MbpBookState& b = feed->decoder().book(i);
    for (const Side side : {Side::Buy, Side::Sell}) {
      // Compare the spans directly: copying them into vectors first tripped gcc 13's LTO
      // -Wfree-nonheap-object false positive.
      const auto expect = ch.pubs[i].levels(side);
      if (!std::ranges::equal(b.sides[static_cast<std::size_t>(side)].view(), expect)) return false;
      if (!std::ranges::equal(l2_levels(*books.books[i], side), expect)) return false;
    }
    return true;
  }
};

Mdp3FeedConfig small_config() {
  Mdp3FeedConfig c;
  c.reorder_window = 8;
  c.gap_timeout_ns = 1'000;
  c.recovery_packets = 64;
  return c;
}

}  // namespace

TEST_CASE("codecs.mdp3.feed: the first copy of a packet wins and later copies are dropped") {
  Channel ch(1);
  Receiver r(1, small_config());
  Xoshiro256ss rng(1);
  for (int i = 0; i < 20; ++i) {
    const Bytes p = ch.walk(0, rng);
    CHECK(r.inc(FeedLine::A, p) == ParseStatus::Ok);
    CHECK(r.inc(FeedLine::B, p) == ParseStatus::Ignored);
    CHECK(r.matches(ch, 0));
  }
  CHECK(r.feed->state() == ConnState::Live);
  CHECK(r.feed->stats().accepted == 20);
  CHECK(r.feed->stats().duplicates == 20);
  CHECK(r.feed->stats().gaps == 0);
  CHECK(r.feed->stats().packets[0] == 20);
  CHECK(r.feed->stats().packets[1] == 20);
  REQUIRE(r.books.states.size() == 1);
  CHECK(r.books.states[0] == ConnState::Live);
}

TEST_CASE("codecs.mdp3.feed: packets lost on line A are taken from line B") {
  Channel ch(2);
  Receiver r(2, small_config());
  Xoshiro256ss rng(2);
  for (int i = 0; i < 400; ++i) {
    const std::size_t inst = rng.uniform(2);
    const Bytes p = ch.walk(inst, rng);
    const bool lose_a = rng.uniform(100) < 40;
    if (lose_a) {
      r.inc(FeedLine::B, p);
    } else if (rng.uniform(2) == 0) {
      r.inc(FeedLine::A, p);
      r.inc(FeedLine::B, p);
    } else {
      r.inc(FeedLine::B, p);
      r.inc(FeedLine::A, p);
    }
    CHECK((r.matches(ch, 0) && r.matches(ch, 1)));
  }
  CHECK(r.feed->stats().gaps == 0);
  CHECK(r.feed->state() == ConnState::Live);
  CHECK(r.feed->held_packets() == 0);
}

TEST_CASE("codecs.mdp3.feed: out-of-order packets inside the window are reordered") {
  Channel ch(1);
  Receiver r(1, small_config());
  Xoshiro256ss rng(3);
  std::vector<Bytes> pkts;
  for (int i = 0; i < 10; ++i) pkts.push_back(ch.walk(0, rng));
  for (const std::size_t k : {0U, 2U, 1U, 4U, 3U, 5U, 7U, 6U, 9U, 8U}) r.inc(FeedLine::A, pkts[k]);
  CHECK(r.feed->stats().held == 4);
  CHECK(r.feed->stats().accepted == 10);
  CHECK(r.feed->stats().gaps == 0);
  CHECK(r.feed->held_packets() == 0);
  CHECK(r.feed->next_expected_seq() == 11);
  CHECK(r.matches(ch, 0));
  // A held packet delivered twice is a duplicate.
  const Bytes p12 = [&] {
    static_cast<void>(ch.walk(0, rng));  // seq 11, never delivered yet
    return ch.walk(0, rng);              // seq 12
  }();
  CHECK(r.inc(FeedLine::A, p12) == ParseStatus::Ok);
  CHECK(r.inc(FeedLine::B, p12) == ParseStatus::Ignored);
  CHECK(r.feed->held_packets() == 1);
}

TEST_CASE("codecs.mdp3.feed: a packet beyond the reorder window declares the gap at once") {
  Channel ch(1);
  Receiver r(1, small_config());
  Xoshiro256ss rng(4);
  std::vector<Bytes> pkts;
  for (int i = 0; i < 12; ++i) pkts.push_back(ch.walk(0, rng));
  r.inc(FeedLine::A, pkts[0]);
  r.inc(FeedLine::A, pkts[11]);  // expected 2, window 8
  CHECK(r.feed->state() == ConnState::Resyncing);
  CHECK(r.feed->needs_snapshots());
  CHECK(r.feed->last_resync_reason() == ResyncReason::PacketGap);
  CHECK(r.feed->stats().gaps == 1);
  CHECK(r.feed->stats().lost_packets == 10);
  REQUIRE(r.books.states.size() == 2);
  CHECK(r.books.states[1] == ConnState::Resyncing);
  CHECK(r.books.reasons[1] == static_cast<std::int32_t>(ResyncReason::PacketGap));
  CHECK(r.feed->decoder().book(0).recovering);
  CHECK(r.feed->buffered_packets() == 1);

  CHECK(r.snap(ch.snapshot(1, 0, 1)) == ParseStatus::Ok);
  CHECK(r.feed->state() == ConnState::Live);
  CHECK(r.matches(ch, 0));
  CHECK(r.books.states.back() == ConnState::Live);
  CHECK(r.feed->stats().snapshots_used == 1);
  CHECK(r.feed->buffered_packets() == 0);
  // Snapshots are ignored while Live.
  CHECK(r.snap(ch.snapshot(1, 0, 1)) == ParseStatus::Ignored);
}

TEST_CASE("codecs.mdp3.feed: the gap timer declares a hole the other line never fills") {
  Channel ch(1);
  Receiver r(1, small_config());
  Xoshiro256ss rng(5);
  const Bytes p1 = ch.walk(0, rng);
  const Bytes p2 = ch.walk(0, rng);
  const Bytes p3 = ch.walk(0, rng);
  r.inc(FeedLine::A, p1, 1'000);
  r.inc(FeedLine::B, p3, 1'100);
  CHECK(r.feed->held_packets() == 1);
  r.timer(1'500);
  CHECK(r.feed->held_packets() == 1);
  CHECK(r.feed->state() == ConnState::Live);
  r.timer(2'200);
  CHECK(r.feed->held_packets() == 0);
  CHECK(r.feed->stats().gaps == 1);
  CHECK(r.feed->stats().lost_packets == 1);
  CHECK(r.feed->state() == ConnState::Resyncing);
  CHECK(r.feed->next_expected_seq() == 4);
  CHECK(r.inc(FeedLine::A, p2, 2'300) == ParseStatus::Ignored);  // too late
}

TEST_CASE(
    "codecs.mdp3.feed: loss on both lines recovers from the snapshot loop and replays buffered "
    "packets") {
  Channel ch(2);
  Receiver r(2, small_config());
  Xoshiro256ss rng(6);
  std::vector<Bytes> pkts;
  Bytes old_snapshot;
  std::vector<Bytes> loop;
  for (std::uint32_t i = 1; i <= 20; ++i) {
    pkts.push_back(ch.walk(i % 2, rng));
    if (i == 5) old_snapshot = ch.snapshot(1, 0, 2);
    if (i == 16) {
      loop.push_back(ch.snapshot(1, 0, 2));
      loop.push_back(ch.snapshot(2, 1, 2));
    }
  }
  for (std::uint32_t seq = 1; seq <= 10; ++seq) r.inc(FeedLine::A, pkts[seq - 1]);
  for (std::uint32_t seq = 13; seq <= 20; ++seq) r.inc(FeedLine::B, pkts[seq - 1]);  // 11, 12 lost
  CHECK(r.feed->state() == ConnState::Resyncing);
  CHECK(r.feed->stats().gaps == 1);
  CHECK(r.feed->stats().lost_packets == 2);
  CHECK(r.feed->buffered_packets() == 8);
  CHECK(r.feed->decoder().recovering_count() == 2);

  // Taken before the hole: cannot be used.
  CHECK(r.snap(old_snapshot) == ParseStatus::Ok);
  CHECK(r.feed->stats().snapshots_waiting == 1);
  CHECK(r.feed->decoder().recovering_count() == 2);

  CHECK(r.snap(loop[0]) == ParseStatus::Ok);
  CHECK_FALSE(r.feed->decoder().book(0).recovering);
  CHECK(r.matches(ch, 0));  // snapshot after 16 plus the replay of 17-20
  CHECK(r.feed->state() == ConnState::Resyncing);
  CHECK(r.snap(loop[0]) == ParseStatus::Ok);  // second copy of packet 1 restarts the loop count
  CHECK(r.snap(loop[1]) == ParseStatus::Ok);
  CHECK(r.feed->state() == ConnState::Live);
  CHECK(r.matches(ch, 1));
  CHECK(r.feed->stats().snapshots_used == 2);
  CHECK(r.feed->stats().replayed_packets == 16);
  REQUIRE(r.books.states.size() == 3);
  CHECK(r.books.states[0] == ConnState::Live);
  CHECK(r.books.states[1] == ConnState::Resyncing);
  CHECK(r.books.states[2] == ConnState::Live);
  CHECK(r.books.snapshots == 2);

  for (int i = 0; i < 6; ++i) {
    r.inc(FeedLine::A, ch.walk(static_cast<std::size_t>(i % 2), rng));
    CHECK((r.matches(ch, 0) && r.matches(ch, 1)));
  }
}

TEST_CASE("codecs.mdp3.feed: an RptSeq gap resyncs only that instrument") {
  Channel ch(2);
  Receiver r(2, small_config());
  Xoshiro256ss rng(7);
  for (int i = 0; i < 6; ++i) r.inc(FeedLine::A, ch.walk(static_cast<std::size_t>(i % 2), rng));
  static_cast<void>(ch.pubs[1].next_rpt_seq());  // an entry the receiver never sees
  r.inc(FeedLine::A, ch.publish(1, ladder(Side::Buy, 1150, 5, 7), ladder(Side::Sell, 1151, 5, 7)));
  CHECK(r.feed->state() == ConnState::Resyncing);
  CHECK(r.feed->last_resync_reason() == ResyncReason::RptSeqGap);
  CHECK(r.feed->decoder().book(1).recovering);
  CHECK_FALSE(r.feed->decoder().book(0).recovering);
  for (int i = 0; i < 4; ++i) {
    r.inc(FeedLine::A, ch.walk(0, rng));
    CHECK(r.matches(ch, 0));  // the other instrument keeps updating
  }
  r.inc(FeedLine::A, ch.walk(1, rng));
  CHECK(r.snap(ch.snapshot(1, 1, 2)) == ParseStatus::Ok);
  CHECK(r.feed->state() == ConnState::Live);
  CHECK(r.matches(ch, 1));
  CHECK(r.feed->stats().gaps == 0);
  CHECK(r.feed->stats().resyncs == 1);
}

TEST_CASE("codecs.mdp3.feed: a late join resyncs and a complete loop resets idle instruments") {
  Channel ch(4);
  Receiver r(4, small_config());
  Xoshiro256ss rng(8);
  std::vector<Bytes> pkts;
  for (int i = 0; i < 9; ++i) pkts.push_back(ch.walk(static_cast<std::size_t>(i % 3), rng));
  r.inc(FeedLine::A, pkts[6]);  // joins at packet 7; instrument 3 never traded
  CHECK(r.feed->state() == ConnState::Resyncing);
  CHECK(r.feed->last_resync_reason() == ResyncReason::LateJoin);
  CHECK(r.feed->decoder().recovering_count() == 4);
  r.inc(FeedLine::A, pkts[7]);
  r.inc(FeedLine::A, pkts[8]);
  const Bytes s1 = ch.snapshot(1, 0, 3);
  const Bytes s2 = ch.snapshot(2, 1, 3);
  const Bytes s3 = ch.snapshot(3, 2, 3);
  CHECK(r.snap(s1) == ParseStatus::Ok);
  CHECK(r.snap(s2) == ParseStatus::Ok);
  CHECK(r.snap(s2) == ParseStatus::Ignored);  // B copy
  CHECK(r.feed->stats().snapshot_duplicates == 1);
  CHECK(r.feed->decoder().book(3).recovering);
  CHECK(r.snap(s3) == ParseStatus::Ok);
  CHECK(r.feed->stats().loops_completed == 1);
  CHECK(r.feed->state() == ConnState::Live);
  for (std::size_t i = 0; i < 4; ++i) CHECK(r.matches(ch, i));
  // The idle instrument's first update sets its RptSeq baseline.
  r.inc(FeedLine::A, ch.walk(3, rng));
  CHECK(r.matches(ch, 3));
  CHECK(r.feed->decoder().stats().rpt_gaps == 0);
}

TEST_CASE("codecs.mdp3.feed: a channel reset empties every book and ends recovery") {
  Channel ch(2);
  Receiver r(2, small_config());
  Xoshiro256ss rng(9);
  for (int i = 0; i < 6; ++i) r.inc(FeedLine::A, ch.walk(static_cast<std::size_t>(i % 2), rng));
  static_cast<void>(ch.pubs[0].next_rpt_seq());
  r.inc(FeedLine::A, ch.publish(0, ladder(Side::Buy, 1150, 5, 3), ladder(Side::Sell, 1151, 5, 3)));
  CHECK(r.feed->state() == ConnState::Resyncing);
  r.inc(FeedLine::A, ch.channel_reset());
  CHECK(r.feed->state() == ConnState::Live);
  CHECK(r.feed->decoder().recovering_count() == 0);
  CHECK(r.feed->decoder().stats().channel_resets == 1);
  CHECK(r.books.books[0]->depth() == 0);
  CHECK(r.books.books[1]->depth() == 0);
  for (int i = 0; i < 6; ++i) {
    r.inc(FeedLine::A, ch.walk(static_cast<std::size_t>(i % 2), rng));
    CHECK((r.matches(ch, 0) && r.matches(ch, 1)));
  }
  CHECK(r.feed->state() == ConnState::Live);
}
