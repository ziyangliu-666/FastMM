#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/binance_usdm/binance_usdm_md_feed.hpp"

#include <sstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance_usdm;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {
const Timestamp kRecv{1'700'000'000'000'000'000LL};
const Cycles kT0{7};
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}
std::vector<std::string> lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) {
    if (!l.empty()) out.push_back(l);
  }
  return out;
}
std::uint64_t json_uint(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t p = json.find(needle);
  REQUIRE(p != std::string::npos);
  return std::stoull(json.substr(p + needle.size()));
}
struct Requests {
  int count = 0;
  static void on_request(void* ctx, InstrumentId) noexcept { ++static_cast<Requests*>(ctx)->count; }
};
}  // namespace

TEST_CASE("binance_usdm.md: recorded depthUpdate -> BookDeltaMsg with U, u and pu") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance_usdm/depth_update.json");
  const DecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.kind == MdKind::BookDelta);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.hdr.type == EventType::BookDelta);
  CHECK(m.hdr.len == r.len);
  CHECK(m.hdr.len == BookDeltaMsg::size_for(0, 1));
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.recv_ts == kRecv);
  CHECK(m.hdr.t0_cycles == kT0);
  CHECK(m.hdr.exch_ts.ns == 1789469121836LL * 1'000'000);  // T, the matching-engine time
  CHECK(m.first_update_id == 429012275424ULL);
  CHECK(m.last_update_id == 429012275789ULL);
  CHECK(m.prev_update_id == 429012275360ULL);
  CHECK(m.hdr.venue_seq == 429012275789ULL);
  CHECK(m.bid_count == 0);
  REQUIRE(m.ask_count == 1);
  CHECK(m.asks()[0].price == px("77021.60"));
  CHECK(m.asks()[0].qty == qty("650.0043"));
  CHECK_FALSE(m.is_snapshot());
}

TEST_CASE("binance_usdm.md: recorded bookTicker and aggTrade") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
  {
    const auto fx = padded_fixture("binance_usdm/book_ticker.json");
    const DecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.kind == MdKind::BookTicker);
    const auto& m = s.as<BookTickerMsg>();
    CHECK(m.bid_px == px("76947.30"));
    CHECK(m.bid_qty == qty("0.0374"));
    CHECK(m.ask_px == px("76968.30"));
    CHECK(m.ask_qty == qty("2237.2173"));
    CHECK(m.hdr.venue_seq == 429012276615ULL);
    CHECK(m.hdr.exch_ts.ns == 1789469121938LL * 1'000'000);
  }
  {
    const auto fx = padded_fixture("binance_usdm/agg_trade.json");
    const DecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.kind == MdKind::Trade);
    const auto& m = s.as<TradeMsg>();
    CHECK(m.price == px("76968.30"));
    CHECK(m.qty == qty("1046.7650"));
    CHECK(m.trade_id == 309896910ULL);
    CHECK(m.aggressor == Side::Buy);  // m = false: the buyer took
    CHECK(m.hdr.exch_ts.ns == 1789469121938LL * 1'000'000);
  }
  CHECK(p.stats().book_tickers == 1);
  CHECK(p.stats().trades == 1);
}

