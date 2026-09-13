#include "fastmm/venues/binance/binance_md_parser.hpp"

#include "venue_test_util.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {
const Timestamp kRecv{1'700'000'000'000'000'000LL};
const Cycles kT0{123456};
}  // namespace

TEST_CASE("binance.md: depthUpdate fixture -> BookDeltaMsg with every field") {
  TestUniverse u;
  BinanceMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance/depth_update.json");
  const DecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.kind == MdKind::BookDelta);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.hdr.type == EventType::BookDelta);
  CHECK(m.hdr.len == r.len);
  CHECK(m.hdr.len == BookDeltaMsg::size_for(0, 1));
  CHECK(m.hdr.version == kMessageVersion);
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == VenueId{0});
  CHECK(m.hdr.flags == 0);
  CHECK(m.hdr.recv_ts == kRecv);
  CHECK(m.hdr.t0_cycles == kT0);
  CHECK(m.hdr.exch_ts.ns == 1789295134334LL * 1'000'000);
  CHECK(m.hdr.venue_seq == 1801512);
  CHECK(m.first_update_id == 1801512);
  CHECK(m.last_update_id == 1801512);
  CHECK(m.prev_update_id == 0);
  CHECK(m.bid_count == 0);
  CHECK(m.ask_count == 1);
  CHECK(m.asks()[0].price == Price::from_decimal("76745.19").value());
  CHECK(m.asks()[0].qty == Qty::from_decimal("12.06313").value());
  CHECK_FALSE(m.is_snapshot());
  CHECK(p.stats().book_deltas == 1);
}

TEST_CASE("binance.md: 20/100-level depth updates and qty=0 deletes") {
  TestUniverse u;
  BinanceMdParser p(u.symbols, VenueId{0});
  Scratch s;
  for (const char* name : {"binance/depth_update_20.json", "binance/depth_update_100.json"}) {
    const auto fx = padded_fixture(name);
    const DecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    const auto& m = s.as<BookDeltaMsg>();
    CHECK(m.bid_count + m.ask_count ==
          (std::string(name).find("20") != std::string::npos ? 20U : 100U));
    CHECK(m.hdr.len == BookDeltaMsg::size_for(m.bid_count, m.ask_count));
    // bids descending, asks ascending as in the snapshot they were cut from
    for (std::uint32_t i = 1; i < m.bid_count; ++i)
      CHECK(m.bids()[i].price < m.bids()[i - 1].price);
    for (std::uint32_t i = 1; i < m.ask_count; ++i)
      CHECK(m.asks()[i].price > m.asks()[i - 1].price);
    CHECK(m.last_update_id - m.first_update_id + 1 == m.bid_count + m.ask_count);
  }
  const PaddedJson del(
      R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1,"s":"BTCUSDT","U":10,"u":11,"b":[["76743.86000000","0.00000000"]],"a":[]}})");
  const DecodeResult r = p.decode(del.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.bid_count == 1);
  CHECK(m.bids()[0].qty.is_zero());
  CHECK(m.first_update_id == 10);
  CHECK(m.last_update_id == 11);
}

TEST_CASE("binance.md: bookTicker fixture -> BookTickerMsg") {
  TestUniverse u;
  BinanceMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance/book_ticker.json");
  const DecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.kind == MdKind::BookTicker);
  CHECK(r.len == sizeof(BookTickerMsg));
  const auto& m = s.as<BookTickerMsg>();
  CHECK(m.hdr.type == EventType::BookTicker);
  CHECK(m.hdr.len == sizeof(BookTickerMsg));
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == VenueId{0});
  CHECK(m.hdr.venue_seq == 1801512);
  CHECK(m.hdr.exch_ts.ns == 0);  // spot bookTicker carries no event time
  CHECK(m.bid_px == Price::from_decimal("76745.18").value());
  CHECK(m.bid_qty == Qty::from_decimal("8.74206").value());
  CHECK(m.ask_px == Price::from_decimal("76745.19").value());
  CHECK(m.ask_qty == Qty::from_decimal("12.06313").value());
}

TEST_CASE("binance.md: trade fixture -> TradeMsg with aggressor from m") {
  TestUniverse u;
  BinanceMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance/trade.json");
  const DecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.kind == MdKind::Trade);
  const auto& m = s.as<TradeMsg>();
  CHECK(m.hdr.type == EventType::Trade);
  CHECK(m.hdr.len == sizeof(TradeMsg));
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.exch_ts.ns == 1789295134225LL * 1'000'000);  // T (trade time), not E
  CHECK(m.hdr.venue_seq == 388510);
  CHECK(m.trade_id == 388510);
  CHECK(m.price == Price::from_decimal("76745.19").value());
  CHECK(m.qty == Qty::from_decimal("0.00065").value());
  CHECK(m.aggressor == Side::Buy);  // m=false: buyer is taker
  const PaddedJson maker(
      R"({"stream":"btcusdt@trade","data":{"e":"trade","E":1,"s":"BTCUSDT","t":2,"p":"1","q":"1","T":1,"m":true,"M":true}})");
  REQUIRE(p.decode(maker.view(), kRecv, kT0, s.span()).status == ParseStatus::Ok);
  CHECK(s.as<TradeMsg>().aggressor == Side::Sell);
}

