// Coinbase Exchange public-feed decoding on frames recorded from the production feed (2026-09-30,
// tests/fixtures/coinbase/fixtures.meta.json) and on frames built to the documented format:
// snapshots cut to a message, updates split by side, trades with the taker's side, control frames
// and errors; then the feed over the recorded session.
#include "fastmm/venues/coinbase/coinbase_md_parser.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/venues/coinbase/coinbase_md_feed.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::coinbase;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kVenue{1};

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(venues::test::make_instrument("BTC-USD", 1, "BTC", "USD")));  // id 0
    REQUIRE(instruments.add(venues::test::make_instrument("ETH-USD", 1, "ETH", "USD")));  // id 1
    REQUIRE(symbols.build(instruments));
  }
};

// The recorded session's frames (tab-separated receive time and frame, one per line).
std::vector<std::string> recorded_frames() {
  const std::string raw = fastmm::test::fixture("coinbase/raw_md_stream.jsonl");
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t end = raw.find('\n', pos);
    if (end == std::string::npos) end = raw.size();
    const std::string line = raw.substr(pos, end - pos);
    pos = end + 1;
    const std::size_t tab = line.find('\t');
    if (tab != std::string::npos) out.push_back(line.substr(tab + 1));
  }
  return out;
}

// A snapshot of `n` levels a side around 60000, best first unless `unsorted`.
std::string snapshot(int n, bool unsorted = false) {
  std::string bids;
  std::string asks;
  for (int i = 0; i < n; ++i) {
    const int b = unsorted && i == 1 ? 60001 : 60000 - i;
    bids += (i != 0 ? "," : "") + std::string("[\"") + std::to_string(b) + ".00\",\"0.5\"]";
    asks +=
        (i != 0 ? "," : "") + std::string("[\"") + std::to_string(60010 + i) + ".00\",\"0.25\"]";
  }
  return R"({"type":"snapshot","product_id":"BTC-USD","asks":[)" + asks + R"(],"bids":[)" + bids +
         R"(],"time":"2026-09-30T01:41:50.644756Z"})";
}

}  // namespace

TEST_CASE("coinbase.md_parser: recorded snapshot, updates, trades and heartbeats") {
  Universe u;
  CoinbaseMdParser p(u.symbols, kVenue);
  Scratch s;
  std::size_t snaps = 0;
  std::size_t deltas = 0;
  std::size_t trades = 0;
  std::size_t heartbeats = 0;
  for (const std::string& f : recorded_frames()) {
    const PaddedJson j(f);
    const MdDecodeResult r = p.decode(j.view(), Timestamp{1}, Cycles{2}, s.span());
    REQUIRE(r.status != ParseStatus::Malformed);
    REQUIRE(r.status != ParseStatus::UnknownSymbol);
    if (r.kind == MdKind::BookSnapshot) {
      ++snaps;
      const auto& m = s.as<BookDeltaMsg>();
      CHECK(m.is_snapshot());
      CHECK(m.bid_count == 150);
      CHECK(m.ask_count == 150);
      CHECK(m.bids()[0].price > m.bids()[1].price);  // best first
      CHECK(m.asks()[0].price < m.asks()[1].price);
      CHECK(m.bids()[0].price < m.asks()[0].price);
      CHECK(m.hdr.exch_ts.ns > 0);
    } else if (r.kind == MdKind::BookDelta) {
      ++deltas;
      const auto& m = s.as<BookDeltaMsg>();
      CHECK(m.bid_count + m.ask_count > 0);
    } else if (r.kind == MdKind::Trade) {
      ++trades;
      CHECK(s.as<TradeMsg>().trade_id == r.trade_id);
    } else if (r.control == ControlOp::Heartbeat) {
      ++heartbeats;
      CHECK(r.trade_id > 0);
    }
  }
  CHECK(snaps == 2);
  CHECK(deltas == 314);
  CHECK(trades == 61);
  CHECK(heartbeats == 20);
  CHECK(p.stats().malformed == 0);
}

