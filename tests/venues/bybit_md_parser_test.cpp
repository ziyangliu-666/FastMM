#include "fastmm/venues/bybit/bybit_md_parser.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/bybit/bybit_md_feed.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {
constexpr VenueId kBybit{1};
const InstrumentId kBtc{2};  // TestUniverse: BTCUSDT on venue 1
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}
}  // namespace

TEST_CASE("bybit.md_parser: recorded orderbook.50 snapshot and delta") {
  TestUniverse u;
  BybitMdParser p(u.symbols, kBybit);
  Scratch s;
  const auto snap = padded_fixture("bybit/orderbook50_snapshot.json");
  auto r = p.decode(snap.view(), Timestamp{7}, Cycles{9}, s.span());
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::BookSnapshot);
  CHECK(r.count == 1);
  const auto& m = s.as<BookDeltaMsg>();
  CHECK(m.hdr.type == EventType::BookSnapshot);
  CHECK(m.is_snapshot());
  CHECK(m.hdr.instrument == kBtc);
  CHECK(m.hdr.venue == kBybit);
  CHECK(m.bid_count == 50);
  CHECK(m.ask_count == 50);
  CHECK(m.last_update_id == 3734358);
  CHECK(m.first_update_id == 3734358);
  CHECK(m.hdr.venue_seq == 2189232157ULL);
  CHECK(m.hdr.exch_ts.ns == 1789299655598LL * 1'000'000);  // cts
  CHECK(m.hdr.recv_ts.ns == 7);
  CHECK(m.bids()[0] == Level{px("77140.5"), qty("0.216593")});
  CHECK(m.asks()[0] == Level{px("77140.6"), qty("0.324006")});
  CHECK(m.bids()[49] == Level{px("76204"), qty("0.002495")});
  CHECK(m.hdr.len == BookDeltaMsg::size_for(50, 50));
  CHECK(r.len == m.hdr.len);

  const auto delta = padded_fixture("bybit/orderbook50_delta.json");
  r = p.decode(delta.view(), Timestamp{8}, Cycles{9}, s.span());
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::BookDelta);
  const auto& d = s.as<BookDeltaMsg>();
  CHECK(d.hdr.type == EventType::BookDelta);
  CHECK_FALSE(d.is_snapshot());
  CHECK(d.bid_count == 4);
  CHECK(d.ask_count == 2);
  CHECK(d.last_update_id == 3734359);
  CHECK(d.bids()[0].qty.is_zero());  // size "0" = delete
  CHECK(d.bids()[3] == Level{px("76181.8"), qty("0.003835")});
  CHECK(p.stats().book_snapshots == 1);
  CHECK(p.stats().book_deltas == 1);
}

TEST_CASE("bybit.md_parser: orderbook.1 synthesises BookTicker and applies level-1 deltas") {
  TestUniverse u;
  BybitMdParser p(u.symbols, kBybit);
  Scratch s;
  const auto top = padded_fixture("bybit/orderbook1_snapshot.json");
  auto r = p.decode(top.view(), Timestamp{1}, Cycles{1}, s.span());
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::BookTicker);
  const auto& t = s.as<BookTickerMsg>();
  CHECK(t.hdr.type == EventType::BookTicker);
  CHECK(t.hdr.instrument == kBtc);
  CHECK(t.bid_px == px("77140.5"));
  CHECK(t.bid_qty == qty("0.216593"));
  CHECK(t.ask_px == px("77140.6"));
  CHECK(t.ask_qty == qty("0.324006"));
  CHECK(t.hdr.venue_seq == 1980705);

  // A delta that only moves the ask keeps the bid.
  const venues::PaddedJson d(
      R"({"topic":"orderbook.1.BTCUSDT","ts":1789299655700,"type":"delta","data":{"s":"BTCUSDT","b":[],"a":[["77141","1.5"]],"u":1980706,"seq":2189232160},"cts":1789299655690})");
  r = p.decode(d.view(), Timestamp{2}, Cycles{2}, s.span());
  REQUIRE(r.ok());
  CHECK(s.as<BookTickerMsg>().bid_px == px("77140.5"));
  CHECK(s.as<BookTickerMsg>().ask_px == px("77141"));
  CHECK(s.as<BookTickerMsg>().ask_qty == qty("1.5"));
}

