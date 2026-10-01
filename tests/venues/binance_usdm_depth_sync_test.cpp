// USDⓈ-M depth sync ("How to manage a local order book correctly", USDⓈ-M futures): the first
// applied delta brackets the snapshot (U <= lastUpdateId <= u) and every later delta's pu must
// equal the previous u; a pu gap resyncs with a rate-limited REST snapshot.
#include "venue_test_util.hpp"

#include "fastmm/venues/binance_usdm/binance_usdm_md_feed.hpp"

#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance_usdm;
using fastmm::venues::test::RecordingSink;

namespace {

struct Requests {
  std::vector<InstrumentId> ids;
  static void on_request(void* ctx, InstrumentId id) noexcept {
    static_cast<Requests*>(ctx)->ids.push_back(id);
  }
};

struct Delta {
  alignas(64) std::byte raw[BookDeltaMsg::size_for(0, 0)] = {};
  operator const BookDeltaMsg&() const noexcept {  // NOLINT(google-explicit-constructor)
    return *reinterpret_cast<const BookDeltaMsg*>(raw);
  }
};
Delta delta(std::uint64_t U, std::uint64_t u, std::uint64_t pu, bool snapshot = false) {
  Delta out;
  auto& d = *reinterpret_cast<BookDeltaMsg*>(out.raw);
  init_header(d,
              snapshot ? EventType::BookSnapshot : EventType::BookDelta,
              InstrumentId{0},
              VenueId{0},
              BookDeltaMsg::size_for(0, 0));
  d.first_update_id = U;
  d.last_update_id = u;
  d.prev_update_id = pu;
  if (snapshot) d.hdr.flags |= EventHeader::kSnapshot;
  return out;
}
Delta snapshot(std::uint64_t last_update_id) {
  return delta(last_update_id, last_update_id, 0, true);
}
constexpr std::int64_t kSec = 1'000'000'000;

}  // namespace

TEST_CASE("binance_usdm.depth_sync: buffered deltas bracket the snapshot, then pu chains") {
  RecordingSink rs;
  Requests req;
  UsdmDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req});
  std::int64_t now = 100 * kSec;
  sync.start(now);
  CHECK(req.ids.empty());  // the snapshot is requested once the stream has an event
  // Futures update ids are not consecutive: U of the next event is not u + 1, only pu links them.
  sync.on_delta(delta(90, 95, 80), now);  // u < lastUpdateId: dropped at replay
  REQUIRE(req.ids.size() == 1);
  sync.on_delta(delta(97, 104, 95), now);  // U <= 100 <= u: the first applied delta
  sync.on_delta(delta(110, 120, 104), now);
  CHECK(rs.drain().empty());
  sync.on_snapshot(snapshot(100), now);
  CHECK(sync.synced());
  auto out = rs.drain();
  REQUIRE(out.size() == 3);
  CHECK(RecordingSink::type_of(out[0]) == EventType::BookSnapshot);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[1]).last_update_id == 104);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[2]).last_update_id == 120);
  sync.on_delta(delta(125, 130, 120), now);
  out = rs.drain();
  REQUIRE(out.size() == 1);
  CHECK(sync.last_update_id() == 130);
  CHECK(sync.resync_count() == 0);
}

TEST_CASE("binance_usdm.depth_sync: pu gap resyncs with a rate-limited snapshot request") {
  RecordingSink rs;
  Requests req;
  UsdmDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req}, 2 * kSec);
  std::int64_t now = 100 * kSec;
  sync.start(now);
  sync.on_delta(delta(95, 105, 90), now);
  sync.on_snapshot(snapshot(100), now);
  sync.on_delta(delta(106, 110, 105), now);
  CHECK(sync.synced());
  static_cast<void>(rs.drain());
  now += kSec / 2;
  sync.on_delta(delta(115, 118, 112), now);  // pu 112 != previous u 110
  CHECK_FALSE(sync.synced());
  CHECK(sync.resync_count() == 1);
  auto out = rs.drain();
  REQUIRE(out.size() == 1);
  const auto& cs = RecordingSink::as<ConnectionStateMsg>(out[0]);
  CHECK(cs.state == ConnState::Resyncing);
  CHECK(cs.channel == 0);
  CHECK(cs.reason_code == static_cast<std::int32_t>(SyncReason::SequenceGap));
  CHECK(req.ids.size() == 1);  // 0.5 s after the first request: deferred
  sync.on_timer(now + 2 * kSec);
  CHECK(req.ids.size() == 2);
  sync.on_delta(delta(119, 125, 118), now + 2 * kSec);  // buffered
  sync.on_delta(delta(126, 128, 125), now + 2 * kSec);
  sync.on_snapshot(snapshot(122), now + 2 * kSec);
  CHECK(sync.synced());
  out = rs.drain();
  REQUIRE(out.size() == 3);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[2]).last_update_id == 128);
}

