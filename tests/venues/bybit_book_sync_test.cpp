#include "fastmm/venues/bybit/bybit_book_sync.hpp"

#include "venue_test_util.hpp"

#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using fastmm::venues::test::RecordingSink;

namespace {

struct Resubs {
  std::vector<InstrumentId> ids;
  static void fn(void* ctx, InstrumentId id) noexcept {
    static_cast<Resubs*>(ctx)->ids.push_back(id);
  }
};

// Zero-level BookDeltaMsg backed by a full size_for(0, 0) buffer (hdr.len is 128, the struct 96).
struct Msg {
  alignas(64) std::byte raw[BookDeltaMsg::size_for(0, 0)] = {};
  operator const BookDeltaMsg&() const noexcept {  // NOLINT(google-explicit-constructor)
    return *reinterpret_cast<const BookDeltaMsg*>(raw);
  }
};
Msg book(std::uint64_t u, bool snapshot) {
  Msg out;
  auto& d = *reinterpret_cast<BookDeltaMsg*>(out.raw);
  init_header(d,
              snapshot ? EventType::BookSnapshot : EventType::BookDelta,
              InstrumentId{2},
              VenueId{1},
              BookDeltaMsg::size_for(0, 0));
  d.first_update_id = u;
  d.last_update_id = u;
  if (snapshot) d.hdr.flags |= EventHeader::kSnapshot;
  return out;
}
constexpr std::int64_t kSec = 1'000'000'000;

}  // namespace

TEST_CASE("bybit.book_sync: snapshot first, deltas dropped before it") {
  RecordingSink rs;
  Resubs rq;
  BybitBookSync sync(InstrumentId{2}, VenueId{1}, rs.sink, {&Resubs::fn, &rq});
  sync.start(kSec);
  sync.on_book(book(10, false), kSec);  // before the snapshot: discarded
  CHECK(rs.drain().empty());
  sync.on_book(book(10, true), kSec);
  CHECK(sync.synced());
  sync.on_book(book(11, false), kSec);
  sync.on_book(book(12, false), kSec);
  sync.on_book(book(12, false), kSec);  // duplicate (rollover overlap): dropped
  const auto out = rs.drain();
  REQUIRE(out.size() == 3);
  CHECK(RecordingSink::type_of(out[0]) == EventType::BookSnapshot);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[2]).last_update_id == 12);
  CHECK(rq.ids.empty());
}

TEST_CASE("bybit.book_sync: u == 1 marker and a stale u resync and resubscribe") {
  RecordingSink rs;
  Resubs rq;
  BybitBookSync sync(InstrumentId{2}, VenueId{1}, rs.sink, {&Resubs::fn, &rq}, 2 * kSec);
  std::int64_t now = 100 * kSec;
  sync.start(now);
  sync.on_book(book(50, true), now);
  sync.on_book(book(51, false), now);
  static_cast<void>(rs.drain());
  sync.on_book(book(1, false), now);  // venue restart marker on a delta
  CHECK_FALSE(sync.synced());
  CHECK(sync.resync_count() == 1);
  REQUIRE(rq.ids.size() == 1);
  auto out = rs.drain();
  REQUIRE(out.size() == 1);
  CHECK(RecordingSink::as<ConnectionStateMsg>(out[0]).state == ConnState::Resyncing);
  CHECK(RecordingSink::as<ConnectionStateMsg>(out[0]).reason_code ==
        static_cast<std::int32_t>(SyncReason::SnapshotMarker));
  // The fresh snapshot after resubscribing restarts the numbering.
  sync.on_book(book(1, true), now);
  CHECK(sync.synced());
  sync.on_book(book(2, false), now);
  CHECK(rs.drain().size() == 2);
  // A second resync within min_interval is deferred to on_timer.
  now += kSec / 2;
  sync.resync(SyncReason::Explicit, now);
  CHECK(rq.ids.size() == 1);
  sync.on_timer(now + 2 * kSec);
  CHECK(rq.ids.size() == 2);
}

TEST_CASE("bybit.book_sync: no snapshot within the timeout resubscribes") {
  RecordingSink rs;
  Resubs rq;
  BybitBookSync sync(InstrumentId{2}, VenueId{1}, rs.sink, {&Resubs::fn, &rq}, 0, 10 * kSec);
  sync.start(kSec);
  sync.on_timer(5 * kSec);
  CHECK(rq.ids.empty());
  sync.on_timer(12 * kSec);
  CHECK(rq.ids.size() == 1);
  sync.stop();
  sync.on_timer(100 * kSec);
  CHECK(rq.ids.size() == 1);
}