TEST_CASE("coinbase.md_parser: match side is the maker's, control frames and errors") {
  Universe u;
  CoinbaseMdParser p(u.symbols, kVenue);
  Scratch s;
  const PaddedJson match(
      R"({"type":"match","trade_id":10,"maker_order_id":"ac928c66-ca53-498f-9c13-a110027a60e8","taker_order_id":"132fb6ae-456b-4654-b4e0-d681ac05cea1","side":"sell","size":"5.23512","price":"400.23","product_id":"ETH-USD","sequence":50,"time":"2014-11-07T08:19:27.028459Z"})");
  MdDecodeResult r = p.decode(match.view(), Timestamp{1}, Cycles{2}, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& t = s.as<TradeMsg>();
  CHECK(t.hdr.instrument == InstrumentId{1});
  CHECK(t.aggressor == Side::Buy);  // a sell maker: the taker bought (an up-tick)
  CHECK(t.price == Price::from_decimal("400.23").value());
  CHECK(t.qty == Qty::from_decimal("5.23512").value());
  CHECK(t.trade_id == 10);
  CHECK(t.hdr.exch_ts == Timestamp{1415348367028459000});

  const PaddedJson last(
      R"({"type":"last_match","trade_id":9,"maker_order_id":"a","taker_order_id":"b","side":"buy","size":"1","price":"1","product_id":"BTC-USD","sequence":1,"time":"2014-11-07T08:19:27.028459Z"})");
  r = p.decode(last.view(), Timestamp{1}, Cycles{2}, s.span());
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.control == ControlOp::LastMatch);
  CHECK(r.trade_id == 9);

  const PaddedJson hb(
      R"({"type":"heartbeat","last_trade_id":20,"product_id":"BTC-USD","sequence":90,"time":"2014-11-07T08:19:28.464459Z"})");
  r = p.decode(hb.view(), Timestamp{1}, Cycles{2}, s.span());
  CHECK(r.control == ControlOp::Heartbeat);
  CHECK(r.trade_id == 20);
  CHECK(r.instrument == InstrumentId{0});

  const PaddedJson err(
      R"({"type":"error","message":"Failed to subscribe","reason":"level2, level3, and full channels now require authentication."})");
  r = p.decode(err.view(), Timestamp{1}, Cycles{2}, s.span());
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.control == ControlOp::Error);
  CHECK(r.msg == "Failed to subscribe");
  CHECK(r.reason.find("require authentication") != std::string_view::npos);

  const PaddedJson subs(R"({"type":"subscriptions","channels":[]})");
  CHECK(p.decode(subs.view(), Timestamp{1}, Cycles{2}, s.span()).control ==
        ControlOp::Subscriptions);
  const PaddedJson ticker(R"({"type":"ticker","product_id":"BTC-USD","price":"1"})");
  CHECK(p.decode(ticker.view(), Timestamp{1}, Cycles{2}, s.span()).status == ParseStatus::Ignored);
  const PaddedJson unknown(
      R"({"type":"l2update","product_id":"SOL-USD","changes":[["buy","1","1"]],"time":"2026-09-30T01:41:50Z"})");
  CHECK(p.decode(unknown.view(), Timestamp{1}, Cycles{2}, s.span()).status ==
        ParseStatus::UnknownSymbol);
  for (const char* bad :
       {R"({"type":"l2update","product_id":"BTC-USD","changes":[["hold","1","1"]]})",
        R"({"type":"l2update","product_id":"BTC-USD","changes":[["buy","x","1"]]})",
        R"({"type":"match","product_id":"BTC-USD","side":"buy"})",
        R"({"type":"snapshot","product_id":"BTC-USD","changes":[]})",
        R"(not json)"}) {
    CAPTURE(bad);
    const PaddedJson j(bad);
    CHECK(p.decode(j.view(), Timestamp{1}, Cycles{2}, s.span()).status == ParseStatus::Malformed);
  }
}

