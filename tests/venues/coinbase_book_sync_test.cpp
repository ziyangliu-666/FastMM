// Coinbase Exchange book synchronisation through CoinbaseMdFeed: the snapshot first, the feed's
// own update numbering, and the loss checks on trade ids (a match that skips one, a heartbeat that
// names a trade that never came), each resyncing and resubscribing at most every 2 s; a missing
// snapshot, an unsorted one and a full ring.
#include "venue_test_util.hpp"

#include "fastmm/venues/coinbase/coinbase_md_feed.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::coinbase;
using fastmm::venues::test::RecordingSink;

namespace {

constexpr VenueId kVenue{1};
constexpr std::int64_t kSec = 1'000'000'000;

struct Resubs {
  std::vector<InstrumentId> ids;
  static void fn(void* ctx, InstrumentId id) noexcept {
    static_cast<Resubs*>(ctx)->ids.push_back(id);
  }
};

struct Fixture {
  InstrumentTable instruments;
  SymbolTable symbols;
  RecordingSink sink;
  Resubs rq;
  std::unique_ptr<CoinbaseMdFeed> feed;
  std::int64_t now = 0;
  explicit Fixture(std::size_t ring_bytes = 1U << 20) : sink(ring_bytes) {
    REQUIRE(instruments.add(venues::test::make_instrument("BTC-USD", 1, "BTC", "USD")));
    REQUIRE(symbols.build(instruments));
    feed = std::make_unique<CoinbaseMdFeed>(symbols,
                                            kVenue,
                                            sink.sink,
                                            ResubscribeRequester{&Resubs::fn, &rq},
                                            DepthChannel::Level2Batch,
                                            2 * kSec);
    REQUIRE(feed->add_instrument(InstrumentId{0}));
    feed->on_connected();  // starts the snapshot clock at steady_now()
    now = steady_now().ns;
  }
  ParseStatus send(const std::string& frame) {
    const PaddedJson j(frame);
    return feed->on_message(j.view(), now);
  }
  [[nodiscard]] bool synced() { return feed->sync(InstrumentId{0})->synced(); }
  std::vector<std::vector<std::byte>> drain() { return sink.drain(); }
};

const std::string kSnapshot =
    R"({"type":"snapshot","product_id":"BTC-USD","asks":[["60010.00","1"]],"bids":[["60000.00","1"]],"time":"2026-09-30T01:41:50Z"})";
const std::string kUpdate =
    R"({"type":"l2update","product_id":"BTC-USD","changes":[["buy","60001.00","2"]],"time":"2026-09-30T01:41:51Z"})";

std::string match(std::uint64_t id) {
  return R"({"type":"match","trade_id":)" + std::to_string(id) +
         R"(,"maker_order_id":"a","taker_order_id":"b","side":"sell","size":"0.1","price":"60010.00","product_id":"BTC-USD","sequence":1,"time":"2026-09-30T01:41:51Z"})";
}
std::string heartbeat(std::uint64_t last_trade_id) {
  return R"({"type":"heartbeat","last_trade_id":)" + std::to_string(last_trade_id) +
         R"(,"product_id":"BTC-USD","sequence":1,"time":"2026-09-30T01:41:52Z"})";
}

std::size_t resyncing(const std::vector<std::vector<std::byte>>& out, SyncReason reason) {
  std::size_t n = 0;
  for (const auto& m : out) {
    if (RecordingSink::type_of(m) != EventType::ConnectionState) continue;
    const auto& c = RecordingSink::as<ConnectionStateMsg>(m);
    n += c.state == ConnState::Resyncing && c.reason_code == static_cast<std::int32_t>(reason) ? 1U
                                                                                               : 0U;
  }
  return n;
}

}  // namespace

TEST_CASE("coinbase.book_sync: snapshot first, updates dropped before it and chained after") {
  Fixture f;
  REQUIRE(f.send(kUpdate) == ParseStatus::Ok);  // before the snapshot: discarded
  CHECK(f.drain().empty());
  REQUIRE(f.send(kSnapshot) == ParseStatus::Ok);
  CHECK(f.synced());
  REQUIRE(f.send(kUpdate) == ParseStatus::Ok);
  REQUIRE(f.send(kUpdate) == ParseStatus::Ok);
  const auto out = f.drain();
  REQUIRE(out.size() == 3);
  CHECK(RecordingSink::type_of(out[0]) == EventType::BookSnapshot);
  const auto& snap = RecordingSink::as<BookDeltaMsg>(out[0]);
  const auto& d1 = RecordingSink::as<BookDeltaMsg>(out[1]);
  const auto& d2 = RecordingSink::as<BookDeltaMsg>(out[2]);
  CHECK(d1.prev_update_id == snap.last_update_id);
  CHECK(d2.prev_update_id == d1.last_update_id);
  CHECK(f.rq.ids.empty());
}

