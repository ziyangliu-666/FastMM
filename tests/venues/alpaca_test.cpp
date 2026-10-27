// Alpaca wire formats: the market-data parser and feed on a recorded stream, the trade_updates
// parser on recorded frames, the REST requests byte for byte, the REST replies and the error map
// on bodies the paper account returned (tests/fixtures/alpaca/fixtures.meta.json).
#include "venue_test_util.hpp"

#include "fastmm/venues/alpaca/alpaca_md.hpp"
#include "fastmm/venues/alpaca/alpaca_rest.hpp"
#include "fastmm/venues/alpaca/alpaca_venue.hpp"
#include "fastmm/venues/registry.hpp"

#include <sstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::alpaca;
using namespace fastmm::venues::test;

namespace {

constexpr VenueId kVenue{0};

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    for (const char* s : {"SPY", "FAKEPACA"}) {
      Instrument i = make_instrument(s, 0, s, "USD");
      i.asset_class = AssetClass::Equity;
      i.lot = Qty::from_int(1);
      REQUIRE(instruments.add(i));
    }
    REQUIRE(symbols.build(instruments));
  }
};

std::vector<std::string> lines(const std::string& rel) {
  std::istringstream in(fastmm::test::fixture(rel));
  std::vector<std::string> out;
  for (std::string l; std::getline(in, l);)
    if (!l.empty()) out.push_back(l);
  return out;
}

OrderCommand new_cmd(const OutNewOrderMsg& m) {
  return *OrderCommand::from(m.hdr);
}

}  // namespace

TEST_CASE("alpaca.md: recorded test stream gives one-level snapshots and trades") {
  Universe u;
  RecordingSink rs(1U << 20);
  AlpacaMdFeed feed(u.symbols, kVenue, rs.sink);
  const InstrumentId fake = u.symbols.find(kVenue, "FAKEPACA");
  REQUIRE(feed.add_instrument(fake));
  CHECK(feed.subscription_payloads()[0] ==
        R"({"action":"subscribe","quotes":["FAKEPACA"],"trades":["FAKEPACA"]})");
  feed.on_connected();
  const auto frames = lines("alpaca/md_test_stream.jsonl");
  REQUIRE(frames.size() == 7);
  std::vector<MdControl> control;
  for (const std::string& f : frames) {
    const PaddedJson p(f);
    const ParseStatus st = feed.on_message(p.view(), 1);
    if (st != ParseStatus::Ok) control.push_back(feed.last().control);
  }
  CHECK(control == std::vector<MdControl>{
                       MdControl::Connected, MdControl::Authenticated, MdControl::Subscription});
  CHECK(feed.synced_count() == 1);
  CHECK(feed.resync_count() == 0);
  const auto msgs = rs.drain();
  std::size_t snaps = 0;
  std::size_t trades = 0;
  for (const auto& m : msgs) {
    if (RecordingSink::type_of(m) == EventType::BookSnapshot) {
      const auto& b = RecordingSink::as<BookDeltaMsg>(m);
      CHECK(b.is_snapshot());
      REQUIRE(b.bid_count == 1);
      REQUIRE(b.ask_count == 1);
      CHECK(b.bids()[0].price == Price::from_decimal("133.85").value());
      CHECK(b.bids()[0].qty == Qty::from_int(4));
      CHECK(b.asks()[0].price == Price::from_decimal("135.77").value());
      CHECK(b.asks()[0].qty == Qty::from_int(5));
      CHECK(b.hdr.exch_ts.ns > 0);
      ++snaps;
    } else if (RecordingSink::type_of(m) == EventType::Trade) {
      const auto& t = RecordingSink::as<TradeMsg>(m);
      CHECK(t.price == Price::from_decimal("134.56").value());
      CHECK(t.qty == Qty::from_int(3));
      CHECK_FALSE(t.has_aggressor());
      ++trades;
    }
  }
  CHECK(snaps == 3);
  CHECK(trades == 3);
}

