// Bybit tickers -> PerpStateMsg on the frames recorded from production
// (wss://stream.bybit.com/v5/public/linear, 2026-09-30, fixtures.meta.json): the snapshot's
// fields, deltas that keep the fields they omit, the reconnect that clears the cache, the
// subscription and the feed on the recorded stream.
#include "venue_test_util.hpp"

#include "fastmm/venues/bybit/bybit_md_feed.hpp"
#include "fastmm/venues/bybit/bybit_md_parser.hpp"
#include "fastmm/venues/bybit/bybit_rest_decoder.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {

constexpr VenueId kBybit{1};
const InstrumentId kBtc{2};  // TestUniverse: BTCUSDT on venue 1
constexpr std::uint8_t kAll = PerpStateMsg::kMark | PerpStateMsg::kIndex | PerpStateMsg::kFunding |
                              PerpStateMsg::kOpenInterest;
constexpr Duration kEightHours{8LL * 3600 * 1'000'000'000};

// Test literals; a bad one reads as zero and fails its CHECK.
Price px(const char* s) {
  return Price::from_decimal(s).value_or(Price{});
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value_or(Qty{});
}
bool near(double a, double b) {
  return std::fabs(a - b) < 1e-12;
}

// BTCUSDT (id 0) and ETHUSDT (id 1), linear perpetuals on venue 1.
struct PerpUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  PerpUniverse() {
    Instrument btc = make_instrument("BTCUSDT", 1, "BTC", "USDT");
    btc.asset_class = AssetClass::Perpetual;
    Instrument eth = make_instrument("ETHUSDT", 1, "ETH", "USDT");
    eth.asset_class = AssetClass::Perpetual;
    REQUIRE(instruments.add(btc));
    REQUIRE(instruments.add(eth));
    REQUIRE(symbols.build(instruments));
  }
};

// The recorded frames of raw_tickers_stream.jsonl (`<rx_ns>\t<json>` per line).
std::vector<PaddedJson> recorded_frames() {
  const std::string raw = fastmm::test::fixture("bybit/raw_tickers_stream.jsonl");
  std::vector<PaddedJson> out;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t end = raw.find('\n', pos);
    if (end == std::string::npos) end = raw.size();
    const std::string_view line = std::string_view(raw).substr(pos, end - pos);
    pos = end + 1;
    const std::size_t tab = line.find('\t');
    if (tab != std::string_view::npos) out.emplace_back(line.substr(tab + 1));
  }
  return out;
}

}  // namespace

TEST_CASE("bybit.perp: the recorded tickers snapshot becomes a PerpState with every field") {
  TestUniverse u;
  BybitMdParser p(u.symbols, kBybit);
  Scratch s;
  const auto snap = padded_fixture("bybit/tickers_snapshot.json");
  const auto r = p.decode(snap.view(), Timestamp{7}, Cycles{9}, s.span());
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::PerpState);
  CHECK(r.count == 1);
  CHECK(r.len == sizeof(PerpStateMsg));
  const auto& m = s.as<PerpStateMsg>();
  CHECK(m.hdr.type == EventType::PerpState);
  CHECK(m.hdr.len == sizeof(PerpStateMsg));
  CHECK(m.hdr.instrument == kBtc);
  CHECK(m.hdr.venue == kBybit);
  CHECK(m.hdr.exch_ts.ns == 1790757082283LL * 1'000'000);  // ts
  CHECK(m.hdr.venue_seq == 818232112541ULL);               // cs
  CHECK(m.hdr.recv_ts.ns == 7);
  CHECK(m.fields == kAll);
  CHECK(m.mark_price == px("83024.70"));
  CHECK(m.index_price == px("83074.01"));
  CHECK(near(m.funding_rate, -0.00007774));
  CHECK(m.funding_interval == kEightHours);  // fundingIntervalHour "8"
  CHECK(m.next_funding.ns == 1790784000000LL * 1'000'000);
  CHECK(m.open_interest == qty("28559.256"));  // singleOpenInterest: one side
  CHECK(p.stats().perp_states == 1);
}

