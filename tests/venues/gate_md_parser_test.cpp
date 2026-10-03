// GateMdParser and GateMdFeed on frames recorded from wss://fx-ws.gateio.ws/v4/ws/usdt
// (tests/fixtures/gate/fixtures.meta.json): the obu snapshot and increments, the book ticker,
// trades with the taker side in the sign of the size, tickers to PerpState, control frames, and
// a recorded session that syncs both books with no resync.
#include "fastmm/venues/gate/gate_md_parser.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/gate/gate_md_feed.hpp"

#include <fstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::gate;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kGate{3};

struct GateUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  InstrumentId nvda;
  InstrumentId btc;
  GateUniverse() {
    Instrument n = make_instrument("NVDA_USDT", 3, "NVDA", "USDT");
    n.asset_class = AssetClass::Perpetual;
    n.lot = Qty::from_int(1);
    Instrument b = make_instrument("BTC_USDT", 3, "BTC", "USDT");
    b.asset_class = AssetClass::Perpetual;
    b.lot = Qty::from_int(1);
    nvda = instruments.add(n).value();
    btc = instruments.add(b).value();
    REQUIRE(symbols.build(instruments));
  }
};

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}

}  // namespace

TEST_CASE("gate.md_parser: recorded obu snapshot and increments") {
  GateUniverse u;
  GateMdParser p(u.symbols, kGate);
  Scratch s;
  const auto snap = padded_fixture("gate/obu_snapshot.json");
  auto r = p.decode(snap.view(), Timestamp{7}, Cycles{9}, s.span());
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::BookSnapshot);
  CHECK(r.count == 1);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.hdr.type == EventType::BookSnapshot);
  CHECK(m.is_snapshot());
  CHECK(m.hdr.instrument == u.nvda);
  CHECK(m.hdr.venue == kGate);
  CHECK(m.bid_count == 50);
  CHECK(m.ask_count == 50);
  CHECK(m.last_update_id == 90220637);
  CHECK(m.first_update_id == 90220637);
  CHECK(m.hdr.exch_ts.ns == 1791009973133LL * 1'000'000);
  CHECK(m.hdr.recv_ts.ns == 7);
  CHECK(m.bids()[0] == Level{px("234.34"), qty("1196")});
  CHECK(m.bids()[1] == Level{px("234.32"), qty("35")});
  CHECK(m.hdr.len == BookDeltaMsg::size_for(50, 50));
  CHECK(r.len == m.hdr.len);

  const auto delta = padded_fixture("gate/obu_delta.json");
  r = p.decode(delta.view(), Timestamp{8}, Cycles{9}, s.span());
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::BookDelta);
  const auto& d = s.as<BookDeltaMsg>();
  CHECK(d.hdr.type == EventType::BookDelta);
  CHECK_FALSE(d.is_snapshot());
  CHECK(d.first_update_id == 90220638);
  CHECK(d.last_update_id == 90220638);
  CHECK(d.bid_count == 1);
  CHECK(d.ask_count == 0);
  CHECK(d.bids()[0] == Level{px("234.1"), qty("258")});

  // An increment without levels still moves the depth id.
  const auto delta2 = padded_fixture("gate/obu_delta2.json");
  r = p.decode(delta2.view(), Timestamp{8}, Cycles{9}, s.span());
  REQUIRE(r.ok());
  const auto& d2 = s.as<BookDeltaMsg>();
  CHECK(d2.first_update_id == 90220639);
  CHECK(d2.last_update_id == 90220640);
  CHECK(d2.bid_count == 0);
  CHECK(d2.ask_count == 0);
  CHECK(p.stats().book_snapshots == 1);
  CHECK(p.stats().book_deltas == 2);
}

