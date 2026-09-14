// DeribitBookSync: change_id / prev_change_id chaining (book.{instrument}.{interval}), gap ->
// Resyncing + resubscribe, duplicates dropped, snapshot timeout, and the recorded testnet chain.
#include "fastmm/venues/deribit/deribit_book_sync.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/deribit/deribit_md_parser.hpp"

#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::deribit;
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
Msg book(std::uint64_t change_id, std::uint64_t prev, bool snapshot) {
  Msg out;
  auto& d = *reinterpret_cast<BookDeltaMsg*>(out.raw);
  init_header(d,
              snapshot ? EventType::BookSnapshot : EventType::BookDelta,
              InstrumentId{0},
              VenueId{2},
              BookDeltaMsg::size_for(0, 0));
  d.first_update_id = change_id;
  d.last_update_id = change_id;
  d.prev_update_id = prev;
  if (snapshot) d.hdr.flags |= EventHeader::kSnapshot;
  return out;
}
constexpr std::int64_t kSec = 1'000'000'000;

}  // namespace

TEST_CASE("deribit.book_sync: snapshot first, then changes chained by prev_change_id") {
  RecordingSink rs;
  Resubs rq;
  DeribitBookSync sync(InstrumentId{0}, VenueId{2}, rs.sink, {&Resubs::fn, &rq});
  sync.start(kSec);
  sync.on_book(book(118850969743, 118850968930, false), kSec);  // before the snapshot: dropped
  CHECK(rs.drain().empty());
  sync.on_book(book(118850968930, 0, true), kSec);
  CHECK(sync.synced());
  // Recorded testnet chain (book_perp_*.json): ids are not consecutive integers.
  sync.on_book(book(118850969743, 118850968930, false), kSec);
  sync.on_book(book(118850970615, 118850969743, false), kSec);
  sync.on_book(book(118850970615, 118850969743, false), kSec);  // duplicate: dropped
  const auto out = rs.drain();
  REQUIRE(out.size() == 3);
  CHECK(RecordingSink::type_of(out[0]) == EventType::BookSnapshot);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[2]).last_update_id == 118850970615);
  CHECK(sync.last_change_id() == 118850970615);
  CHECK(rq.ids.empty());
  CHECK(sync.resync_count() == 0);
}

TEST_CASE("deribit.book_sync: a missed change resyncs and resubscribes, rate limited") {
  RecordingSink rs;
  Resubs rq;
  DeribitBookSync sync(InstrumentId{0}, VenueId{2}, rs.sink, {&Resubs::fn, &rq}, 2 * kSec);
  std::int64_t now = 100 * kSec;
  sync.start(now);
  sync.on_book(book(10, 0, true), now);
  sync.on_book(book(15, 10, false), now);
  static_cast<void>(rs.drain());
  sync.on_book(book(30, 20, false), now);  // prev 20 != 15: a notification was missed
  CHECK_FALSE(sync.synced());
  CHECK(sync.resync_count() == 1);
  REQUIRE(rq.ids.size() == 1);
  CHECK(rq.ids[0] == InstrumentId{0});
  auto out = rs.drain();
  REQUIRE(out.size() == 1);
  CHECK(RecordingSink::type_of(out[0]) == EventType::ConnectionState);
  CHECK(RecordingSink::as<ConnectionStateMsg>(out[0]).state == ConnState::Resyncing);
  CHECK(RecordingSink::as<ConnectionStateMsg>(out[0]).reason_code ==
        static_cast<std::int32_t>(SyncReason::SequenceGap));
  // Changes before the new snapshot are ignored; the snapshot restarts the chain.
  sync.on_book(book(31, 30, false), now);
  CHECK(rs.drain().empty());
  sync.on_book(book(40, 0, true), now);
  CHECK(sync.synced());
  sync.on_book(book(41, 40, false), now);
  CHECK(rs.drain().size() == 2);
  // A second gap within min_interval: resubscribe deferred to on_timer.
  now += kSec / 2;
  sync.on_book(book(50, 45, false), now);
  CHECK(rq.ids.size() == 1);
  sync.on_timer(now + 2 * kSec);
  CHECK(rq.ids.size() == 2);
}

TEST_CASE(
    "deribit.book_sync: a stale change after the snapshot is dropped, no snapshot times out") {
  RecordingSink rs;
  Resubs rq;
  DeribitBookSync sync(InstrumentId{0}, VenueId{2}, rs.sink, {&Resubs::fn, &rq}, 0, 10 * kSec);
  sync.start(kSec);
  sync.on_timer(5 * kSec);
  CHECK(rq.ids.empty());
  sync.on_timer(12 * kSec);
  CHECK(rq.ids.size() == 1);
  sync.on_book(book(100, 0, true), 13 * kSec);
  sync.on_book(book(99, 98, false), 13 * kSec);  // older than the snapshot (in-flight): dropped
  CHECK(sync.synced());
  CHECK(sync.resync_count() == 0);
  sync.stop();
  sync.on_timer(100 * kSec);
  CHECK(rq.ids.size() == 1);
}

TEST_CASE("deribit.book_sync: the recorded testnet chain parses and stays synced") {
  InstrumentTable instruments;
  Instrument perp = fastmm::venues::test::make_instrument("BTC-PERPETUAL", 2, "BTC", "USD");
  perp.asset_class = AssetClass::Perpetual;
  perp.contract_multiplier = Qty::from_int(10);
  REQUIRE(instruments.add(perp));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  DeribitMdParser parser(symbols, instruments, VenueId{2});
  RecordingSink rs;
  Resubs rq;
  DeribitBookSync sync(InstrumentId{0}, VenueId{2}, rs.sink, {&Resubs::fn, &rq});
  sync.start(kSec);
  fastmm::venues::test::Scratch scratch;
  for (const char* f : {"deribit/book_perp_snapshot.json",
                        "deribit/book_perp_change_1.json",
                        "deribit/book_perp_change_2.json"}) {
    const PaddedJson j = fastmm::venues::test::padded_fixture(f);
    const MdDecodeResult r = parser.decode(j.view(), wall_now(), rdtscp(), scratch.span());
    REQUIRE(r.ok());
    sync.on_book(scratch.as<BookDeltaMsg>(), kSec);
  }
  CHECK(sync.synced());
  CHECK(rs.drain().size() == 3);
  const PaddedJson gap = fastmm::venues::test::padded_fixture("deribit/book_perp_change_gap.json");
  REQUIRE(parser.decode(gap.view(), wall_now(), rdtscp(), scratch.span()).ok());
  sync.on_book(scratch.as<BookDeltaMsg>(), kSec);
  CHECK_FALSE(sync.synced());
  CHECK(rq.ids.size() == 1);
}