TEST_CASE("bybit.perp: a delta changes the fields it carries and keeps the others") {
  TestUniverse u;
  BybitMdParser p(u.symbols, kBybit);
  Scratch s;
  const auto snap = padded_fixture("bybit/tickers_snapshot.json");
  REQUIRE(p.decode(snap.view(), Timestamp{1}, Cycles{1}, s.span()).ok());

  // indexPrice only (plus fields that are not ours: bid1Price, turnover24h, ...).
  const auto index = padded_fixture("bybit/tickers_delta_index.json");
  auto r = p.decode(index.view(), Timestamp{2}, Cycles{1}, s.span());
  REQUIRE(r.ok());
  REQUIRE(r.kind == MdKind::PerpState);
  const auto& m = s.as<PerpStateMsg>();
  CHECK(m.fields == kAll);
  CHECK(m.index_price == px("83073.68"));
  CHECK(m.mark_price == px("83024.70"));  // unchanged, reported as current
  CHECK(near(m.funding_rate, -0.00007774));
  CHECK(m.funding_interval == kEightHours);
  CHECK(m.next_funding.ns == 1790784000000LL * 1'000'000);
  CHECK(m.open_interest == qty("28559.256"));
  CHECK(m.hdr.exch_ts.ns == 1790757082784LL * 1'000'000);
  CHECK(m.hdr.venue_seq == 818232113284ULL);

  // markPrice only: the index from the previous delta stays.
  const auto mark = padded_fixture("bybit/tickers_delta_mark.json");
  REQUIRE(p.decode(mark.view(), Timestamp{3}, Cycles{1}, s.span()).ok());
  CHECK(m.fields == kAll);
  CHECK(m.mark_price == px("83024.60"));
  CHECK(m.index_price == px("83073.68"));
  CHECK(near(m.funding_rate, -0.00007774));

  // The minute's funding-rate update.
  const auto funding = padded_fixture("bybit/tickers_delta_funding.json");
  REQUIRE(p.decode(funding.view(), Timestamp{4}, Cycles{1}, s.span()).ok());
  CHECK(m.fields == kAll);
  CHECK(near(m.funding_rate, -0.00008517));
  CHECK(m.mark_price == px("83000.93"));
  CHECK(m.index_price == px("83073.68"));
  CHECK(m.funding_interval == kEightHours);
  CHECK(m.next_funding.ns == 1790784000000LL * 1'000'000);
  CHECK(p.stats().perp_states == 4);
}

TEST_CASE("bybit.perp: a reconnect clears the cache until the next snapshot") {
  TestUniverse u;
  BybitMdParser p(u.symbols, kBybit);
  Scratch s;
  const auto snap = padded_fixture("bybit/tickers_snapshot.json");
  const auto index = padded_fixture("bybit/tickers_delta_index.json");
  // A delta before any snapshot has nothing to complete it.
  CHECK(p.decode(index.view(), Timestamp{1}, Cycles{1}, s.span()).status == ParseStatus::Ignored);
  REQUIRE(p.decode(snap.view(), Timestamp{2}, Cycles{1}, s.span()).ok());
  REQUIRE(p.decode(index.view(), Timestamp{3}, Cycles{1}, s.span()).ok());

  p.reset_tickers();  // what the feed does on (re)connect
  const auto mark = padded_fixture("bybit/tickers_delta_mark.json");
  auto r = p.decode(mark.view(), Timestamp{4}, Cycles{1}, s.span());
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.kind == MdKind::None);

  // The next snapshot starts over: the index from before the reconnect is gone.
  REQUIRE(p.decode(snap.view(), Timestamp{5}, Cycles{1}, s.span()).ok());
  const auto& m = s.as<PerpStateMsg>();
  CHECK(m.index_price == px("83074.01"));
  REQUIRE(p.decode(mark.view(), Timestamp{6}, Cycles{1}, s.span()).ok());
  CHECK(m.mark_price == px("83024.60"));
  CHECK(m.index_price == px("83074.01"));
  CHECK(p.stats().perp_states == 4);
}

