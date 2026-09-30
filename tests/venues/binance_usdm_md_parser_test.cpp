#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/binance_usdm/binance_usdm_md_feed.hpp"
#include "fastmm/venues/level_spill.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance_usdm;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {

Duration hours(std::int64_t h) {
  return seconds(h * 3600);
}
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

TEST_CASE("binance_usdm.md: acks, raw payloads, unknown symbol and malformed frames") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
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

// Production, 2026-09-30T08:31:54Z (fastmm-live --dry-run --record-raw on
// wss://fstream.binance.com/market/stream?streams=...btcusdt@markPrice@1s/ethusdt@markPrice@1s).
TEST_CASE("binance_usdm.md: recorded markPriceUpdate -> PerpStateMsg") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
  CHECK(p.funding_interval(InstrumentId{0}) == hours(8));  // before fundingInfo
  const auto fx = padded_fixture("binance_usdm/mark_price.json");
  const DecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.kind == MdKind::PerpState);
  CHECK(r.len == sizeof(PerpStateMsg));
  const auto& m = s.as<PerpStateMsg>();
  CHECK(m.hdr.type == EventType::PerpState);
  CHECK(m.hdr.len == sizeof(PerpStateMsg));
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == VenueId{0});
  CHECK(m.hdr.recv_ts == kRecv);
  CHECK(m.hdr.t0_cycles == kT0);
  CHECK(m.hdr.exch_ts.ns == 1790757114000LL * 1'000'000);  // E
  CHECK(m.fields == (PerpStateMsg::kMark | PerpStateMsg::kIndex | PerpStateMsg::kFunding));
  CHECK(m.mark_price == px("83018.14569565"));   // p, not ap
  CHECK(m.index_price == px("83062.53108696"));  // i, not P
  CHECK(m.funding_rate == doctest::Approx(-0.00001012).epsilon(1e-12));
  CHECK(m.funding_interval == hours(8));
  CHECK(m.next_funding.ns == 1790784000000LL * 1'000'000);  // T, 16:00 UTC
  CHECK(m.open_interest == Qty{});
  CHECK(p.stats().perp_states == 1);

  // The interval the connector set (GET /fapi/v1/fundingInfo) goes with the rate.
  p.set_funding_interval(InstrumentId{0}, hours(4));
  REQUIRE(p.decode(fx.view(), kRecv, kT0, s.span()).ok());
  CHECK(s.as<PerpStateMsg>().funding_interval == hours(4));
  p.set_funding_interval(InstrumentId{0}, Duration{});  // ignored
  CHECK(p.funding_interval(InstrumentId{0}) == hours(4));
}

TEST_CASE("binance_usdm.md: recorded markPrice stream, both symbols and a rate update") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
  const std::vector<std::string> frames =
      lines(fastmm::test::fixture("binance_usdm/mark_price_stream.jsonl"));
  REQUIRE(frames.size() == 20);
  std::vector<double> btc_rates;
  std::int64_t last_ms = 0;
  for (const std::string& f : frames) {
    const PaddedJson j(f);
    REQUIRE(p.decode(j.view(), kRecv, kT0, s.span()).ok());
    const auto& m = s.as<PerpStateMsg>();
    CHECK(m.hdr.exch_ts.ns / 1'000'000 >= last_ms);
    last_ms = m.hdr.exch_ts.ns / 1'000'000;
    CHECK(m.next_funding.ns == 1790784000000LL * 1'000'000);
    if (m.hdr.instrument == InstrumentId{0}) btc_rates.push_back(m.funding_rate);
  }
  CHECK(p.stats().perp_states == 20);
  CHECK(p.stats().funding_rollovers == 0);
  REQUIRE(btc_rates.size() == 10);
  // The predicted rate moved once, at 08:33:01 UTC.
  CHECK(btc_rates.front() == doctest::Approx(-0.00001012).epsilon(1e-12));
  CHECK(btc_rates.back() == doctest::Approx(-0.00001067).epsilon(1e-12));
}