TEST_CASE("binance_usdm.depth_sync: a snapshot older than the buffered stream is fetched again") {
  RecordingSink rs;
  Requests req;
  UsdmDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req}, 0);
  sync.start(kSec);
  sync.on_delta(delta(200, 210, 190), kSec);
  sync.on_snapshot(snapshot(150), kSec);  // U = 200 > 150: the snapshot does not reach the stream
  CHECK_FALSE(sync.synced());
  CHECK(req.ids.size() == 2);
  sync.on_snapshot(snapshot(205), kSec);
  CHECK(sync.synced());
  // A stale duplicate (u < previous u) is dropped without a resync.
  sync.on_delta(delta(200, 209, 190), kSec);
  CHECK(sync.synced());
  CHECK(sync.last_update_id() == 210);
}

// Production, 2026-10-01 (fastmm-live start, BTCUSDT): the snapshot was requested as the stream
// opened and came back 80 ms later, 120 ms before the stream's first event. Its lastUpdateId lay
// in the event before the first one this connection received (pu > lastUpdateId, U too), so with
// nothing buffered the snapshot was applied and the first event resynced the book: "book resync
// (sequence gap): book at update 11704574149738, next update U=11704574149894 u=11704574173288
// pu=11704574149828", and the next snapshot 2 s later. The ids below are that session's.
TEST_CASE("binance_usdm.depth_sync: a new stream's snapshot is requested after its first event") {
  constexpr std::uint64_t kOld = 11704574149738ULL;  // the snapshot taken as the stream opened
  RecordingSink rs;
  Requests req;
  UsdmDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req});
  std::int64_t now = 100 * kSec;
  sync.start(now);
  CHECK(req.ids.empty());
  sync.on_timer(now + kSec / 10);
  CHECK(req.ids.empty());
  // Requested at once, this is the snapshot that would have come back: it predates the stream.
  // Applied with nothing buffered, the first event cannot bracket it.
  CHECK(kOld < 11704574149828ULL);
  now += kSec / 5;
  sync.on_delta(delta(11704574149894ULL, 11704574173288ULL, 11704574149828ULL), now);
  REQUIRE(req.ids.size() == 1);
  CHECK_FALSE(sync.synced());
  // The snapshot requested now is newer than that buffered event: it is dropped at replay and
  // the next event brackets the snapshot.
  sync.on_snapshot(snapshot(11704574180000ULL), now + kSec / 10);
  CHECK(sync.synced());
  sync.on_delta(delta(11704574173433ULL, 11704574188958ULL, 11704574173288ULL), now + kSec / 10);
  sync.on_delta(delta(11704574188999ULL, 11704574198161ULL, 11704574188958ULL), now + kSec / 5);
  CHECK(sync.synced());
  CHECK(sync.resync_count() == 0);
  CHECK(req.ids.size() == 1);
  CHECK(sync.last_update_id() == 11704574198161ULL);
}

// A book that does not change sends no event: its snapshot is requested kStreamWait after start.
TEST_CASE("binance_usdm.depth_sync: a quiet stream gets its snapshot after kStreamWait") {
  RecordingSink rs;
  Requests req;
  UsdmDepthSync sync(InstrumentId{0}, VenueId{0}, rs.sink, {&Requests::on_request, &req});
  const std::int64_t now = 100 * kSec;
  sync.start(now);
  sync.on_timer(now + UsdmDepthSync::kStreamWait - 1);
  CHECK(req.ids.empty());
  sync.on_timer(now + UsdmDepthSync::kStreamWait);
  REQUIRE(req.ids.size() == 1);
  sync.on_snapshot(snapshot(100), now + UsdmDepthSync::kStreamWait);
  CHECK(sync.synced());
  sync.on_delta(delta(95, 105, 90), now + 10 * kSec);
  CHECK(sync.synced());
  CHECK(sync.resync_count() == 0);
  // A reconnect waits for the new stream again.
  sync.stop();
  sync.start(now + 20 * kSec);
  sync.on_timer(now + 20 * kSec);
  CHECK(req.ids.size() == 1);
  sync.on_delta(delta(200, 210, 190), now + 20 * kSec);
  CHECK(req.ids.size() == 2);
}