TEST_CASE("alpaca.md: empty sides, errors, unknown symbols, malformed frames") {
  Universe u;
  AlpacaMdParser parser(u.symbols, kVenue);
  Scratch s;
  {
    const PaddedJson p(
        R"([{"T":"q","S":"SPY","bx":"V","bp":0,"bs":0,"ax":"V","ap":765.19,"as":1,"c":["R"],"z":"B","t":"2026-10-27T13:30:00.5Z"}])");
    const auto r = parser.decode(p.view(), Timestamp{1}, Cycles{}, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    const auto& b = s.as<BookDeltaMsg>();
    CHECK(b.bid_count == 0);
    CHECK(b.ask_count == 1);
    CHECK(b.hdr.exch_ts.ns == 1793107800500000000LL);
  }
  {
    const PaddedJson p(R"([{"T":"error","code":406,"msg":"connection limit exceeded"}])");
    const auto r = parser.decode(p.view(), Timestamp{1}, Cycles{}, s.span());
    CHECK(r.status == ParseStatus::Error);
    CHECK(r.control == MdControl::Error);
    CHECK(r.error_code == 406);
    CHECK(r.msg == "connection limit exceeded");
  }
  {
    const PaddedJson p(
        R"([{"T":"t","S":"QQQ","i":7,"x":"V","p":1.5,"s":3,"c":["@"],"z":"C","t":"2026-10-27T13:30:00Z"}])");
    CHECK(parser.decode(p.view(), Timestamp{1}, Cycles{}, s.span()).status ==
          ParseStatus::UnknownSymbol);
  }
  for (const char* bad : {R"({"T":"q"})",
                          R"([{"T":"q","S":"SPY","bp":"x"}])",
                          "[1]",
                          R"([{"T":"t","S":"SPY","p":1.5,"s":3,"t":"yesterday"}])"}) {
    const PaddedJson p(bad);
    CHECK(parser.decode(p.view(), Timestamp{1}, Cycles{}, s.span()).status ==
          ParseStatus::Malformed);
  }
  {
    const PaddedJson p(R"([{"T":"c","S":"SPY"},{"T":"b","S":"SPY"}])");
    CHECK(parser.decode(p.view(), Timestamp{1}, Cycles{}, s.span()).status == ParseStatus::Ignored);
  }
}

TEST_CASE("alpaca.trade_updates: recorded frames") {
  TradeUpdateParser parser;
  const auto frames = lines("alpaca/trade_updates.jsonl");
  REQUIRE(frames.size() == 9);
  std::vector<std::string> events;
  TradeUpdate u;
  for (const std::string& f : frames) {
    const PaddedJson p(f);
    REQUIRE(parser.parse(p.view(), u));
    if (u.stream == TradeUpdate::Stream::Authorization) CHECK(u.status == "authorized");
    if (u.stream != TradeUpdate::Stream::TradeUpdates) continue;
    events.emplace_back(u.event);
    if (u.event == "replaced") {
      CHECK(u.order.client_order_id == "fm000100000001");
      CHECK(u.order.status == "replaced");
      CHECK(u.order.replaced_by == "00000000-0000-4000-8000-000000000002");
    }
    if (u.event == "fill") {
      CHECK(u.order.client_order_id == "fm000100000003");
      CHECK(u.order.symbol == "SPY");
      CHECK(u.order.side == "buy");
      CHECK(u.price == Price::from_decimal("765.37").value());
      CHECK(u.qty == Qty::from_int(1));
      CHECK(u.order.filled_qty == Qty::from_int(1));
      CHECK(u.order.qty == Qty::from_int(1));
      CHECK(u.execution_id == "00000000-0000-4000-8000-000000000009");
      CHECK(u.time_ns == 1793082780332634814LL);
    }
  }
  CHECK(events == std::vector<std::string>{"replaced", "canceled", "pending_new", "new", "fill"});
  const PaddedJson bad(R"({"stream":"trade_updates","data":{"event":"fill","order":[]}})");
  CHECK_FALSE(parser.parse(bad.view(), u));
}

TEST_CASE("alpaca.encoder: new, replace and the request paths") {
  Universe u;
  OutNewOrderMsg m{};
  init_header(m, EventType::OutNewOrder, u.symbols.find(kVenue, "SPY"), kVenue);
  m.cl_ord_id = ClientOrderId{0x000100000003ULL};
  m.side = Side::Buy;
  m.type = OrderType::PostOnly;
  m.tif = TimeInForce::Gtc;
  m.price = Price::from_decimal("765.37").value();
  m.qty = Qty::from_int(2);
  OrderRequest rq;
  AlpacaOrderEncoder day(u.symbols, {});
  REQUIRE(day.encode_new(new_cmd(m), rq));
  CHECK(rq.target() == "/v2/orders");
  CHECK(
      rq.payload() ==
      R"({"symbol":"SPY","qty":"2","side":"buy","type":"limit","time_in_force":"day","limit_price":"765.37","client_order_id":"fm000100000003"})");
  AlpacaOrderEncoder ext(u.symbols, {.resting_tif = Tif::Gtc, .extended_hours = true});
  REQUIRE(ext.encode_new(new_cmd(m), rq));
  CHECK(
      rq.payload() ==
      R"({"symbol":"SPY","qty":"2","side":"buy","type":"limit","time_in_force":"gtc","limit_price":"765.37","client_order_id":"fm000100000003","extended_hours":true})");
  m.tif = TimeInForce::Ioc;
  m.side = Side::Sell;
  REQUIRE(ext.encode_new(new_cmd(m), rq));
  CHECK(
      rq.payload() ==
      R"({"symbol":"SPY","qty":"2","side":"sell","type":"limit","time_in_force":"ioc","limit_price":"765.37","client_order_id":"fm000100000003"})");
  m.type = OrderType::Market;
  m.tif = TimeInForce::Gtc;
  REQUIRE(ext.encode_new(new_cmd(m), rq));
  CHECK(
      rq.payload() ==
      R"({"symbol":"SPY","qty":"2","side":"sell","type":"market","time_in_force":"day","client_order_id":"fm000100000003"})");

  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, u.symbols.find(kVenue, "SPY"), kVenue);
  r.cl_ord_id = ClientOrderId{0x000100000004ULL};
  r.orig_cl_ord_id = ClientOrderId{0x000100000003ULL};
  r.price = Price::from_decimal("765.4").value();
  r.qty = Qty::from_int(1);
  REQUIRE(AlpacaOrderEncoder::encode_replace(
      *OrderCommand::from(r.hdr), "00000000-0000-4000-8000-000000000005", rq));
  CHECK(rq.target() == "/v2/orders/00000000-0000-4000-8000-000000000005");
  CHECK(rq.payload() == R"({"qty":"1","limit_price":"765.4","client_order_id":"fm000100000004"})");
  CHECK_FALSE(AlpacaOrderEncoder::encode_replace(*OrderCommand::from(r.hdr), "", rq));

  const std::string syms[] = {"SPY", "BRK.B"};
  CHECK(AlpacaOrderEncoder::open_orders_path(syms) ==
        "/v2/orders?status=open&limit=500&direction=asc&symbols=SPY%2CBRK.B");
  CHECK(AlpacaOrderEncoder::fills_path(1793082780333, 0, "", 100) ==
        "/v2/account/activities/FILL?direction=desc&page_size=100&after=2026-10-27T06:33:00.332Z");
  CHECK(AlpacaOrderEncoder::fills_path(0, 1793082780333, "x::y", 50) ==
        "/v2/account/activities/FILL?direction=desc&page_size=50&until=2026-10-27T06:33:00.333Z"
        "&page_token=x::y");
}