TEST_CASE("binance_usdm.md: markPrice, acks, raw payloads, unknown symbol and malformed frames") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const PaddedJson mark(
      R"({"stream":"btcusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":1789467508000,"s":"BTCUSDT","p":"76986.40000000","ap":"76986.40000000","P":"76996.91044964","i":"77014.72500000","r":"0.00010000","T":1789488000000,"st":1}})");
  CHECK(p.decode(mark.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  const PaddedJson ack(R"({"result":null,"id":1})");
  CHECK(p.decode(ack.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  // /ws/<stream> payloads carry no wrapper: the event type decides.
  const PaddedJson raw_ticker(
      R"({"e":"bookTicker","u":428997042613,"s":"BTCUSDT","ps":"BTCUSDT","b":"76986.40","B":"0.0228","a":"76986.50","A":"183.3812","T":1789467546365,"E":1789467546365,"st":1})");
  const DecodeResult rt = p.decode(raw_ticker.view(), kRecv, kT0, s.span());
  REQUIRE(rt.status == ParseStatus::Ok);
  CHECK(rt.kind == MdKind::BookTicker);
  const PaddedJson unknown(
      R"({"stream":"xrpusdt@aggTrade","data":{"e":"aggTrade","E":1,"a":2,"s":"XRPUSDT","p":"0.5","q":"10","nq":"10","f":1,"l":1,"T":1,"m":true,"st":1}})");
  CHECK(p.decode(unknown.view(), kRecv, kT0, s.span()).status == ParseStatus::UnknownSymbol);
  // A depth update without pu is not a futures frame.
  const PaddedJson no_pu(
      R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1,"T":1,"s":"BTCUSDT","U":10,"u":11,"b":[],"a":[]}})");
  CHECK(p.decode(no_pu.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
  const PaddedJson bad_level(
      R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1,"T":1,"s":"BTCUSDT","U":10,"u":11,"pu":9,"b":[["x","1"]],"a":[]}})");
  CHECK(p.decode(bad_level.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
  const PaddedJson truncated(R"({"stream":"btcusdt@bookTicker","data":{"e":"bookT)");
  CHECK(p.decode(truncated.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
}

TEST_CASE("binance_usdm.md: REST depth snapshot -> BookSnapshotMsg") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance_usdm/depth_snapshot.json");
  const DecodeResult r = p.decode_depth_snapshot(fx.view(), InstrumentId{0}, kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.kind == MdKind::BookSnapshot);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.is_snapshot());
  CHECK(m.hdr.type == EventType::BookSnapshot);
  CHECK(m.last_update_id == 429014264524ULL);
  CHECK(m.hdr.exch_ts.ns == 1789469329554LL * 1'000'000);
  REQUIRE(m.bid_count == 5);
  REQUIRE(m.ask_count == 5);
  CHECK(m.bids()[0].price == px("77044.80"));
  CHECK(m.asks()[0].price == px("77045.90"));
  CHECK(m.asks()[0].qty == qty("43.1063"));
  const PaddedJson missing(R"({"bids":[],"asks":[]})");
  CHECK(p.decode_depth_snapshot(missing.view(), InstrumentId{0}, kRecv, kT0, s.span()).status ==
        ParseStatus::Malformed);
}

TEST_CASE("binance_usdm.md_feed: public and market stream targets") {
  TestUniverse u;
  RecordingSink rs;
  Requests req;
  BinanceUsdmMdFeed feed(u.symbols, VenueId{0}, rs.sink, {&Requests::on_request, &req});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  REQUIRE(feed.add_instrument(InstrumentId{1}));
  CHECK_FALSE(feed.add_instrument(InstrumentId{0}));
  CHECK(feed.public_target() ==
        "/public/stream?streams=btcusdt@depth@100ms/btcusdt@bookTicker/ethusdt@depth@100ms/"
        "ethusdt@bookTicker");
  CHECK(feed.market_target() == "/market/stream?streams=btcusdt@aggTrade/ethusdt@aggTrade");
  CHECK(feed.subscription_payloads().empty());
}

TEST_CASE("binance_usdm.md_feed: recorded session syncs the book with no resync") {
  TestUniverse u;
  RecordingSink rs(64U << 20);
  Requests req;
  BinanceUsdmMdFeed feed(u.symbols, VenueId{0}, rs.sink, {&Requests::on_request, &req}, 0);
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  const std::vector<std::string> frames =
      lines(fastmm::test::fixture("binance_usdm/raw_md_stream.jsonl"));
  REQUIRE(frames.size() == 400);
  // The recording has no REST snapshot of its own: use the recorded snapshot's levels with the
  // first delta's U as lastUpdateId, which the first-delta rule (U <= lastUpdateId <= u) accepts.
  std::size_t first_depth = 0;
  while (frames[first_depth].find("depthUpdate") == std::string::npos) ++first_depth;
  const std::uint64_t first_u = json_uint(frames[first_depth], "U");
  std::string snapshot = fastmm::test::fixture("binance_usdm/depth_snapshot.json");
  const std::string key = R"("lastUpdateId":)";
  const std::size_t at = snapshot.find(key) + key.size();
  snapshot.replace(at, snapshot.find(',', at) - at, std::to_string(first_u));

  feed.on_connected();
  CHECK(req.count == 1);
  const std::int64_t now = 1'000'000'000;
  std::size_t i = 0;
  for (; i < 20; ++i) {
    const PaddedJson j(frames[i]);
    CHECK(feed.on_message(j.view(), now) == ParseStatus::Ok);
  }
  const PaddedJson snap(snapshot);
  feed.on_snapshot_body(InstrumentId{0}, snap.view(), now);
  for (; i < frames.size(); ++i) {
    const PaddedJson j(frames[i]);
    CHECK(feed.on_message(j.view(), now) == ParseStatus::Ok);
  }
  UsdmDepthSync* sync = feed.sync(InstrumentId{0});
  REQUIRE(sync != nullptr);
  CHECK(sync->synced());
  CHECK(feed.resync_count() == 0);
  CHECK(req.count == 1);
  CHECK(feed.stats().snapshots_ok == 1);
  CHECK(feed.stats().malformed == 0);
  std::size_t deltas = 0;
  std::size_t tickers = 0;
  for (const auto& m : rs.drain()) {
    deltas += RecordingSink::type_of(m) == EventType::BookDelta ? 1U : 0U;
    tickers += RecordingSink::type_of(m) == EventType::BookTicker ? 1U : 0U;
  }
  CHECK(deltas == 161);  // every recorded depth update was applied
  CHECK(tickers == 400 - 161);
}