TEST_CASE("coinbase.md_parser: a deep snapshot keeps the best levels, an unsorted one is refused") {
  Universe u;
  CoinbaseMdParser p(u.symbols, kVenue);
  Scratch s;
  const PaddedJson deep(snapshot(3000));
  MdDecodeResult r = p.decode(deep.view(), Timestamp{1}, Cycles{2}, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.bid_count == kMaxBookLevelsPerMsg);
  CHECK(m.ask_count == kMaxBookLevelsPerMsg);
  CHECK(m.bids()[0].price == Price::from_int(60000));
  CHECK(m.bids()[kMaxBookLevelsPerMsg - 1].price == Price::from_int(60000 - 1023));
  CHECK(m.asks()[0].price == Price::from_int(60010));
  CHECK(p.stats().snapshot_cut == 1);

  // Out of order: refused, and the product named so the feed can start its book over.
  const PaddedJson bad(snapshot(5, true));
  r = p.decode(bad.view(), Timestamp{1}, Cycles{2}, s.span());
  CHECK(r.status == ParseStatus::Malformed);
  CHECK(r.instrument == InstrumentId{0});
}

TEST_CASE(
    "coinbase.md_parser: an update past a message a side keeps the levels nearest the touch") {
  Universe u;
  CoinbaseMdParser p(u.symbols, kVenue);
  Scratch s;
  // 1500 bid changes, worst first, and two asks.
  std::string changes;
  for (int i = 0; i < 1500; ++i)
    changes +=
        (i != 0 ? "," : "") + std::string(R"(["buy",")") + std::to_string(50000 + i) + R"(","1"])";
  changes += R"(,["sell","70000","0"],["sell","69999","2"])";
  const PaddedJson j(R"({"type":"l2update","product_id":"BTC-USD","changes":[)" + changes +
                     R"(],"time":"2026-09-30T01:41:50.644756Z"})");
  const MdDecodeResult r = p.decode(j.view(), Timestamp{1}, Cycles{2}, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.bid_count == kMaxBookLevelsPerMsg);
  CHECK(m.ask_count == 2);
  CHECK((m.hdr.flags & EventHeader::kTruncatedBids) != 0);
  CHECK((m.hdr.flags & EventHeader::kTruncatedAsks) == 0);
  Price lowest = Price::from_int(1'000'000);
  for (const Level& l : m.bids()) lowest = std::min(lowest, l.price);
  CHECK(lowest == Price::from_int(50000 + 1500 - 1024));  // the 1024 highest bids
  CHECK(m.asks()[0].qty.is_zero());
  CHECK(p.stats().truncated == 1);
}

TEST_CASE("coinbase.md_feed: the recorded session syncs both books with no resync") {
  Universe u;
  RecordingSink sink(32U << 20);
  int requests = 0;
  CoinbaseMdFeed feed(
      u.symbols,
      kVenue,
      sink.sink,
      ResubscribeRequester{[](void* ctx, InstrumentId) noexcept { ++*static_cast<int*>(ctx); },
                           &requests});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  REQUIRE(feed.add_instrument(InstrumentId{1}));
  REQUIRE(feed.subscription_payloads().size() == 1);
  CHECK(
      feed.subscription_payloads()[0] ==
      R"({"type":"subscribe","product_ids":["BTC-USD","ETH-USD"],"channels":["level2_batch","matches","heartbeat"]})");
  feed.on_connected();
  std::int64_t now = 1'000'000'000;
  for (const std::string& f : recorded_frames()) {
    const PaddedJson j(f);
    static_cast<void>(feed.on_message(j.view(), now));
    now += 10'000'000;
  }
  CHECK(feed.synced_count() == 2);
  CHECK(feed.resync_count() == 0);
  CHECK(feed.stats().trade_gaps == 0);
  CHECK(feed.stats().heartbeat_gaps == 0);
  CHECK(requests == 0);
  venues::test::Collected c;
  c.take(sink);
  CHECK(c.count(EventType::BookSnapshot) == 2);
  CHECK(c.count(EventType::BookDelta) == 314);
  CHECK(c.count(EventType::Trade) == 61);
  CHECK(c.count(EventType::ConnectionState) == 0);
  // The feed's own numbering chains every update onto the one before.
  const auto* first = c.first_if<BookDeltaMsg>(EventType::BookDelta, [](const BookDeltaMsg& m) {
    return m.hdr.instrument == InstrumentId{0};
  });
  REQUIRE(first != nullptr);
  CHECK(first->prev_update_id + 1 == first->last_update_id);
}
