// OKX v5 public stream: the books / bbo-tbt / trades decoder, the seqId chain and the legacy
// checksum. Frames follow the order book and trades channel pages
// (https://www.okx.com/docs-v5/en/#order-book-trading-market-data-ws-order-book-channel, read
// 2026-09-26); reference CRC values come from Python's zlib.crc32.
#include "fastmm/venues/okx/okx_md_parser.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/okx/okx_md_feed.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::okx;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kOkx{1};

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(make_instrument("BTC-USDT-SWAP", 1, "BTC", "USDT")));  // id 0
    REQUIRE(symbols.build(instruments));
  }
};

std::string books(const char* action,
                  const char* asks,
                  const char* bids,
                  long long checksum,
                  long long prev,
                  long long seq) {
  return std::string(R"({"arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"action":")") + action +
         R"(","data":[{"asks":[)" + asks + R"(],"bids":[)" + bids +
         R"(],"ts":"1597026383085","checksum":)" + std::to_string(checksum) + R"(,"prevSeqId":)" +
         std::to_string(prev) + R"(,"seqId":)" + std::to_string(seq) + "}]}";
}

// The documented snapshot, on BTC-USDT-SWAP.
const std::string kSnapshot = books("snapshot",
                                    R"(["8476.98","415","0","13"],["8477","7","0","2"])",
                                    R"(["8476.97","256","0","12"],["8475.55","101","0","1"])",
                                    0,
                                    -1,
                                    10);

Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}
Price px(const char* s) {
  return Price::from_decimal(s).value();
}

MdDecodeResult decode(OkxMdParser& p, const std::string& frame, Scratch& s) {
  const PaddedJson j(frame);
  return p.decode(j.view(), Timestamp{1}, Cycles{}, s.span());
}

struct Resubscribes {
  std::vector<InstrumentId> ids;
  static void fn(void* ctx, InstrumentId id) noexcept {
    static_cast<Resubscribes*>(ctx)->ids.push_back(id);
  }
};

std::vector<ConnState> states(const std::vector<std::vector<std::byte>>& msgs) {
  std::vector<ConnState> out;
  for (const auto& m : msgs) {
    if (RecordingSink::type_of(m) == EventType::ConnectionState)
      out.push_back(RecordingSink::as<ConnectionStateMsg>(m).state);
  }
  return out;
}

}  // namespace

TEST_CASE("okx.md_parser: books snapshot and update carry seqId and prevSeqId") {
  Universe u;
  OkxMdParser p(u.symbols, kOkx);
  Scratch s;
  MdDecodeResult r = decode(p, kSnapshot, s);
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& snap = s.as<BookDeltaMsg>();
  CHECK(snap.hdr.type == EventType::BookSnapshot);
  CHECK(snap.is_snapshot());
  CHECK(snap.hdr.instrument == InstrumentId{0});
  CHECK(snap.last_update_id == 10);
  CHECK(snap.prev_update_id == 0);  // -1
  REQUIRE(snap.bid_count == 2);
  REQUIRE(snap.ask_count == 2);
  CHECK(snap.bids()[0].price == px("8476.97"));
  CHECK(snap.bids()[0].qty == qty("256"));  // contracts
  CHECK(snap.asks()[1].price == px("8477"));
  CHECK(snap.hdr.exch_ts == Timestamp{1597026383085LL * 1'000'000});
  CHECK_FALSE(r.has_checksum);  // 0 since the 2026-06-23 deprecation
  // The level texts, bids first, as sent.
  REQUIRE(p.book_texts().size() == 4);
  CHECK(p.book_bid_count() == 2);
  CHECK(p.book_texts()[0].px == "8476.97");
  CHECK(p.book_texts()[2].px == "8476.98");
  CHECK(p.book_texts()[3].sz == "7");

  r = decode(p, books("update", R"(["8476.98","0","0","0"])", "", 123, 10, 15), s);
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& d = s.as<BookDeltaMsg>();
  CHECK(d.hdr.type == EventType::BookDelta);
  CHECK(d.prev_update_id == 10);
  CHECK(d.last_update_id == 15);
  CHECK(d.bid_count == 0);
  REQUIRE(d.ask_count == 1);
  CHECK(d.asks()[0].qty.is_zero());
  CHECK(p.book_texts()[0].remove);
  CHECK(r.has_checksum);
  CHECK(r.checksum == 123);

  // Malformed and unknown frames.
  CHECK(decode(p, books("update", R"(["x","1","0","1"])", "", 0, 15, 16), s).status ==
        ParseStatus::Malformed);
  CHECK(decode(p, books("sideways", "", "", 0, 15, 16), s).status == ParseStatus::Malformed);
  std::string other = kSnapshot;
  other.replace(other.find("BTC-USDT-SWAP"), 13, "ETH-USDT-SWAP");
  CHECK(decode(p, other, s).status == ParseStatus::UnknownSymbol);
}