TEST_CASE("coinbase.book_sync: a skipped trade id resyncs and resubscribes") {
  Fixture f;
  REQUIRE(f.send(kSnapshot) == ParseStatus::Ok);
  REQUIRE(f.send(match(100)) == ParseStatus::Ok);
  REQUIRE(f.send(match(101)) == ParseStatus::Ok);
  static_cast<void>(f.drain());
  REQUIRE(f.send(match(103)) == ParseStatus::Ok);  // 102 lost
  CHECK_FALSE(f.synced());
  CHECK(f.feed->stats().trade_gaps == 1);
  REQUIRE(f.rq.ids.size() == 1);
  CHECK(f.rq.ids[0] == InstrumentId{0});
  auto out = f.drain();
  CHECK(resyncing(out, SyncReason::SequenceGap) == 1);
  CHECK(RecordingSink::type_of(out.back()) == EventType::Trade);  // the trade still goes out
  // Updates wait for the new snapshot.
  REQUIRE(f.send(kUpdate) == ParseStatus::Ok);
  CHECK(f.drain().empty());
  REQUIRE(f.send(kSnapshot) == ParseStatus::Ok);
  CHECK(f.synced());
  // A trade id at or below the last one is not a gap (the last_match on subscribing).
  REQUIRE(f.send(match(103)) == ParseStatus::Ok);
  REQUIRE(f.send(match(104)) == ParseStatus::Ok);
  CHECK(f.feed->resync_count() == 1);

  // A second gap within 2 s resyncs at once but asks again only once the interval is over.
  f.now += kSec;
  REQUIRE(f.send(match(110)) == ParseStatus::Ok);
  CHECK(f.feed->resync_count() == 2);
  CHECK(f.rq.ids.size() == 1);
  f.now += kSec + 1;
  f.feed->on_timer(f.now);
  CHECK(f.rq.ids.size() == 2);
}

TEST_CASE("coinbase.book_sync: a heartbeat naming a trade that never came resyncs") {
  Fixture f;
  REQUIRE(f.send(kSnapshot) == ParseStatus::Ok);
  REQUIRE(f.send(match(100)) == ParseStatus::Ok);
  // The heartbeat names 101, and 101 arrives before the next one: nothing lost.
  REQUIRE(f.send(heartbeat(101)) == ParseStatus::Ignored);
  REQUIRE(f.send(match(101)) == ParseStatus::Ok);
  REQUIRE(f.send(heartbeat(101)) == ParseStatus::Ignored);
  CHECK(f.synced());
  CHECK(f.feed->stats().heartbeat_gaps == 0);
  // 102 is named and never comes.
  REQUIRE(f.send(heartbeat(102)) == ParseStatus::Ignored);
  CHECK(f.synced());
  REQUIRE(f.send(heartbeat(102)) == ParseStatus::Ignored);
  CHECK_FALSE(f.synced());
  CHECK(f.feed->stats().heartbeat_gaps == 1);
  CHECK(resyncing(f.drain(), SyncReason::SequenceGap) == 1);
  REQUIRE(f.rq.ids.size() == 1);
  // Reported once: the next trade is 103 and is no gap.
  REQUIRE(f.send(kSnapshot) == ParseStatus::Ok);
  REQUIRE(f.send(match(103)) == ParseStatus::Ok);
  REQUIRE(f.send(heartbeat(103)) == ParseStatus::Ignored);
  REQUIRE(f.send(heartbeat(103)) == ParseStatus::Ignored);
  CHECK(f.synced());
  CHECK(f.feed->resync_count() == 1);
}

TEST_CASE("coinbase.book_sync: no snapshot within the timeout resubscribes") {
  Fixture f;
  f.feed->on_timer(f.now + 5 * kSec);
  CHECK(f.rq.ids.empty());
  f.feed->on_timer(f.now + 10 * kSec);
  REQUIRE(f.rq.ids.size() == 1);
}

TEST_CASE("coinbase.book_sync: an unsorted snapshot and a full ring resync") {
  Fixture f;
  REQUIRE(f.send(kSnapshot) == ParseStatus::Ok);
  static_cast<void>(f.drain());
  CHECK(
      f.send(
          R"({"type":"snapshot","product_id":"BTC-USD","asks":[["60010.00","1"]],"bids":[["60000.00","1"],["60005.00","1"]],"time":"2026-09-30T01:41:50Z"})") ==
      ParseStatus::Malformed);
  CHECK_FALSE(f.synced());
  CHECK(f.rq.ids.size() == 1);
  CHECK(resyncing(f.drain(), SyncReason::Explicit) == 1);

  // A ring with no room drops the update: the book is resynced.
  Fixture small(4096);
  REQUIRE(small.send(kSnapshot) == ParseStatus::Ok);
  for (int i = 0; i < 64 && small.synced(); ++i) static_cast<void>(small.send(kUpdate));
  CHECK_FALSE(small.synced());
  CHECK(small.rq.ids.size() == 1);
}
