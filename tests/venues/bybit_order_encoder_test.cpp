#include "fastmm/venues/bybit/bybit_order_encoder.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/bybit/bybit_error_map.hpp"
#include "fastmm/venues/bybit/bybit_rest_decoder.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::TestUniverse;

namespace {
constexpr std::int64_t kTs = 1789299700000;
Signer test_signer() {
  Credentials c;
  c.api_key = "test-key";
  c.secret.value = "test-secret";
  return Signer(c);
}
OutNewOrderMsg new_order(OrderType type, TimeInForce tif) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{2}, VenueId{1});
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Buy;
  n.type = type;
  n.tif = tif;
  n.price = Price::from_decimal("60000.1").value();
  n.qty = Qty::from_decimal("0.001").value();
  return n;
}
}  // namespace

TEST_CASE("bybit.auth: signatures match an independent HMAC implementation") {
  // Expected values computed with Python hmac/hashlib over the documented pre-sign strings
  // (timestamp + api_key + recv_window + payload, and "GET/realtime" + expires).
  const Signer s = test_signer();
  CHECK(s.sign_rest(kTs, 5000, R"({"category":"spot","symbol":"BTCUSDT"})").view() ==
        "b1dc3b1adfec8bfa24d8b59bcb8e50d1f3cae9aba67e364add58709a0f8201fa");
  CHECK(s.sign_rest(kTs, 5000, "category=spot&limit=50").view() ==
        "f1fe062ac033b033a904223196723fce8fddbbf9e253f95207a166f9d584b314");
  CHECK(s.sign_ws_auth(1789299710000).view() ==
        "2e1331666688d5976b4e096d32124c36f01b719476ad44f001d1d14e184d9632");
  const std::string h =
      s.rest_headers(kTs, 5000, R"({"category":"spot","symbol":"BTCUSDT"})", true);
  CHECK(
      h ==
      "X-BAPI-API-KEY: test-key\r\nX-BAPI-TIMESTAMP: 1789299700000\r\nX-BAPI-RECV-WINDOW: 5000\r\n"
      "X-BAPI-SIGN: b1dc3b1adfec8bfa24d8b59bcb8e50d1f3cae9aba67e364add58709a0f8201fa\r\n"
      "Content-Type: application/json\r\n");
  CHECK_FALSE(Credentials{}.usable());
}

TEST_CASE("bybit.encoder: order.create / order.cancel / order.amend WS frames") {
  TestUniverse u;
  const Signer s = test_signer();
  BybitOrderEncoder enc(s, u.symbols, 5000);
  char buf[kMaxRequestBytes];

  OutNewOrderMsg n = new_order(OrderType::PostOnly, TimeInForce::Gtc);
  std::size_t len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  CHECK(
      std::string_view(buf, len) ==
      R"({"reqId":"nfm000100000001","header":{"X-BAPI-TIMESTAMP":"1789299700000","X-BAPI-RECV-WINDOW":"5000"},"op":"order.create","args":[{"category":"spot","symbol":"BTCUSDT","side":"Buy","orderType":"Limit","qty":"0.001","price":"60000.1","timeInForce":"PostOnly","orderLinkId":"fm000100000001"}]})");

  n = new_order(OrderType::Limit, TimeInForce::Ioc);
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  CHECK(std::string_view(buf, len).find(R"("timeInForce":"IOC")") != std::string_view::npos);
  n = new_order(OrderType::Market, TimeInForce::Gtc);
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  const std::string_view market(buf, len);
  CHECK(market.find(R"("orderType":"Market","qty":"0.001","marketUnit":"baseCoin")") !=
        std::string_view::npos);
  CHECK(market.find("price") == std::string_view::npos);

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{2}, VenueId{1});
  c.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  len = enc.encode_ws(*OrderCommand::from(c.hdr), nullptr, kTs, buf);
  CHECK(
      std::string_view(buf, len).find(
          R"("op":"order.cancel","args":[{"category":"spot","symbol":"BTCUSDT","orderLinkId":"fm000100000001"}])") !=
      std::string_view::npos);
  c.venue_order_id.assign("2012345678901234567");
  len = enc.encode_ws(*OrderCommand::from(c.hdr), nullptr, kTs, buf);
  CHECK(std::string_view(buf, len).find(R"("orderId":"2012345678901234567")") !=
        std::string_view::npos);

  OutReplaceMsg rp{};
  init_header(rp, EventType::OutReplace, InstrumentId{2}, VenueId{1});
  rp.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
  rp.orig_cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  rp.price = Price::from_decimal("60000.2").value();
  rp.qty = Qty::from_decimal("0.002").value();
  CHECK(enc.encode_ws(*OrderCommand::from(rp.hdr), nullptr, kTs, buf) == 0);  // needs the shadow
  OrderShadow sh{
      InstrumentId{2}, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, rp.orig_cl_ord_id, {}};
  len = enc.encode_ws(*OrderCommand::from(rp.hdr), &sh, kTs, buf);
  CHECK(
      std::string_view(buf, len) ==
      R"({"reqId":"rfm000100000002","header":{"X-BAPI-TIMESTAMP":"1789299700000","X-BAPI-RECV-WINDOW":"5000"},"op":"order.amend","args":[{"category":"spot","symbol":"BTCUSDT","orderLinkId":"fm000100000001","qty":"0.002","price":"60000.2"}]})");

  CHECK(
      std::string_view(buf, enc.encode_ws_auth(1789299710000, buf)) ==
      R"({"op":"auth","args":["test-key",1789299710000,"2e1331666688d5976b4e096d32124c36f01b719476ad44f001d1d14e184d9632"]})");
  CHECK(std::string_view(buf, BybitOrderEncoder::encode_ping("p1", buf)) ==
        R"({"req_id":"p1","op":"ping"})");
  const std::string_view topics[] = {"order", "execution"};
  CHECK(std::string_view(buf, BybitOrderEncoder::encode_subscribe("s", topics, buf)) ==
        R"({"req_id":"s","op":"subscribe","args":["order","execution"]})");
  char tiny[16];
  CHECK(enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, tiny) == 0);
}