TEST_CASE("okx.md_parser: bbo-tbt, trades and control frames") {
  Universe u;
  OkxMdParser p(u.symbols, kOkx);
  Scratch s;
  MdDecodeResult r = decode(
      p,
      R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT-SWAP"},"data":[{"asks":[["8476.98","415","0","13"]],"bids":[["8476.97","256","0","12"]],"ts":"1597026383085","seqId":123456}]})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& t = s.as<BookTickerMsg>();
  CHECK(t.hdr.type == EventType::BookTicker);
  CHECK(t.bid_px == px("8476.97"));
  CHECK(t.bid_qty == qty("256"));
  CHECK(t.ask_px == px("8476.98"));
  CHECK(t.ask_qty == qty("415"));
  CHECK(t.hdr.venue_seq == 123456);

  r = decode(
      p,
      R"({"arg":{"channel":"trades","instId":"BTC-USDT-SWAP"},"data":[{"instId":"BTC-USDT-SWAP","tradeId":"130639474","px":"42219.9","sz":"12","side":"sell","ts":"1630048897897","count":"3","source":"0","seqId":1234},{"instId":"BTC-USDT-SWAP","tradeId":"130639475","px":"42220","sz":"1","side":"buy","ts":"1630048897898","count":"1","source":"0","seqId":1235}]})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 2);
  const auto& tr = s.as<TradeMsg>();
  CHECK(tr.price == px("42219.9"));
  CHECK(tr.qty == qty("12"));
  CHECK(tr.aggressor == Side::Sell);  // the taker's side
  CHECK(tr.trade_id == 130639474);
  const auto& tr2 = *reinterpret_cast<const TradeMsg*>(s.buf + sizeof(TradeMsg));
  CHECK(tr2.aggressor == Side::Buy);

  r = decode(p, "pong", s);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.control == ControlOp::Pong);
  r = decode(
      p,
      R"({"event":"subscribe","arg":{"channel":"books","instId":"BTC-USDT-SWAP"},"connId":"a4d3ae55"})",
      s);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.control == ControlOp::Subscribe);
  CHECK(r.channel == "books");
  r = decode(
      p,
      R"({"event":"error","code":"64003","msg":"Your trading fee tier doesn't meet the requirement to access this channel","connId":"a4d3ae55"})",
      s);
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.control == ControlOp::Error);
  CHECK(r.code == 64003);
  r = decode(p, R"({"event":"login","code":"0","msg":"","connId":"a4d3ae55"})", s);
  CHECK(r.control == ControlOp::Login);
  CHECK(r.control_success);
  r = decode(
      p,
      R"({"event":"notice","code":"64008","msg":"The connection will soon be closed for a service upgrade. Please reconnect.","connId":"a4d3ae55"})",
      s);
  CHECK(r.control == ControlOp::Notice);
  CHECK(r.code == 64008);
  CHECK(decode(p, "{not json", s).status == ParseStatus::Malformed);
}

TEST_CASE("okx.checksum: CRC-32 of the first 25 interleaved levels, as zlib computes it") {
  CHECK(crc32("") == 0U);
  CHECK(crc32("123456789") == 0xCBF43926U);  // the CRC-32/ISO-HDLC check value
  OkxShadowBook b;
  auto lv = [](const char* p, const char* s) {
    return OkxLevelText{
        Price::from_decimal(p).value().raw, p, s, Qty::from_decimal(s).value().is_zero()};
  };
  // One bid, three asks: the longer side's rest follows (legacy docs example).
  REQUIRE(b.apply(true, lv("3366.1", "7")));
  REQUIRE(b.apply(false, lv("3368", "8")));
  REQUIRE(b.apply(false, lv("3366.8", "9")));
  REQUIRE(b.apply(false, lv("3372", "8")));
  CHECK(b.checksum_text() == "3366.1:7:3366.8:9:3368:8:3372:8");
  CHECK(b.checksum() == 831078360);  // zlib.crc32 of that text, as int32
  // Updates in place, deletes and the text as sent ("7.10" stays "7.10").
  REQUIRE(b.apply(true, lv("3366.1", "7.10")));
  REQUIRE(b.apply(false, lv("3368", "0")));
  REQUIRE(b.apply(true, lv("3366.5", "1")));
  CHECK(b.checksum_text() == "3366.5:1:3366.8:9:3366.1:7.10:3372:8");
  // Only 25 levels a side count.
  OkxShadowBook deep;
  for (int i = 0; i < 30; ++i) {
    const std::string p = std::to_string(1000 - i);
    REQUIRE(deep.apply(true, OkxLevelText{Price::from_int(1000 - i).raw, p, "1", false}));
  }
  CHECK(deep.bid_count() == 30);
  const std::string text = deep.checksum_text();
  CHECK(text.find("976:1") != std::string::npos);
  CHECK(text.find("975:1") == std::string::npos);
}

