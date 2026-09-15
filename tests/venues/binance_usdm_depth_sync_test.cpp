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
  REQUIRE(req.ids.size() == 1);
  // Futures update ids are not consecutive: U of the next event is not u + 1, only pu links them.
  sync.on_delta(delta(90, 95, 80), now);   // u < lastUpdateId: dropped at replay
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
  sync.on_snapshot(snapshot(100), now);
  sync.on_delta(delta(95, 105, 90), now);  // first live delta after an empty buffer
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