TEST_CASE("alpaca.rest: replies recorded on the paper account") {
  OrderRow o;
  REQUIRE(
      decode_order(
          R"({"id":"00000000-0000-4000-8000-000000000002","client_order_id":"fm000100000002","created_at":"2026-10-27T06:31:30.773202498Z","replaced_by":null,"replaces":"00000000-0000-4000-8000-000000000001","symbol":"AAPL","asset_class":"us_equity","notional":null,"qty":"2","filled_qty":"0","filled_avg_price":null,"order_class":"","order_type":"limit","type":"limit","side":"buy","position_intent":"buy_to_open","time_in_force":"day","limit_price":"101","stop_price":null,"status":"new","extended_hours":true,"legs":null})",
          o)
          .empty());
  CHECK(o.id == "00000000-0000-4000-8000-000000000002");
  CHECK(o.client_order_id == "fm000100000002");
  CHECK(o.limit_price == Price::from_int(101));
  CHECK(o.qty == Qty::from_int(2));
  CHECK(o.status == "new");
  CHECK(o.replaced_by.empty());

  std::vector<OrderRow> rows;
  CHECK(decode_orders("[]", rows).empty());
  CHECK(rows.empty());
  CHECK_FALSE(decode_orders("{}", rows).empty());

  std::vector<PositionRow> pos;
  REQUIRE(
      decode_positions(
          R"([{"asset_id":"00000000-0000-4000-8000-000000000009","symbol":"SPY","exchange":"ARCA","asset_class":"us_equity","asset_marginable":true,"qty":"1","qty_available":"1","avg_entry_price":"765.37","side":"long","market_value":"765.1","cost_basis":"765.37"},{"symbol":"AAPL","qty":"-3","avg_entry_price":"230.5","side":"short"}])",
          pos)
          .empty());
  REQUIRE(pos.size() == 2);
  CHECK(pos[0].qty == Qty::from_int(1));
  CHECK(pos[0].avg_px == Price::from_decimal("765.37").value());
  CHECK(pos[1].qty == Qty::from_int(-3));

  std::vector<FillRow> fills;
  REQUIRE(
      decode_fills(
          R"([{"id":"20261027023300332::00000000-0000-4000-8000-000000000006","activity_type":"FILL","transaction_time":"2026-10-27T06:33:00.332635Z","type":"fill","price":"765.37","qty":"1","side":"buy","symbol":"SPY","leaves_qty":"0","order_id":"00000000-0000-4000-8000-000000000005","cum_qty":"1","order_status":"filled"},{"id":"20261027023400000::e2","activity_type":"FILL","transaction_time":"2026-10-27T06:34:00Z","type":"partial_fill","price":"765.5","qty":"2","side":"sell_short","symbol":"SPY","order_id":"o2"}])",
          fills)
          .empty());
  REQUIRE(fills.size() == 2);
  CHECK(fills[0].exec_id == "00000000-0000-4000-8000-000000000006");
  CHECK(fills[0].page_id == "20261027023300332::00000000-0000-4000-8000-000000000006");
  CHECK(fills[0].time_ms == 1793082780332);
  CHECK(fills[0].side == Side::Buy);
  CHECK(fills[1].side == Side::Sell);

  AssetInfo a;
  REQUIRE(
      decode_asset(
          R"({"id":"x","class":"us_equity","exchange":"ARCA","symbol":"SPY","status":"active","tradable":true,"marginable":true,"shortable":true,"easy_to_borrow":true,"fractionable":true})",
          a)
          .empty());
  CHECK(a.tradable);
  CHECK(a.shortable);
  CHECK(a.fractionable);

  std::int64_t ms = 0;
  bool open = true;
  REQUIRE(
      decode_clock(
          R"({"is_open":false,"next_close":"2026-10-27T16:00:00-04:00","next_open":"2026-10-27T09:30:00-04:00","timestamp":"2026-10-27T02:28:45.264685368-04:00"})",
          ms,
          open)
          .empty());
  CHECK(ms == 1793082525264);
  CHECK_FALSE(open);

  const ApiError e = decode_error(
      R"({"buy_limit_price":"950","code":40310000,"existing_order_id":"x","message":"potential wash trade detected. use complex orders","reject_reason":"sell order exists, buy limit price should be less than existing sell limit price","sell_limit_price":"900"})");
  CHECK(e.code == 40310000);
  CHECK(e.message == "potential wash trade detected. use complex orders");
  CHECK(decode_error("not json").code == 0);
}