TEST_CASE("okx.book_sync: a seqId gap resyncs and resubscribes, a heartbeat and a reset do not") {
  Universe u;
  RecordingSink sink(8U << 20);
  Resubscribes resub;
  OkxMdFeed feed(u.symbols, kOkx, sink.sink, ResubscribeRequester{&Resubscribes::fn, &resub});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  REQUIRE(feed.subscription_payloads().size() == 1);
  CHECK(
      feed.subscription_payloads()[0] ==
      R"({"id":"md","op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT-SWAP"},{"channel":"bbo-tbt","instId":"BTC-USDT-SWAP"},{"channel":"trades","instId":"BTC-USDT-SWAP"}]})");
  feed.on_connected();
  auto push = [&](const std::string& f) {
    const PaddedJson j(f);
    return feed.on_message(j.view(), 1);
  };
  // An update before the snapshot is not applied.
  REQUIRE(push(books("update", R"(["8477","1","0","1"])", "", 0, 9, 10)) == ParseStatus::Ok);
  CHECK_FALSE(feed.sync(InstrumentId{0})->synced());
  REQUIRE(push(kSnapshot) == ParseStatus::Ok);
  CHECK(feed.sync(InstrumentId{0})->synced());
  // The documented sequence: 10 -> 15, a heartbeat 15 -> 15, a reset 15 -> 3, then 3 -> 5.
  REQUIRE(push(books("update", R"(["8477","8","0","2"])", "", 0, 10, 15)) == ParseStatus::Ok);
  REQUIRE(push(books("update", "", "", 0, 15, 15)) == ParseStatus::Ok);
  REQUIRE(push(books("update", "", R"(["8476","1","0","1"])", 0, 15, 3)) == ParseStatus::Ok);
  REQUIRE(push(books("update", "", R"(["8476","2","0","1"])", 0, 3, 5)) == ParseStatus::Ok);
  CHECK(feed.sync(InstrumentId{0})->synced());
  CHECK(feed.resync_count() == 0);
  CHECK(resub.ids.empty());
  auto before = sink.drain();
  CHECK(states(before).empty());
  // 5 -> 7 misses 6: Resyncing, and a resubscribe for a new snapshot.
  REQUIRE(push(books("update", R"(["8478","1","0","1"])", "", 0, 6, 7)) == ParseStatus::Ok);
  CHECK_FALSE(feed.sync(InstrumentId{0})->synced());
  CHECK(feed.resync_count() == 1);
  REQUIRE(resub.ids.size() == 1);
  CHECK(resub.ids[0] == InstrumentId{0});
  const auto after = sink.drain();
  const auto st = states(after);
  REQUIRE(st.size() == 1);
  CHECK(st[0] == ConnState::Resyncing);
  const auto p = feed.resubscribe_payloads(InstrumentId{0});
  REQUIRE(p.size() == 2);
  CHECK(
      p[0] ==
      R"({"id":"unsub","op":"unsubscribe","args":[{"channel":"books","instId":"BTC-USDT-SWAP"}]})");
  CHECK(p[1] ==
        R"({"id":"resub","op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT-SWAP"}]})");
  // The new snapshot syncs again.
  REQUIRE(push(kSnapshot) == ParseStatus::Ok);
  CHECK(feed.sync(InstrumentId{0})->synced());
}