// Production, 2026-09-30 around the 08:00 UTC funding (Python websockets client on
// wss://fstream.binance.com/market/stream): BTCUSDT and ETHUSDT (8 h) and LPTUSDT (4 h in
// GET /fapi/v1/fundingInfo). The frame at E = 08:00:00.000 still names T = 08:00; the next one
// names the next funding.
TEST_CASE("binance_usdm.md: the next funding time rolling over corrects the interval") {
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCUSDT", 0, "BTC", "USDT")));  // id 0
  REQUIRE(instruments.add(make_instrument("ETHUSDT", 0, "ETH", "USDT")));  // id 1
  REQUIRE(instruments.add(make_instrument("LPTUSDT", 0, "LPT", "USDT")));  // id 2
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  BinanceUsdmMdParser p(symbols, VenueId{0});
  Scratch s;
  const std::vector<std::string> frames =
      lines(fastmm::test::fixture("binance_usdm/mark_price_rollover.jsonl"));
  REQUIRE(frames.size() == 12);
  const std::int64_t funding_ms = 1790755200000LL;  // 2026-09-30T08:00:00Z
  std::vector<PerpStateMsg> lpt;
  for (const std::string& f : frames) {
    const PaddedJson j(f);
    REQUIRE(p.decode(j.view(), kRecv, kT0, s.span()).ok());
    if (s.as<PerpStateMsg>().hdr.instrument == InstrumentId{2}) lpt.push_back(s.as<PerpStateMsg>());
  }
  REQUIRE(lpt.size() == 4);
  // Until the rollover LPTUSDT reports the 8 h default (as if fundingInfo had failed).
  CHECK(lpt[1].hdr.exch_ts.ns == funding_ms * 1'000'000);
  CHECK(lpt[1].next_funding.ns == funding_ms * 1'000'000);
  CHECK(lpt[1].funding_interval == hours(8));
  // 08:00:01: T moved to 12:00, the interval in force is 4 h.
  CHECK(lpt[2].next_funding.ns == (funding_ms + 4 * 3'600'000LL) * 1'000'000);
  CHECK(lpt[2].funding_interval == hours(4));
  CHECK(lpt[3].funding_interval == hours(4));
  CHECK(p.funding_interval(InstrumentId{2}) == hours(4));
  // BTCUSDT and ETHUSDT stepped by their 8 h: no change.
  CHECK(p.funding_interval(InstrumentId{0}) == hours(8));
  CHECK(p.funding_interval(InstrumentId{1}) == hours(8));
  CHECK(p.stats().funding_rollovers == 1);

  // A T that moves after a gap longer than the rollover window proves nothing: fundings may have
  // been missed (16 h here is two 8 h periods).
  BinanceUsdmMdParser q(symbols, VenueId{0});
  auto frame = [](std::int64_t e, std::int64_t t) {
    return R"({"stream":"btcusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":)" +
           std::to_string(e) +
           R"(,"s":"BTCUSDT","p":"83018.1","ap":"83018.1","P":"83205.5","i":"83062.5","r":"0.0001","T":)" +
           std::to_string(t) + R"(,"st":1}})";
  };
  const PaddedJson before(frame(funding_ms - 1000, funding_ms));
  REQUIRE(q.decode(before.view(), kRecv, kT0, s.span()).ok());
  const PaddedJson late(frame(funding_ms + 3'600'000, funding_ms + 16 * 3'600'000LL));
  REQUIRE(q.decode(late.view(), kRecv, kT0, s.span()).ok());
  CHECK(s.as<PerpStateMsg>().funding_interval == hours(8));
  CHECK(q.stats().funding_rollovers == 0);
  // A step that is not a whole number of hours is not an interval either.
  const PaddedJson odd(
      frame(funding_ms + 16 * 3'600'000LL + 1000, funding_ms + 17 * 3'600'000LL + 1));
  REQUIRE(q.decode(odd.view(), kRecv, kT0, s.span()).ok());
  CHECK(q.funding_interval(InstrumentId{0}) == hours(8));
}

