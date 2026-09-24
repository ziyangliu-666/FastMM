#include "fastmm/venues/binance/binance_depth_sync.hpp"

#include "venue_test_util.hpp"

#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using fastmm::venues::test::RecordingSink;

namespace {

struct Requests {
  std::vector<InstrumentId> ids;
  static void on_request(void* ctx, InstrumentId id) noexcept {
    static_cast<Requests*>(ctx)->ids.push_back(id);
  }
};

// A BookDeltaMsg with zero levels is 96 bytes but reports len == 128 (64-byte granule), so
// back it with a full 128-byte buffer.
struct Delta {
  alignas(64) std::byte raw[BookDeltaMsg::size_for(0, 0)] = {};
  operator const BookDeltaMsg&() const noexcept {  // NOLINT(google-explicit-constructor)
    return *reinterpret_cast<const BookDeltaMsg*>(raw);
  }
};
Delta delta(std::uint64_t U, std::uint64_t u, bool snapshot = false) {
  Delta out;
  auto& d = *reinterpret_cast<BookDeltaMsg*>(out.raw);
  init_header(d,
              snapshot ? EventType::BookSnapshot : EventType::BookDelta,
              InstrumentId{0},
              VenueId{0},
              BookDeltaMsg::size_for(0, 0));
  d.first_update_id = U;
  d.last_update_id = u;
  if (snapshot) d.hdr.flags |= EventHeader::kSnapshot;
  return out;
}
constexpr std::int64_t kSec = 1'000'000'000;

}  // namespace

TEST_CASE("binance.depth_sync: buffer, snapshot, replay, then live deltas") {
  RecordingSink rs;
  Requests req;
  BinanceDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req});
  std::int64_t now = 100 * kSec;
  sync.start(now);
  CHECK(sync.state() == SyncState::Buffering);
  REQUIRE(req.ids.size() == 1);
  sync.on_delta(delta(5, 7), now);
  sync.on_delta(delta(8, 10), now);
  sync.on_delta(delta(11, 12), now);
  CHECK(rs.drain().empty());                 // nothing reaches the engine before the snapshot
  sync.on_snapshot(delta(9, 9, true), now);  // L=9
  CHECK(sync.synced());
  auto out = rs.drain();
  REQUIRE(out.size() == 3);
  CHECK(RecordingSink::type_of(out[0]) == EventType::BookSnapshot);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[0]).is_snapshot());
  CHECK(RecordingSink::as<BookDeltaMsg>(out[1]).first_update_id == 8);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[2]).first_update_id == 11);
  sync.on_delta(delta(13, 15), now);
  out = rs.drain();
  REQUIRE(out.size() == 1);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[0]).last_update_id == 15);
  CHECK(sync.last_update_id() == 15);
  sync.on_delta(delta(13, 15), now);  // duplicate (stale) -> dropped silently
  CHECK(rs.drain().empty());
  CHECK(sync.synced());
}

TEST_CASE("binance.depth_sync: gap -> Resyncing event + rate-limited re-snapshot") {
  RecordingSink rs;
  Requests req;
  BinanceDepthSync sync(
      InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req}, 2 * kSec);
  std::int64_t now = 100 * kSec;
  sync.start(now);
  sync.on_snapshot(delta(9, 9, true), now);
  sync.on_delta(delta(10, 10), now);
  static_cast<void>(rs.drain());
  now += 500'000'000;
  sync.on_delta(delta(12, 13), now);  // U != prev_u + 1
  CHECK_FALSE(sync.synced());
  CHECK(sync.resync_count() == 1);
  auto out = rs.drain();
  REQUIRE(out.size() == 1);
  CHECK(RecordingSink::type_of(out[0]) == EventType::ConnectionState);
  const auto& cs = RecordingSink::as<ConnectionStateMsg>(out[0]);
  CHECK(cs.state == ConnState::Resyncing);
  CHECK(cs.channel == 0);
  CHECK(cs.reason_code == static_cast<std::int32_t>(SyncReason::SequenceGap));
  CHECK(cs.hdr.instrument == InstrumentId{0});
  // Second request is deferred: only 0.5 s since the first one.
  CHECK(req.ids.size() == 1);
  CHECK(sync.deferred_requests() >= 1);
  sync.on_timer(now + kSec);  // 1.5 s: still deferred
  CHECK(req.ids.size() == 1);
  sync.on_timer(now + 2 * kSec);  // 2.5 s: issued
  CHECK(req.ids.size() == 2);
  CHECK(sync.request_pending());
  // The gap-causing delta itself is discarded with the book (core BookSyncer); later deltas
  // are buffered and replayed once a snapshot at or past the gap arrives.
  sync.on_delta(delta(14, 14), now + 2 * kSec);
  sync.on_snapshot(delta(13, 13, true), now + 2 * kSec);
  CHECK(sync.synced());
  out = rs.drain();
  REQUIRE(out.size() == 2);
  CHECK(RecordingSink::type_of(out[0]) == EventType::BookSnapshot);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[1]).first_update_id == 14);
}

TEST_CASE("binance.depth_sync: snapshot too old is retried, failure is retried") {
  RecordingSink rs;
  Requests req;
  BinanceDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req}, 0);
  std::int64_t now = kSec;
  sync.start(now);
  sync.on_delta(delta(20, 21), now);
  sync.on_snapshot(delta(5, 5, true), now);  // older than the first buffered U
  CHECK_FALSE(sync.synced());
  CHECK(req.ids.size() == 2);
  auto out = rs.drain();
  REQUIRE(out.size() == 1);
  CHECK(RecordingSink::as<ConnectionStateMsg>(out[0]).reason_code ==
        static_cast<std::int32_t>(SyncReason::SnapshotTooOld));
  sync.on_snapshot_failed(now);
  CHECK(req.ids.size() == 3);
  sync.on_snapshot(delta(20, 20, true), now);
  CHECK(sync.synced());
  out = rs.drain();
  REQUIRE(out.size() == 2);
}

TEST_CASE("binance.depth_sync: a request that fails synchronously is retried by the timer") {
  // A connector with no REST connection fails the request from inside it. With no minimum
  // interval, retrying there recursed until the stack ran out.
  struct Failing {
    BinanceDepthSync* sync = nullptr;
    int calls = 0;
    static void on_request(void* ctx, InstrumentId) noexcept {
      auto* f = static_cast<Failing*>(ctx);
      ++f->calls;
      f->sync->on_snapshot_failed(kSec);
    }
  };
  RecordingSink rs;
  Failing f;
  BinanceDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Failing::on_request, &f}, 0);
  f.sync = &sync;
  sync.start(kSec);
  CHECK(f.calls == 1);
  sync.on_timer(2 * kSec);
  CHECK(f.calls == 2);
}

TEST_CASE("binance.depth_sync: full market-data ring forces a resync") {
  RecordingSink rs(64 * 4);  // room for ~2 messages
  Requests req;
  BinanceDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req}, 0);
  sync.start(0);
  sync.on_snapshot(delta(1, 1, true), 0);
  sync.on_delta(delta(2, 2), 0);
  CHECK(sync.synced());
  sync.on_delta(delta(3, 3), 0);  // ring full: dropped, resync
  CHECK_FALSE(sync.synced());
  CHECK(rs.sink.overflows() >= 1);
  CHECK(req.ids.size() == 2);
}