TEST_CASE("alpaca.error_map: refusals seen on the paper account") {
  const auto m = [](int status, std::int64_t code, std::string_view msg) {
    return map_error(status, code, msg);
  };
  CHECK(m(403, 40310000, "potential wash trade detected. use complex orders").reason ==
        RejectReason::SelfTradePrevention);
  CHECK(m(403, 40310000, "cannot open a short sell while a long buy order is open").reason ==
        RejectReason::SelfTradePrevention);
  CHECK(m(403, 40310000, "insufficient qty available for order (requested: 5, available: 0)")
            .reason == RejectReason::InsufficientBalance);
  CHECK(m(403, 40310000, "insufficient buying power").reason == RejectReason::InsufficientBalance);
  const ErrorMapping tick =
      m(422,
        42210000,
        "invalid limit_price 700.123. sub-penny increment does not fulfill minimum "
        "pricing criteria");
  CHECK(tick.reason == RejectReason::InvalidTick);
  CHECK(tick.action == VenueAction::DisableInstrument);
  CHECK(m(422, 42210000, "order is not open").reason == RejectReason::VenueUnknownOrder);
  CHECK(m(422, 40010001, "client_order_id must be unique").reason == RejectReason::DuplicateId);
  CHECK(m(404, 40410000, "order not found for x").reason == RejectReason::VenueUnknownOrder);
  CHECK(m(401, 40110000, "request is not authorized").action == VenueAction::Fatal);
  CHECK(m(429, 0, "rate limit exceeded").action == VenueAction::RateLimit);
  CHECK(m(500, 0, "internal").action == VenueAction::Backoff);
  CHECK_FALSE(m(418, 0, "teapot").known);
}