TEST_CASE("bybit.md_parser: publicTrade aggressor from S, control frames, errors") {
  TestUniverse u;
  BybitMdParser p(u.symbols, kBybit);
  Scratch s;
  const auto trade = padded_fixture("bybit/public_trade.json");
  auto r = p.decode(trade.view(), Timestamp{3}, Cycles{3}, s.span());
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::Trade);
  CHECK(r.count == 1);
  const auto& t = s.as<TradeMsg>();
  CHECK(t.price == px("77140.5"));
  CHECK(t.qty == qty("0.00389"));
  CHECK(t.aggressor == Side::Sell);  // S = taker side
  CHECK(t.trade_id == 2100000000188691524ULL);
  CHECK(t.hdr.exch_ts.ns == 1789299660688LL * 1'000'000);

  const venues::PaddedJson two(
      R"({"topic":"publicTrade.BTCUSDT","ts":1,"type":"snapshot","data":[{"i":"1","T":5,"p":"1.5","v":"2","S":"Buy","seq":1,"s":"BTCUSDT","BT":false},{"i":"2","T":6,"p":"1.6","v":"3","S":"Sell","seq":2,"s":"BTCUSDT","BT":false}]})");
  r = p.decode(two.view(), Timestamp{3}, Cycles{3}, s.span());
  REQUIRE(r.ok());
  CHECK(r.count == 2);
  CHECK(r.len == 2 * sizeof(TradeMsg));
  CHECK(s.as<TradeMsg>().aggressor == Side::Buy);
  const auto* second = reinterpret_cast<const TradeMsg*>(s.buf + sizeof(TradeMsg));
  CHECK(second->aggressor == Side::Sell);
  CHECK(second->trade_id == 2);

  const auto ok = padded_fixture("bybit/subscribe_ok.json");
  r = p.decode(ok.view(), Timestamp{}, Cycles{}, s.span());
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.control == ControlOp::Subscribe);
  CHECK(r.control_success);
  CHECK(r.req_id == "sub1");
  const auto pong = padded_fixture("bybit/pong.json");
  r = p.decode(pong.view(), Timestamp{}, Cycles{}, s.span());
  CHECK(r.control == ControlOp::Pong);
  const auto bad_sub = padded_fixture("bybit/subscribe_error.json");
  r = p.decode(bad_sub.view(), Timestamp{}, Cycles{}, s.span());
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.ret_msg.find("NOSUCHSYM") != std::string_view::npos);

  const auto unknown = padded_fixture("bybit/public_trade_unknown_symbol.json");
  CHECK(p.decode(unknown.view(), Timestamp{}, Cycles{}, s.span()).status ==
        ParseStatus::UnknownSymbol);
  const auto malformed = padded_fixture("bybit/orderbook50_malformed.json");
  CHECK(p.decode(malformed.view(), Timestamp{}, Cycles{}, s.span()).status ==
        ParseStatus::Malformed);
  const venues::PaddedJson garbage("{\"topic\":");
  CHECK(p.decode(garbage.view(), Timestamp{}, Cycles{}, s.span()).status == ParseStatus::Malformed);
}

TEST_CASE("bybit.md_feed: recorded session syncs the book with no resync") {
  TestUniverse u;
  RecordingSink rs(8U << 20);
  std::vector<InstrumentId> resubs;
  struct Ctx {
    std::vector<InstrumentId>* v;
    static void fn(void* c, InstrumentId id) noexcept { static_cast<Ctx*>(c)->v->push_back(id); }
  } ctx{&resubs};
  BybitMdFeed feed(u.symbols, kBybit, rs.sink, {&Ctx::fn, &ctx}, 50);
  REQUIRE(feed.add_instrument(kBtc));
  REQUIRE(feed.subscription_payloads().size() == 1);
  CHECK(
      feed.subscription_payloads()[0] ==
      R"({"req_id":"md0","op":"subscribe","args":["orderbook.50.BTCUSDT","orderbook.1.BTCUSDT","publicTrade.BTCUSDT"]})");
  feed.on_connected();
  const std::string raw = fastmm::test::fixture("bybit/raw_md_stream.jsonl");
  std::size_t pos = 0;
  std::size_t frames = 0;
  while (pos < raw.size()) {
    std::size_t nl = raw.find('\n', pos);
    if (nl == std::string::npos) nl = raw.size();
    const venues::PaddedJson line(std::string_view(raw).substr(pos, nl - pos));
    static_cast<void>(feed.on_message(line.view(), 1'000'000'000));
    pos = nl + 1;
    ++frames;
  }
  CHECK(frames > 20);
  CHECK(feed.synced_count() == 1);
  CHECK(feed.resync_count() == 0);
  CHECK(resubs.empty());
  CHECK(feed.stats().subscribe_errors == 1);  // the NOSUCHSYM probe at the end
  CHECK(feed.stats().malformed == 0);
  const auto out = rs.drain();
  REQUIRE_FALSE(out.empty());
  std::size_t snapshots = 0;
  std::size_t deltas = 0;
  bool delta_before_snapshot = false;
  for (const auto& m : out) {
    const EventType t = RecordingSink::type_of(m);
    if (t == EventType::BookSnapshot) ++snapshots;
    if (t == EventType::BookDelta) {
      if (snapshots == 0) delta_before_snapshot = true;
      ++deltas;
    }
  }
  CHECK(snapshots == 1);
  CHECK(deltas == 18);
  CHECK_FALSE(delta_before_snapshot);
}