TEST_CASE("bybit.encoder: REST requests sign the exact bytes sent") {
  TestUniverse u;
  const Signer s = test_signer();
  BybitOrderEncoder enc(s, u.symbols, 5000);
  RestRequest rr;
  const OutNewOrderMsg n = new_order(OrderType::Limit, TimeInForce::Gtc);
  REQUIRE(enc.encode_rest(*OrderCommand::from(n.hdr), nullptr, rr));
  CHECK(rr.method == "POST");
  CHECK(rr.path == "/v5/order/create");
  CHECK(rr.target() == "/v5/order/create");
  CHECK(rr.body.find(R"("orderLinkId":"fm000100000001")") != std::string::npos);
  const std::string h = enc.rest_headers(rr, kTs);
  CHECK(h.find("X-BAPI-SIGN: " + std::string(s.sign_rest(kTs, 5000, rr.body).view())) !=
        std::string::npos);

  REQUIRE(enc.encode_rest_cancel_all("BTCUSDT", rr));
  CHECK(rr.path == "/v5/order/cancel-all");
  CHECK(rr.body == R"({"category":"spot","symbol":"BTCUSDT"})");
  CHECK(enc.rest_headers(rr, kTs).find(
            "b1dc3b1adfec8bfa24d8b59bcb8e50d1f3cae9aba67e364add58709a0f8201fa") !=
        std::string::npos);
  REQUIRE(enc.encode_rest_open_orders({}, {}, rr));
  CHECK(rr.method == "GET");
  CHECK(rr.target() == "/v5/order/realtime?category=spot&limit=50");
  // A page past the first carries the previous page's nextPageCursor.
  RestRequest page2;
  REQUIRE(enc.encode_rest_open_orders({}, "cur%3D2", page2));
  CHECK(page2.target() == "/v5/order/realtime?category=spot&limit=50&cursor=cur%3D2");
  const std::string gh = enc.rest_headers(rr, kTs);
  CHECK(gh.find("f1fe062ac033b033a904223196723fce8fddbbf9e253f95207a166f9d584b314") !=
        std::string::npos);
  CHECK(gh.find("Content-Type") == std::string::npos);
}