TEST_CASE("alpaca.config: defaults, keys and the live-host guard") {
  VenueSection s;
  s.name = "alpaca";
  s.kind = "alpaca";
  s.api_key = "PKTEST";
  s.api_secret = "secret";
  AlpacaVenueConfig c = make_alpaca_config(s, false);
  CHECK(c.rest_url == "https://paper-api.alpaca.markets");
  CHECK(c.ws_url == "wss://stream.data.alpaca.markets/v2/iex");
  CHECK(c.ws_private_url == "wss://paper-api.alpaca.markets/stream");
  CHECK(c.orders.resting_tif == Tif::Day);
  CHECK_FALSE(c.orders.extended_hours);
  s.extra["time_in_force"] = "gtc";
  s.extra["extended_hours"] = "true";
  s.extra["requests_per_minute"] = "120";
  c = make_alpaca_config(s, false);
  CHECK(c.orders.resting_tif == Tif::Gtc);
  CHECK(c.orders.extended_hours);
  CHECK(c.requests_per_minute == 120);
  s.extra["time_in_force"] = "ioc";
  CHECK_THROWS_AS(make_alpaca_config(s, false), std::invalid_argument);
  s.extra.erase("time_in_force");
  s.rest_url = "https://api.alpaca.markets";
  CHECK_THROWS_AS(make_alpaca_config(s, false), std::invalid_argument);
  s.testnet = false;
  CHECK_NOTHROW(make_alpaca_config(s, false));
  s.api_secret = "bad\"secret";
  CHECK_THROWS_AS(make_alpaca_config(s, false), std::invalid_argument);

  VenueRegistry reg;
  register_builtin_venues(reg);
  const VenueEntry* e = reg.find("alpaca");
  REQUIRE(e != nullptr);
  CHECK(e->caps.replace);
  CHECK(e->caps.executions);
  CHECK(e->caps.position_snapshot);
}