TEST_CASE("bybit.perp: a snapshot replaces the cache and an empty field is unknown") {
  TestUniverse u;
  BybitMdParser p(u.symbols, kBybit);
  Scratch s;
  const auto snap = padded_fixture("bybit/tickers_snapshot.json");
  REQUIRE(p.decode(snap.view(), Timestamp{1}, Cycles{1}, s.span()).ok());
  // A resubscribe's snapshot without funding and open interest (a dated future's shape:
  // "fundingRate":"", "nextFundingTime":""): the old funding does not survive it.
  const PaddedJson resub(
      R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"symbol":"BTCUSDT","markPrice":"83100.00","indexPrice":"83101.00","nextFundingTime":"","fundingRate":""},"cs":1,"ts":1790757200000})");
  REQUIRE(p.decode(resub.view(), Timestamp{2}, Cycles{1}, s.span()).ok());
  const auto& m = s.as<PerpStateMsg>();
  CHECK(m.fields == (PerpStateMsg::kMark | PerpStateMsg::kIndex));
  CHECK(m.mark_price == px("83100.00"));
  // A delta that empties the mark leaves the index.
  const PaddedJson empty(
      R"({"topic":"tickers.BTCUSDT","type":"delta","data":{"symbol":"BTCUSDT","markPrice":""},"cs":2,"ts":1790757200100})");
  REQUIRE(p.decode(empty.view(), Timestamp{3}, Cycles{1}, s.span()).ok());
  CHECK(m.fields == PerpStateMsg::kIndex);
  // A value that is not a number is a malformed frame; the cache is untouched.
  const PaddedJson bad(
      R"({"topic":"tickers.BTCUSDT","type":"delta","data":{"symbol":"BTCUSDT","fundingRate":"0.0001x"},"cs":3,"ts":1790757200200})");
  CHECK(p.decode(bad.view(), Timestamp{4}, Cycles{1}, s.span()).status == ParseStatus::Malformed);
  const PaddedJson unknown(
      R"({"topic":"tickers.XRPUSDT","type":"snapshot","data":{"symbol":"XRPUSDT","markPrice":"1"},"cs":1,"ts":1})");
  CHECK(p.decode(unknown.view(), Timestamp{5}, Cycles{1}, s.span()).status ==
        ParseStatus::UnknownSymbol);
}