TEST_CASE("bybit.decoder: trade responses, REST envelopes, open orders, reference data") {
  BybitResponseDecoder d;
  TradeResponse r;
  auto j = padded_fixture("bybit/trade_create_ok.json");
  REQUIRE(d.decode_ws(j.view(), r) == ParseStatus::Ok);
  CHECK(r.success);
  CHECK(r.req_id == "nfm000100000001");
  CHECK(r.op == "order.create");
  CHECK(r.order_id == "2012345678901234567");
  CHECK(r.order_link_id == "fm000100000001");
  CHECK(r.limit == 20);
  CHECK(r.limit_status == 19);
  CHECK(r.limit_reset_ms == 1789299700208);
  j = padded_fixture("bybit/trade_create_reject.json");
  REQUIRE(d.decode_ws(j.view(), r) == ParseStatus::Ok);
  CHECK_FALSE(r.success);
  CHECK(r.ret_code == 170218);
  CHECK(map_error(r.ret_code, r.ret_msg).reason == RejectReason::PostOnlyWouldCross);
  j = padded_fixture("bybit/auth_ok.json");
  REQUIRE(d.decode_ws(j.view(), r) == ParseStatus::Ok);
  CHECK(r.op == "auth");
  CHECK(r.success);
  j = padded_fixture("bybit/pong.json");
  REQUIRE(d.decode_ws(j.view(), r) == ParseStatus::Ok);
  CHECK(r.is_op_ack);
  j = padded_fixture("bybit/orderbook50_delta.json");
  CHECK(d.decode_ws(j.view(), r) == ParseStatus::Ignored);

  RestResponse rest;
  j = padded_fixture("bybit/rest_error.json");
  REQUIRE(d.decode_rest(j.view(), rest) == ParseStatus::Ok);
  CHECK(rest.ret_code == 10002);
  CHECK(map_error(rest.ret_code).action == VenueAction::ResyncClock);

  std::vector<std::string> links;
  std::string cursor = "stale";
  j = padded_fixture("bybit/rest_open_orders.json");
  REQUIRE(d.decode_open_orders(j.view(), cursor, [&](const OpenOrderRecord& o) {
    links.emplace_back(o.order_link_id);
    if (o.order_link_id == "fm000100000001") {
      CHECK(o.symbol == "BTCUSDT");
      CHECK(o.price == "60000.1");
      CHECK(o.side == "Buy");
      CHECK(o.status == "New");
    }
  }) == ParseStatus::Ok);
  CHECK(links == std::vector<std::string>{"fm000100000001", "manual-1"});
  CHECK(cursor.empty());  // last page
  // retCode != 0 comes back with HTTP 200; the body must not read as an empty snapshot.
  j = padded_fixture("bybit/rest_error.json");
  CHECK(d.decode_open_orders(j.view(), cursor, [](const OpenOrderRecord&) {}) ==
        ParseStatus::Error);

  std::vector<InstrumentInfo> infos;
  REQUIRE(decode_instruments(fastmm::test::fixture("bybit/instruments_info.json"), infos).empty());
  REQUIRE(infos.size() == 1);
  CHECK(infos[0].symbol == "BTCUSDT");
  CHECK(infos[0].status == "Trading");
  CHECK(infos[0].tick == Price::from_decimal("0.1").value());
  CHECK(infos[0].base_precision == Qty::from_decimal("0.000001").value());
  CHECK(infos[0].min_amount == Notional::from_int(5));
  std::int64_t t = 0;
  REQUIRE(decode_server_time(fastmm::test::fixture("bybit/server_time.json"), t).empty());
  CHECK(t == 1789299611107);
  CHECK_FALSE(decode_instruments(fastmm::test::fixture("bybit/rest_error.json"), infos).empty());
}

TEST_CASE("bybit.error_map: documented retCodes") {
  CHECK(map_error(10006).action == VenueAction::RateLimit);
  CHECK(map_error(10002).action == VenueAction::ResyncClock);
  CHECK(map_error(10003).action == VenueAction::Fatal);
  CHECK(map_error(10004).action == VenueAction::Fatal);
  CHECK(map_error(110001).reason == RejectReason::VenueUnknownOrder);
  CHECK(map_error(110001).action == VenueAction::Reconcile);
  CHECK(map_error(170131).reason == RejectReason::InsufficientBalance);
  CHECK(map_error(170137).reason == RejectReason::InvalidLot);
  CHECK(map_error(170999).reason == RejectReason::VenueReject);
  CHECK_FALSE(map_error(170999).known);
  CHECK(map_http_status(403).action == VenueAction::RateLimit);
  CHECK(map_reject_reason("EC_PostOnlyWillTakeLiquidity") == RejectReason::PostOnlyWouldCross);
  CHECK(map_reject_reason("EC_InvalidSymbolStatus") == RejectReason::InstrumentDisabled);
  CHECK(map_reject_reason("EC_DuplicatedClOrdID") == RejectReason::DuplicateId);
  CHECK(map_reject_reason("EC_OrderNotExist") == RejectReason::VenueUnknownOrder);
  CHECK(map_reject_reason("EC_OrigClOrdIDDoesNotExist") == RejectReason::VenueUnknownOrder);
  CHECK(map_reject_reason("EC_BySelfMatch") == RejectReason::SelfTradePrevention);
  CHECK(map_reject_reason("EC_InvalidPriceScale") == RejectReason::InvalidTick);
  CHECK(map_reject_reason("EC_QtyCannotBeZero") == RejectReason::InvalidLot);
  CHECK(map_reject_reason("EC_ReachRiskPriceLimit") == RejectReason::PriceCollar);
  CHECK(map_reject_reason("EC_Others") == RejectReason::VenueReject);
  CHECK(map_reject_reason("EC_InsufficientBalance") == RejectReason::VenueReject);  // not an enum
}