TEST_CASE("okx.md_feed: a checksum that disagrees resyncs, a zero one is not checked") {
  Universe u;
  RecordingSink sink(8U << 20);
  Resubscribes resub;
  OkxMdFeed feed(u.symbols, kOkx, sink.sink, ResubscribeRequester{&Resubscribes::fn, &resub});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  feed.on_connected();
  auto push = [&](const std::string& f) {
    const PaddedJson j(f);
    return feed.on_message(j.view(), 1);
  };
  // "8476.97:256:8476.98:415:8475.55:101:8477:7" -> 2123921068 (zlib).
  constexpr long long kGood = 2123921068;
  const std::string snap = books("snapshot",
                                 R"(["8476.98","415","0","13"],["8477","7","0","2"])",
                                 R"(["8476.97","256","0","12"],["8475.55","101","0","1"])",
                                 kGood,
                                 -1,
                                 10);
  REQUIRE(push(snap) == ParseStatus::Ok);
  CHECK(feed.sync(InstrumentId{0})->synced());
  CHECK(feed.stats().checksums_checked == 1);
  CHECK(feed.stats().checksum_errors == 0);
  CHECK(feed.shadow(InstrumentId{0})->checksum_text() ==
        "8476.97:256:8476.98:415:8475.55:101:8477:7");
  // Removing 8477 leaves "8476.97:256:8476.98:415:8475.55:101".
  constexpr long long kAfterDelete = 214565906;
  REQUIRE(push(books("update", R"(["8477","0","0","0"])", "", kAfterDelete, 10, 11)) ==
          ParseStatus::Ok);
  CHECK(feed.stats().checksum_errors == 0);
  CHECK(feed.sync(InstrumentId{0})->synced());
  // A wrong one: Resyncing and a resubscribe, as for a gap.
  REQUIRE(push(books("update", R"(["8478","1","0","1"])", "", 12345, 11, 12)) == ParseStatus::Ok);
  CHECK(feed.stats().checksum_errors == 1);
  CHECK_FALSE(feed.sync(InstrumentId{0})->synced());
  CHECK(resub.ids.size() == 1);
  // Checksums of 0 (OKX since 2026-06-23): the chain alone decides.
  REQUIRE(push(kSnapshot) == ParseStatus::Ok);
  const std::uint64_t checked = feed.stats().checksums_checked;
  REQUIRE(push(books("update", R"(["8478","1","0","1"])", "", 0, 10, 11)) == ParseStatus::Ok);
  CHECK(feed.stats().checksums_checked == checked);
  CHECK(feed.sync(InstrumentId{0})->synced());
}

TEST_CASE("okx.md_feed: a recorded production session syncs the book with no resync") {
  // wss://ws.okx.com:8443/ws/v5/public, books + bbo-tbt + trades on BTC-USDT-SWAP, recorded with
  // fastmm-live --record-raw on 2026-09-26 (tests/fixtures/okx/fixtures.meta.json).
  Universe u;
  RecordingSink sink(8U << 20);
  Resubscribes resub;
  OkxMdFeed feed(u.symbols, kOkx, sink.sink, ResubscribeRequester{&Resubscribes::fn, &resub});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  feed.on_connected();
  const std::string raw = fastmm::test::fixture("okx/raw_md_stream.jsonl");
  std::size_t pos = 0;
  std::size_t frames = 0;
  while (pos < raw.size()) {
    std::size_t end = raw.find('\n', pos);
    if (end == std::string::npos) end = raw.size();
    const std::string line = raw.substr(pos, end - pos);
    pos = end + 1;
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    const PaddedJson j(line.substr(tab + 1));
    const ParseStatus st = feed.on_message(j.view(), 1);
    CHECK((st == ParseStatus::Ok || st == ParseStatus::Ignored));
    ++frames;
  }
  CHECK(frames > 100);
  CHECK(feed.sync(InstrumentId{0})->synced());
  CHECK(feed.resync_count() == 0);
  CHECK(resub.ids.empty());
  CHECK(feed.stats().malformed == 0);
  CHECK(feed.parser_stats().book_snapshots == 1);
  CHECK(feed.parser_stats().book_deltas == 24);
  CHECK(feed.parser_stats().trades > 0);
  CHECK(feed.parser_stats().book_tickers > 0);
  CHECK(feed.stats().checksums_checked == 0);  // every checksum is 0
  // The snapshot's 400 levels a side reach the engine.
  bool snapshot_seen = false;
  for (const auto& m : sink.drain()) {
    if (RecordingSink::type_of(m) != EventType::BookSnapshot) continue;
    const auto& s = RecordingSink::as<BookDeltaMsg>(m);
    CHECK(s.bid_count == 400);
    CHECK(s.ask_count == 400);
    CHECK(s.bids()[0].price < s.asks()[0].price);
    snapshot_seen = true;
  }
  CHECK(snapshot_seen);
}