TEST_CASE("bybit.perp: without fundingIntervalHour the instruments-info interval is used") {
  std::vector<InstrumentInfo> infos;
  REQUIRE(decode_instruments(fastmm::test::fixture("bybit/linear_instruments_info.json"), infos)
              .empty());
  REQUIRE(infos.size() == 1);
  CHECK(infos[0].funding_interval_min == 480);

  TestUniverse u;
  BybitMdParser p(u.symbols, kBybit);
  Scratch s;
  const PaddedJson snap(
      R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"symbol":"BTCUSDT","markPrice":"83100.00","fundingRate":"0.0001","nextFundingTime":"1790784000000","openInterest":"100.5"},"cs":1,"ts":1790757200000})");
  REQUIRE(p.decode(snap.view(), Timestamp{1}, Cycles{1}, s.span()).ok());
  const auto& m = s.as<PerpStateMsg>();
  CHECK((m.fields & PerpStateMsg::kFunding) != 0);
  CHECK(m.funding_interval.ns == 0);       // neither source known yet
  CHECK(m.open_interest == qty("50.25"));  // openInterest counts both sides
  p.set_funding_interval(kBtc, Duration{infos[0].funding_interval_min * 60'000'000'000LL});
  REQUIRE(p.decode(snap.view(), Timestamp{2}, Cycles{1}, s.span()).ok());
  CHECK(m.funding_interval == kEightHours);
}

TEST_CASE("bybit.perp: tickers are subscribed for perpetuals only") {
  PerpUniverse u;
  RecordingSink rs;
  BybitMdFeed perp(u.symbols, kBybit, rs.sink, {}, 50);
  REQUIRE(perp.add_instrument(InstrumentId{0}, true));
  REQUIRE(perp.subscription_payloads().size() == 1);
  CHECK(
      perp.subscription_payloads()[0] ==
      R"({"req_id":"md0","op":"subscribe","args":["orderbook.50.BTCUSDT","orderbook.1.BTCUSDT","publicTrade.BTCUSDT","tickers.BTCUSDT"]})");

  BybitMdFeed spot(u.symbols, kBybit, rs.sink, {}, 50);
  REQUIRE(spot.add_instrument(InstrumentId{0}));
  REQUIRE(spot.add_instrument(InstrumentId{1}, false));
  for (const std::string& p : spot.subscription_payloads())
    CHECK(p.find("tickers.") == std::string::npos);
  CHECK(spot.topics(InstrumentId{1}).size() == 3);
}

TEST_CASE("bybit.perp: the feed turns the recorded stream into one PerpState per tickers push") {
  PerpUniverse u;
  RecordingSink rs(8U << 20);
  BybitMdFeed feed(u.symbols, kBybit, rs.sink, {}, 50);
  REQUIRE(feed.add_instrument(InstrumentId{0}, true));
  REQUIRE(feed.add_instrument(InstrumentId{1}, true));
  feed.on_connected();
  const std::vector<PaddedJson> frames = recorded_frames();
  REQUIRE(frames.size() == 964);  // the subscribe ack and 963 tickers frames
  for (const PaddedJson& f : frames) CHECK(feed.on_message(f.view(), 1) != ParseStatus::Malformed);
  CHECK(feed.stats().malformed == 0);
  CHECK(feed.stats().dropped == 0);
  CHECK(feed.parser_stats().perp_states == 963);

  const auto out = rs.drain();
  std::size_t btc = 0;
  std::size_t eth = 0;
  const PerpStateMsg* last_btc = nullptr;
  const PerpStateMsg* last_eth = nullptr;
  bool all_fields = true;
  for (const auto& raw : out) {
    REQUIRE(RecordingSink::type_of(raw) == EventType::PerpState);
    const auto& m = RecordingSink::as<PerpStateMsg>(raw);
    all_fields = all_fields && m.fields == kAll && m.funding_interval == kEightHours;
    if (m.hdr.instrument == InstrumentId{0}) {
      ++btc;
      last_btc = &m;
    } else {
      ++eth;
      last_eth = &m;
    }
  }
  CHECK(btc == 435);
  CHECK(eth == 528);
  CHECK(all_fields);
  REQUIRE(last_btc != nullptr);
  REQUIRE(last_eth != nullptr);
  // The last value of each field anywhere in the stream, not only in the last frame.
  CHECK(last_btc->mark_price == px("83017.30"));
  CHECK(last_btc->index_price == px("83069.45"));
  CHECK(near(last_btc->funding_rate, -0.00008517));
  CHECK(last_btc->open_interest == qty("28563.246"));
  CHECK(last_btc->hdr.exch_ts.ns == 1790757142183LL * 1'000'000);
  CHECK(last_eth->mark_price == px("2665.19"));
  CHECK(last_eth->index_price == px("2666.12"));
  CHECK(near(last_eth->funding_rate, 0.0000863));
  CHECK(last_eth->open_interest == qty("400239.31"));

  // A reconnect: nothing until the next snapshot.
  feed.on_disconnected();
  feed.on_connected();
  CHECK(feed.on_message(frames.back().view(), 2) == ParseStatus::Ignored);
  CHECK(rs.drain().empty());
}