TEST_CASE("binance.md: raw (non-combined) payloads are recognised by their fields") {
  TestUniverse u;
  BinanceMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const PaddedJson depth(
      R"({"e":"depthUpdate","E":1,"s":"ETHUSDT","U":5,"u":6,"b":[["1.5","2"]],"a":[]})");
  REQUIRE(p.decode(depth.view(), kRecv, kT0, s.span()).kind == MdKind::BookDelta);
  CHECK(s.as<BookDeltaMsg>().hdr.instrument == InstrumentId{1});
  const PaddedJson tick(R"({"u":9,"s":"ETHUSDT","b":"1","B":"2","a":"3","A":"4"})");
  REQUIRE(p.decode(tick.view(), kRecv, kT0, s.span()).kind == MdKind::BookTicker);
  CHECK(s.as<BookTickerMsg>().ask_qty == Qty::from_int(4));
  const PaddedJson trade(
      R"({"e":"trade","E":1,"s":"ETHUSDT","t":3,"p":"1","q":"1","T":1,"m":false,"M":true})");
  REQUIRE(p.decode(trade.view(), kRecv, kT0, s.span()).kind == MdKind::Trade);
}

TEST_CASE("binance.md: malformed, unknown symbol, truncated and ignored frames") {
  TestUniverse u;
  BinanceMdParser p(u.symbols, VenueId{0});
  Scratch s;
  CHECK(p.decode(padded_fixture("binance/depth_update_malformed.json").view(), kRecv, kT0, s.span())
            .status == ParseStatus::Malformed);
  CHECK(p.decode(padded_fixture("binance/book_ticker_malformed.json").view(), kRecv, kT0, s.span())
            .status == ParseStatus::Malformed);
  CHECK(p.decode(padded_fixture("binance/trade_unknown_symbol.json").view(), kRecv, kT0, s.span())
            .status == ParseStatus::UnknownSymbol);
  CHECK(p.decode(padded_fixture("binance/truncated.json").view(), kRecv, kT0, s.span()).status ==
        ParseStatus::Malformed);
  const PaddedJson sub_ack(R"({"result":null,"id":1})");
  CHECK(p.decode(sub_ack.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  const PaddedJson kline(R"({"stream":"btcusdt@kline_1m","data":{"e":"kline"}})");
  CHECK(p.decode(kline.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  const PaddedJson not_json("hello");
  CHECK(p.decode(not_json.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
  const PaddedJson bad_price(
      R"({"stream":"btcusdt@trade","data":{"e":"trade","E":1,"s":"BTCUSDT","t":3,"p":"1e3","q":"1","T":1,"m":false}})");
  CHECK(p.decode(bad_price.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
  std::byte tiny[64];
  CHECK(p.decode(padded_fixture("binance/trade.json").view(), kRecv, kT0, tiny).status ==
        ParseStatus::Overflow);
  CHECK(p.stats().malformed == 5);
  CHECK(p.stats().unknown_symbol == 1);
  CHECK(p.stats().ignored == 2);
}

TEST_CASE("binance.md: REST depth snapshot -> BookSnapshotMsg") {
  TestUniverse u;
  BinanceMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance/depth_snapshot.json");
  const DecodeResult r = p.decode_depth_snapshot(fx.view(), InstrumentId{0}, kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.kind == MdKind::BookSnapshot);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.hdr.type == EventType::BookSnapshot);
  CHECK(m.is_snapshot());
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.last_update_id == 1801494);
  CHECK(m.first_update_id == 1801494);
  CHECK(m.bid_count == 100);
  CHECK(m.ask_count == 100);
  CHECK(m.bids()[0].price == Price::from_decimal("76745.18").value());
  CHECK(m.bids()[0].qty == Qty::from_decimal("8.7516").value());
  CHECK(m.hdr.len == BookDeltaMsg::size_for(100, 100));
  const PaddedJson bad(R"({"lastUpdateId":"x","bids":[],"asks":[]})");
  CHECK(p.decode_depth_snapshot(bad.view(), InstrumentId{0}, kRecv, kT0, s.span()).status ==
        ParseStatus::Malformed);
  // More than kMaxBookLevelsPerMsg levels on a side: reported as overflow, never truncated.
  std::string huge = R"({"lastUpdateId":1,"bids":[)";
  for (int i = 0; i < 1100; ++i) huge += std::string(i ? "," : "") + "[\"1\",\"1\"]";
  huge += R"(],"asks":[]})";
  const PaddedJson hp(huge);
  CHECK(p.decode_depth_snapshot(hp.view(), InstrumentId{0}, kRecv, kT0, s.span()).status ==
        ParseStatus::Overflow);
}