TEST_CASE("binance_usdm.md: markPrice stream names, no funding, and malformed mark frames") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
  // The 3 s stream and a raw /ws payload decode alike.
  const PaddedJson slow(
      R"({"stream":"ethusdt@markPrice","data":{"e":"markPriceUpdate","E":1790757114000,"s":"ETHUSDT","p":"2664.40","ap":"2664.40","P":"2665.00","i":"2665.10","r":"0.00004780","T":1790784000000,"st":1}})");
  REQUIRE(p.decode(slow.view(), kRecv, kT0, s.span()).ok());
  CHECK(s.as<PerpStateMsg>().hdr.instrument == InstrumentId{1});
  const PaddedJson raw(
      R"({"e":"markPriceUpdate","E":1790757114000,"s":"ETHUSDT","p":"2664.40","ap":"2664.40","P":"2665.00","i":"2665.10","r":"0.00004780","T":1790784000000,"st":1})");
  const DecodeResult rr = p.decode(raw.view(), kRecv, kT0, s.span());
  REQUIRE(rr.ok());
  CHECK(rr.kind == MdKind::PerpState);
  // A contract without funding: r "" and T 0 -> mark and index only.
  const PaddedJson no_funding(
      R"({"stream":"btcusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":1790757114000,"s":"BTCUSDT","p":"83100.0","ap":"83100.0","P":"83101.0","i":"83062.5","r":"","T":0,"st":1}})");
  REQUIRE(p.decode(no_funding.view(), kRecv, kT0, s.span()).ok());
  CHECK(s.as<PerpStateMsg>().fields == (PerpStateMsg::kMark | PerpStateMsg::kIndex));
  CHECK(s.as<PerpStateMsg>().next_funding == Timestamp{});
  const PaddedJson bad_rate(
      R"({"stream":"btcusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":1,"s":"BTCUSDT","p":"83100.0","ap":"83100.0","P":"1","i":"83062.5","r":"0.0001x","T":1,"st":1}})");
  CHECK(p.decode(bad_rate.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
  const PaddedJson no_index(
      R"({"stream":"btcusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":1,"s":"BTCUSDT","p":"83100.0","r":"0.0001","T":1,"st":1}})");
  CHECK(p.decode(no_index.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
  const PaddedJson unknown(
      R"({"stream":"xrpusdt@markPrice@1s","data":{"e":"markPriceUpdate","E":1,"s":"XRPUSDT","p":"0.5","ap":"0.5","P":"0.5","i":"0.5","r":"0.0001","T":1,"st":1}})");
  CHECK(p.decode(unknown.view(), kRecv, kT0, s.span()).status == ParseStatus::UnknownSymbol);
  CHECK(p.stats().perp_states == 3);
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
  // Mark price streams for the perpetuals only, on the market connection.
  CHECK_FALSE(feed.add_perpetual(InstrumentId{2}, hours(8)));  // not added
  REQUIRE(feed.add_perpetual(InstrumentId{1}, hours(4)));
  REQUIRE(feed.add_perpetual(InstrumentId{1}, hours(4)));  // once
  CHECK(feed.market_target() ==
        "/market/stream?streams=btcusdt@aggTrade/ethusdt@aggTrade/ethusdt@markPrice@1s");
  CHECK(feed.public_target().find("markPrice") == std::string::npos);
  CHECK(feed.funding_interval(InstrumentId{1}) == hours(4));
  CHECK(feed.funding_interval(InstrumentId{0}) == hours(8));
}

TEST_CASE("binance_usdm.md_feed: recorded mark prices reach the market-data sink") {
  TestUniverse u;
  RecordingSink rs;
  Requests req;
  BinanceUsdmMdFeed feed(u.symbols, VenueId{0}, rs.sink, {&Requests::on_request, &req});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  REQUIRE(feed.add_instrument(InstrumentId{1}));
  REQUIRE(feed.add_perpetual(InstrumentId{0}, hours(8)));
  REQUIRE(feed.add_perpetual(InstrumentId{1}, hours(8)));
  const std::vector<std::string> frames =
      lines(fastmm::test::fixture("binance_usdm/mark_price_stream.jsonl"));
  for (const std::string& f : frames) {
    const PaddedJson j(f);
    CHECK(feed.on_message(j.view(), 1) == ParseStatus::Ok);
  }
  CHECK(feed.stats().pushed == frames.size());
  CHECK(req.count == 0);  // mark prices do not touch the book syncs
  std::size_t btc = 0;
  std::size_t eth = 0;
  for (const auto& m : rs.drain()) {
    REQUIRE(RecordingSink::type_of(m) == EventType::PerpState);
    const auto& ps = RecordingSink::as<PerpStateMsg>(m);
    CHECK(ps.hdr.t1_delta > 0);
    btc += ps.hdr.instrument == InstrumentId{0} ? 1U : 0U;
    eth += ps.hdr.instrument == InstrumentId{1} ? 1U : 0U;
  }
  CHECK(btc == 10);
  CHECK(eth == 10);
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

// Production, 2026-09-28: one 100 ms update of BTCUSDT changed 1035 bids and 1224 asks, more than
// a BookDeltaMsg carries (1024 a side). The parser refused it, the next update's pu did not chain
// and the book resynced: 14 resyncs in 15 minutes while the raw stream had no gap. The update now
// keeps the 1024 levels a side nearest the touch and the book stays synced.
TEST_CASE(
    "binance_usdm.md_feed: a depth update longer than a message keeps the levels nearest "
    "the touch") {
  TestUniverse u;
  RecordingSink rs(16U << 20);
  Requests req;
  BinanceUsdmMdFeed feed(u.symbols, VenueId{0}, rs.sink, {&Requests::on_request, &req}, 0);
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  const std::vector<std::string> frames =
      lines(fastmm::test::fixture("binance_usdm/depth_burst.jsonl"));
  REQUIRE(frames.size() == 3);
  // A snapshot inside the first frame's range, with the recorded snapshot's levels.
  std::string snapshot = fastmm::test::fixture("binance_usdm/depth_snapshot.json");
  const std::string key = R"("lastUpdateId":)";
  const std::size_t at = snapshot.find(key) + key.size();
  snapshot.replace(at, snapshot.find(',', at) - at, std::to_string(json_uint(frames[0], "U")));
  feed.on_connected();
  const PaddedJson snap(snapshot);
  feed.on_snapshot_body(InstrumentId{0}, snap.view(), 1'000'000'000);
  for (const std::string& f : frames) {
    const PaddedJson j(f);
    CHECK(feed.on_message(j.view(), 1'000'000'000) == ParseStatus::Ok);
  }
  UsdmDepthSync* sync = feed.sync(InstrumentId{0});
  REQUIRE(sync != nullptr);
  CHECK(sync->synced());
  CHECK(feed.resync_count() == 0);
  CHECK(req.count == 1);
  CHECK(feed.parser_stats().truncated == 1);
  CHECK(feed.stats().dropped == 0);

  std::vector<std::vector<std::byte>> deltas;
  for (auto& m : rs.drain()) {
    if (RecordingSink::type_of(m) == EventType::BookDelta) deltas.push_back(std::move(m));
  }
  REQUIRE(deltas.size() == 3);
  const auto& big = RecordingSink::as<BookDeltaMsg>(deltas[1]);
  REQUIRE(big.bid_count == kMaxBookLevelsPerMsg);
  REQUIRE(big.ask_count == kMaxBookLevelsPerMsg);
  CHECK(big.hdr.flags == truncation_flags(true, true));
  CHECK(RecordingSink::as<BookDeltaMsg>(deltas[0]).hdr.flags == 0);
  CHECK(big.last_update_id == 11673992035278ULL);
  // Wire order (ascending) is kept: the far bids 33999.90 .. 34209.40 and asks above 85346.30 go.
  CHECK(big.bids()[0].price == px("34209.50"));
  CHECK(big.bids()[kMaxBookLevelsPerMsg - 1].price == px("84517.20"));
  CHECK(big.asks()[0].price == px("84501.40"));
  Price worst_ask = big.asks()[0].price;
  for (std::uint32_t i = 0; i < big.ask_count; ++i)
    worst_ask = std::max(worst_ask, big.asks()[i].price);
  CHECK(worst_ask == px("85346.30"));
}

// A side longer than LevelSpill::kCapacity is still refused: counted as dropped, and the gap it
// leaves resyncs the book.
TEST_CASE("binance_usdm.md: a side longer than the spill is an overflow") {
  TestUniverse u;
  BinanceUsdmMdParser p(u.symbols, VenueId{0});
  Scratch s;
  std::string huge = R"({"lastUpdateId":1,"T":1,"bids":[)";
  for (std::uint32_t i = 0; i <= LevelSpill::kCapacity; ++i)
    huge += std::string(i != 0 ? "," : "") + "[\"" + std::to_string(i + 1) + "\",\"1\"]";
  huge += R"(],"asks":[]})";
  const PaddedJson hp(huge);
  CHECK(p.decode_depth_snapshot(hp.view(), InstrumentId{0}, kRecv, kT0, s.span()).status ==
        ParseStatus::Overflow);
  CHECK(p.stats().overflow == 1);
}