TEST_CASE("gate.md_parser: book ticker, trades (taker side from the sign), tickers, control") {
  GateUniverse u;
  GateMdParser p(u.symbols, kGate);
  Scratch s;
  {
    const auto f = padded_fixture("gate/book_ticker.json");
    const auto r = p.decode(f.view(), Timestamp{1}, Cycles{2}, s.span());
    REQUIRE(r.ok());
    CHECK(r.kind == MdKind::BookTicker);
    const auto& t = s.as<BookTickerMsg>();
    CHECK(t.hdr.instrument == u.btc);
    CHECK(t.bid_px == px("84588.1"));
    CHECK(t.bid_qty == qty("5564"));
    CHECK(t.ask_px == px("84588.2"));
    CHECK(t.ask_qty == qty("23621"));
    CHECK(t.hdr.venue_seq == 126906668187ULL);
    CHECK(t.hdr.exch_ts.ns == 1791009973168LL * 1'000'000);
  }
  {
    const auto f = padded_fixture("gate/trades.json");
    const auto r = p.decode(f.view(), Timestamp{1}, Cycles{2}, s.span());
    REQUIRE(r.ok());
    CHECK(r.kind == MdKind::Trade);
    CHECK(r.count == 1);
    const auto& t = s.as<TradeMsg>();
    CHECK(t.hdr.instrument == u.btc);
    CHECK(t.price == px("84588.2"));
    CHECK(t.qty == qty("5"));
    CHECK(t.aggressor == Side::Buy);
    CHECK(t.trade_id == 846107575);
    CHECK(t.hdr.exch_ts.ns == 1791009974834LL * 1'000'000);
  }
  {
    // A seller-initiated trade, size as an integer (no X-Gate-Size-Decimal header), two items.
    const PaddedJson f(
        R"({"time":1790838646,"time_ms":1790838646086,"channel":"futures.trades","event":"update","result":[{"id":39421091,"size":-29,"create_time":1790838646,"create_time_ms":1790838646086,"price":"234.42","contract":"NVDA_USDT"},{"id":39421092,"size":"1.5","create_time":1790838646,"create_time_ms":1790838646090,"price":"234.43","contract":"NVDA_USDT","is_internal":true}]})");
    const auto r = p.decode(f.view(), Timestamp{1}, Cycles{2}, s.span());
    REQUIRE(r.ok());
    CHECK(r.count == 2);
    const auto& t = s.as<TradeMsg>();
    CHECK(t.qty == qty("29"));
    CHECK(t.aggressor == Side::Sell);
    const auto& t2 = *reinterpret_cast<const TradeMsg*>(s.span().data() + sizeof(TradeMsg));
    CHECK(t2.qty == qty("1.5"));
    CHECK(t2.aggressor == Side::Buy);
    CHECK(t2.trade_id == 39421092);
  }
  {
    const auto f = padded_fixture("gate/tickers.json");
    const auto r = p.decode(f.view(), Timestamp{1}, Cycles{2}, s.span());
    REQUIRE(r.ok());
    CHECK(r.kind == MdKind::PerpState);
    const auto& m = s.as<PerpStateMsg>();
    CHECK(m.hdr.instrument == u.btc);
    CHECK((m.fields & PerpStateMsg::kMark) != 0);
    CHECK((m.fields & PerpStateMsg::kIndex) != 0);
    CHECK((m.fields & PerpStateMsg::kFunding) != 0);
    CHECK(m.mark_price == px("84588.1"));
    CHECK(m.index_price == px("84634.7"));
    CHECK(m.funding_rate == doctest::Approx(-0.000011));
    CHECK(m.funding_interval.ns == 28800LL * 1'000'000'000);
    CHECK(m.next_funding.ns == 1791014400LL * 1'000'000'000);
    CHECK(m.hdr.exch_ts.ns == 1791009972577LL * 1'000'000);
  }
  {
    const auto ok = padded_fixture("gate/subscribe_ok.json");
    auto r = p.decode(ok.view(), Timestamp{1}, Cycles{2}, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(r.control == ControlOp::Subscribe);
    CHECK(r.control_success);
    const PaddedJson bad(
        R"({"time":1747391482,"time_ms":1747391482960,"id":1,"conn_id":"d9db9373dc5e081e","channel":"futures.obu","event":"subscribe","payload":["ob.BTC_USDT.400"],"error":{"code":2,"message":"Alert sub ob.BTC_USDT.400"},"result":{"status":"fail"}})");
    r = p.decode(bad.view(), Timestamp{1}, Cycles{2}, s.span());
    CHECK(r.status == ParseStatus::Error);
    CHECK(r.control == ControlOp::Subscribe);
    CHECK_FALSE(r.control_success);
    CHECK(r.error_code == 2);
    CHECK(r.error == "Alert sub ob.BTC_USDT.400");
    const auto pong = padded_fixture("gate/pong.json");
    r = p.decode(pong.view(), Timestamp{1}, Cycles{2}, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(r.control == ControlOp::Pong);
    const PaddedJson api(R"({"request_id":"login-p","header":{"status":"200"},"data":{}})");
    r = p.decode(api.view(), Timestamp{1}, Cycles{2}, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(r.control == ControlOp::Api);
    const PaddedJson unknown(
        R"({"time":1,"time_ms":1000,"channel":"futures.book_ticker","event":"update","result":{"t":1000,"u":1,"s":"NOSUCH_USDT","b":"1","B":"1","a":"2","A":"1"}})");
    r = p.decode(unknown.view(), Timestamp{1}, Cycles{2}, s.span());
    CHECK(r.status == ParseStatus::UnknownSymbol);
    const PaddedJson malformed(
        R"({"time":1,"time_ms":1000,"channel":"futures.book_ticker","event":"update","result":{"t":1000,"u":1,"s":"BTC_USDT","b":"x","B":"1","a":"2","A":"1"}})");
    r = p.decode(malformed.view(), Timestamp{1}, Cycles{2}, s.span());
    CHECK(r.status == ParseStatus::Malformed);
    const PaddedJson empty_side(
        R"({"time":1,"time_ms":1000,"channel":"futures.book_ticker","event":"update","result":{"t":1000,"u":1,"s":"BTC_USDT","b":"","B":0,"a":"2","A":3}})");
    r = p.decode(empty_side.view(), Timestamp{1}, Cycles{2}, s.span());
    REQUIRE(r.ok());
    const auto& t = s.as<BookTickerMsg>();
    CHECK(t.bid_px.is_zero());
    CHECK(t.ask_px == px("2"));
    CHECK(t.ask_qty == qty("3"));
  }
}

TEST_CASE("gate.md_feed: recorded session syncs both books with no resync") {
  GateUniverse u;
  RecordingSink rs(8U << 20);
  int resubscribes = 0;
  GateMdFeed feed(
      u.symbols,
      kGate,
      rs.sink,
      ResubscribeRequester{[](void* ctx, InstrumentId) noexcept { ++*static_cast<int*>(ctx); },
                           &resubscribes});
  REQUIRE(feed.add_instrument(u.nvda));
  REQUIRE(feed.add_instrument(u.btc));
  const auto payloads = feed.subscription_payloads();
  REQUIRE(payloads.size() == 4);
  CHECK(
      payloads[0] ==
      R"({"time":0000000000,"channel":"futures.obu","event":"subscribe","payload":["ob.NVDA_USDT.50","ob.BTC_USDT.50"]})");
  std::string p0(payloads[0]);
  GateMdFeed::fill_time(p0, 1791009973);
  CHECK(p0.starts_with(R"({"time":1791009973,"channel":"futures.obu")"));
  CHECK(
      payloads[1] ==
      R"({"time":0000000000,"channel":"futures.book_ticker","event":"subscribe","payload":["NVDA_USDT","BTC_USDT"]})");
  feed.on_connected();
  std::ifstream in(fastmm::test::fixtures_dir() / "gate/raw_md_stream.jsonl");
  REQUIRE(in.good());
  std::string line;
  std::size_t frames = 0;
  while (std::getline(in, line)) {
    const std::size_t tab = line.find('\t');
    REQUIRE(tab != std::string::npos);
    const PaddedJson frame(line.substr(tab + 1));
    const ParseStatus st = feed.on_message(frame.view(), 1);
    CHECK(st != ParseStatus::Malformed);
    ++frames;
  }
  CHECK(frames > 600);
  CHECK(feed.synced_count() == 2);
  CHECK(feed.resync_count() == 0);
  CHECK(resubscribes == 0);
  CHECK(feed.parser_stats().book_snapshots == 2);
  CHECK(feed.parser_stats().book_deltas > 500);
  CHECK(feed.parser_stats().trades > 10);
  CHECK(feed.parser_stats().perp_states > 4);
  CHECK(feed.stats().pongs == 1);
  CHECK(feed.stats().subscribe_errors == 0);
  const auto msgs = rs.drain();
  std::size_t snapshots = 0;
  std::size_t deltas = 0;
  for (const auto& m : msgs) {
    if (RecordingSink::type_of(m) == EventType::BookSnapshot) ++snapshots;
    if (RecordingSink::type_of(m) == EventType::BookDelta) ++deltas;
  }
  CHECK(snapshots == 2);
  CHECK(deltas == feed.parser_stats().book_deltas);
}

TEST_CASE("gate.md_feed: a gap in the obu ids resubscribes the instrument") {
  GateUniverse u;
  RecordingSink rs(1U << 20);
  int resubscribes = 0;
  GateMdFeed feed(
      u.symbols,
      kGate,
      rs.sink,
      ResubscribeRequester{[](void* ctx, InstrumentId) noexcept { ++*static_cast<int*>(ctx); },
                           &resubscribes});
  REQUIRE(feed.add_instrument(u.nvda));
  feed.on_connected();
  const auto snap = padded_fixture("gate/obu_snapshot.json");
  REQUIRE(feed.on_message(snap.view(), 1) == ParseStatus::Ok);
  const auto delta = padded_fixture("gate/obu_delta.json");
  REQUIRE(feed.on_message(delta.view(), 2) == ParseStatus::Ok);
  CHECK(feed.synced_count() == 1);
  // U jumps past the local id + 1: a gap.
  const PaddedJson gap(
      R"({"time":1,"time_ms":1000,"channel":"futures.obu","event":"update","result":{"t":1000,"s":"ob.NVDA_USDT.50","U":90220645,"u":90220646,"b":[["234.1","1"]]}})");
  REQUIRE(feed.on_message(gap.view(), 3) == ParseStatus::Ok);
  CHECK(feed.synced_count() == 0);
  CHECK(feed.resync_count() == 1);
  CHECK(resubscribes == 1);
  const auto payloads = feed.resubscribe_payloads(u.nvda);
  REQUIRE(payloads.size() == 2);
  CHECK(payloads[0].find(R"("event":"unsubscribe","payload":["ob.NVDA_USDT.50"])") !=
        std::string::npos);
  CHECK(payloads[1].find(R"("event":"subscribe","payload":["ob.NVDA_USDT.50"])") !=
        std::string::npos);
  // A new full snapshot syncs again.
  REQUIRE(feed.on_message(snap.view(), 4) == ParseStatus::Ok);
  CHECK(feed.synced_count() == 1);
}
