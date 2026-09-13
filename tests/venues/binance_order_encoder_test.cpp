#include "fastmm/venues/binance/binance_order_encoder.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/binance/binance_error_map.hpp"
#include "fastmm/venues/binance/binance_rest_decoder.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::TestUniverse;

namespace {
// Known-answer key pair from the Binance docs ("SIGNED Endpoint Examples for POST
// /api/v3/order", rest-api.md): the documented signature for the documented payload.
constexpr std::string_view kDocApiKey =
    "vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A";
constexpr std::string_view kDocSecret =
    "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
constexpr std::string_view kDocPayload =
    "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&"
    "timestamp=1499827319559";
constexpr std::string_view kDocSignature =
    "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71";

Signer hmac_signer() {
  Credentials c;
  c.api_key = std::string(kDocApiKey);
  c.secret.value = std::string(kDocSecret);
  return Signer(c);
}
constexpr std::int64_t kTs = 1789295199000;
}  // namespace

TEST_CASE("binance.auth: HMAC known answer from the official docs") {
  const Signer s = hmac_signer();
  CHECK(s.sign_hmac(kDocPayload).view() == kDocSignature);
  CHECK(s.sign(kDocPayload) == kDocSignature);
  net::QueryBuilder<512> q;
  q.append_raw(kDocPayload);
  REQUIRE(s.sign_query(q));
  CHECK(q.view() == std::string(kDocPayload) + "&signature=" + std::string(kDocSignature));
  CHECK(api_key_header("k") == "X-MBX-APIKEY: k\r\n");
  Credentials bad;
  CHECK_FALSE(bad.usable());
}

TEST_CASE("binance.encoder: order.place golden JSON with sorted params and signature") {
  TestUniverse u;
  const Signer s = hmac_signer();
  BinanceOrderEncoder enc(s, u.symbols, 3000);
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.tif = TimeInForce::Gtc;
  n.price = Price::from_decimal("70000.5").value();
  n.qty = Qty::from_decimal("0.001").value();
  char buf[kMaxRequestBytes];
  const std::size_t len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  const std::string payload =
      "apiKey=" + std::string(kDocApiKey) +
      "&newClientOrderId=fm000100000001&newOrderRespType=ACK&price=70000.5&quantity=0.001"
      "&recvWindow=3000&side=BUY&symbol=BTCUSDT&timestamp=1789295199000&type=LIMIT_MAKER";
  const std::string sig(s.sign_hmac(payload).view());
  const std::string expected =
      R"({"id":"nfm000100000001","method":"order.place","params":{"apiKey":")" +
      std::string(kDocApiKey) +
      R"(","newClientOrderId":"fm000100000001","newOrderRespType":"ACK","price":"70000.5","quantity":"0.001","recvWindow":3000,"side":"BUY","symbol":"BTCUSDT","timestamp":1789295199000,"type":"LIMIT_MAKER","signature":")" +
      sig + R"("}})";
  CHECK(std::string_view(buf, len) == expected);

  // LIMIT carries timeInForce; MARKET carries neither price nor timeInForce.
  n.type = OrderType::Limit;
  n.tif = TimeInForce::Ioc;
  const std::size_t l2 = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(l2 > 0);
  const std::string_view v2(buf, l2);
  CHECK(v2.find(R"("timeInForce":"IOC","timestamp":1789295199000,"type":"LIMIT")") !=
        std::string_view::npos);
  n.type = OrderType::Market;
  const std::size_t l3 = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(l3 > 0);
  const std::string_view v3(buf, l3);
  CHECK(v3.find("price") == std::string_view::npos);
  CHECK(v3.find("timeInForce") == std::string_view::npos);
  CHECK(v3.find(R"("type":"MARKET")") != std::string_view::npos);
}

TEST_CASE("binance.encoder: cancel by client id / order id, cancelReplace, cancelAll") {
  TestUniverse u;
  const Signer s = hmac_signer();
  BinanceOrderEncoder enc(s, u.symbols, 3000);
  char buf[kMaxRequestBytes];
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  std::size_t len = enc.encode_ws(*OrderCommand::from(c.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  std::string_view v(buf, len);
  CHECK(v.starts_with(R"({"id":"cfm000100000001","method":"order.cancel","params":{"apiKey":")"));
  CHECK(
      v.find(
          R"("origClientOrderId":"fm000100000001","recvWindow":3000,"symbol":"BTCUSDT","timestamp":1789295199000,"signature":")") !=
      std::string_view::npos);
  c.venue_order_id = "4293153";
  len = enc.encode_ws(*OrderCommand::from(c.hdr), nullptr, kTs, buf);
  v = std::string_view(buf, len);
  CHECK(v.find(R"("orderId":4293153,"recvWindow":3000)") != std::string_view::npos);
  CHECK(v.find("origClientOrderId") == std::string_view::npos);

  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  r.cl_ord_id = decode_cl_ord_id("fm000100000005").value();
  r.orig_cl_ord_id = c.cl_ord_id;
  r.price = Price::from_int(69000);
  r.qty = Qty::from_decimal("0.002").value();
  CHECK(enc.encode_ws(*OrderCommand::from(r.hdr), nullptr, kTs, buf) == 0);  // needs shadow
  const OrderShadow shadow{Side::Buy, OrderType::PostOnly, TimeInForce::Gtc};
  len = enc.encode_ws(*OrderCommand::from(r.hdr), &shadow, kTs, buf);
  REQUIRE(len > 0);
  v = std::string_view(buf, len);
  CHECK(v.starts_with(
      R"({"id":"rfm000100000005","method":"order.cancelReplace","params":{"apiKey":")"));
  CHECK(
      v.find(
          R"("cancelOrigClientOrderId":"fm000100000001","cancelReplaceMode":"STOP_ON_FAILURE","newClientOrderId":"fm000100000005","newOrderRespType":"ACK","price":"69000","quantity":"0.002","recvWindow":3000,"side":"BUY","symbol":"BTCUSDT","timestamp":1789295199000,"type":"LIMIT_MAKER")") !=
      std::string_view::npos);

  len = enc.encode_ws_cancel_all("BTCUSDT", "ca1", kTs, buf);
  REQUIRE(len > 0);
  v = std::string_view(buf, len);
  CHECK(v.starts_with(R"({"id":"ca1","method":"openOrders.cancelAll","params":{"apiKey":")"));
  len = enc.encode_ws_open_orders("BTCUSDT", "oo1", kTs, buf);
  REQUIRE(len > 0);
  CHECK(std::string_view(buf, len).find(R"("method":"openOrders.status")") !=
        std::string_view::npos);
  len = BinanceOrderEncoder::encode_ws_ping("p1", buf);
  CHECK(std::string_view(buf, len) == R"({"id":"p1","method":"ping"})");
  len = enc.encode_ws_user_stream_subscribe("uds", kTs, false, buf);
  CHECK(std::string_view(buf, len) == R"({"id":"uds","method":"userDataStream.subscribe"})");
  len = enc.encode_ws_user_stream_subscribe("uds", kTs, true, buf);
  v = std::string_view(buf, len);
  CHECK(v.starts_with(
      R"({"id":"uds","method":"userDataStream.subscribe.signature","params":{"apiKey":")"));
  const std::string sub_sig(
      s.sign_hmac("apiKey=" + std::string(kDocApiKey) + "&recvWindow=3000&timestamp=1789295199000")
          .view());
  CHECK(v.find(R"("signature":")" + sub_sig + "\"") != std::string_view::npos);
  len = enc.encode_ws_logon("lg", kTs, buf);
  CHECK(std::string_view(buf, len).find(R"("method":"session.logon")") != std::string_view::npos);

  // Session-authenticated: no apiKey / signature.
  enc.set_session_authenticated(true);
  len = enc.encode_ws(*OrderCommand::from(c.hdr), nullptr, kTs, buf);
  v = std::string_view(buf, len);
  CHECK(v.find("apiKey") == std::string_view::npos);
  CHECK(v.find("signature") == std::string_view::npos);
  CHECK(
      v ==
      R"({"id":"cfm000100000001","method":"order.cancel","params":{"orderId":4293153,"recvWindow":3000,"symbol":"BTCUSDT","timestamp":1789295199000}})");

  // Unknown instrument / too small buffer.
  OutCancelMsg bad = c;
  bad.hdr.instrument = InstrumentId{77};
  CHECK(enc.encode_ws(*OrderCommand::from(bad.hdr), nullptr, kTs, buf) == 0);
  char tiny[32];
  CHECK(enc.encode_ws(*OrderCommand::from(c.hdr), nullptr, kTs, tiny) == 0);
}

TEST_CASE("binance.encoder: REST fallback queries are signed over the exact bytes sent") {
  TestUniverse u;
  const Signer s = hmac_signer();
  BinanceOrderEncoder enc(s, u.symbols, 5000);
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Sell;
  n.type = OrderType::Limit;
  n.tif = TimeInForce::Gtc;
  n.price = Price::from_decimal("80000").value();
  n.qty = Qty::from_decimal("0.001").value();
  RestRequest rr;
  REQUIRE(enc.encode_rest(*OrderCommand::from(n.hdr), nullptr, kTs, rr));
  CHECK(rr.method == "POST");
  CHECK(rr.path == "/api/v3/order");
  CHECK(rr.is_order);
  const std::string q(rr.query.view());
  const std::string body = q.substr(0, q.find("&signature="));
  CHECK(
      body ==
      "newClientOrderId=fm000100000001&newOrderRespType=ACK&price=80000&quantity=0.001&recvWindow="
      "5000&side=SELL&symbol=BTCUSDT&timeInForce=GTC&timestamp=1789295199000&type=LIMIT");
  CHECK(q.substr(q.find("&signature=") + 11) == std::string(s.sign_hmac(body).view()));
  CHECK(q.find("apiKey") == std::string::npos);  // header, not query

  REQUIRE(enc.encode_rest_cancel_all("BTCUSDT", kTs, rr));
  CHECK(rr.method == "DELETE");
  CHECK(rr.path == "/api/v3/openOrders");
  CHECK(std::string(rr.query.view())
            .starts_with("recvWindow=5000&symbol=BTCUSDT&timestamp=1789295199000&signature="));
  REQUIRE(enc.encode_rest_open_orders("BTCUSDT", kTs, rr));
  CHECK(rr.method == "GET");
  CHECK(rr.weight == 6);
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = n.cl_ord_id;
  REQUIRE(enc.encode_rest(*OrderCommand::from(c.hdr), nullptr, kTs, rr));
  CHECK(rr.method == "DELETE");
  CHECK(rr.path == "/api/v3/order");
}

TEST_CASE("binance.encoder: request ids round-trip") {
  const ClientOrderId id = decode_cl_ord_id("fm0001000000ab").value();
  const RequestId r = make_request_id(RequestKind::Replace, id);
  CHECK(r.view() == "rfm0001000000ab");
  const auto parsed = parse_request_id(r.view());
  REQUIRE(parsed.has_value());
  CHECK(parsed->first == RequestKind::Replace);
  CHECK(parsed->second == id);
  CHECK_FALSE(parse_request_id("uds").has_value());
  CHECK_FALSE(parse_request_id("xfm0001000000ab").has_value());
  CHECK_FALSE(parse_request_id("nfm00010000zzab").has_value());
}

TEST_CASE("binance.ws_api: response decoding (ok, cancel, cancelReplace, errors, rate limits)") {
  BinanceWsApiDecoder d;
  WsApiResponse r;
  auto place = padded_fixture("binance/ws_api_order_place_ok.json");
  REQUIRE(d.decode(place.view(), r) == ParseStatus::Ok);
  CHECK(r.id == "nfm000100000001");
  CHECK(r.status == 200);
  CHECK_FALSE(r.is_error);
  CHECK(r.client_order_id == "fm000100000001");
  CHECK(r.order_id == 4293153);
  CHECK(r.rate.used_weight == 12);
  CHECK(r.rate.weight_limit == 6000);
  CHECK(r.rate.order_count == 1);  // shortest ORDERS window (10 s)
  CHECK(r.rate.order_limit == 50);

  auto cancel = padded_fixture("binance/ws_api_order_cancel_ok.json");
  REQUIRE(d.decode(cancel.view(), r) == ParseStatus::Ok);
  CHECK(r.id == "cfm000100000001");
  CHECK(r.orig_client_order_id == "fm000100000001");
  CHECK(r.order_status == "CANCELED");
  CHECK(r.executed_qty == "0.00040000");

  auto cr = padded_fixture("binance/ws_api_cancel_replace_ok.json");
  REQUIRE(d.decode(cr.view(), r) == ParseStatus::Ok);
  CHECK(r.cancel_result == "SUCCESS");
  CHECK(r.new_order_result == "SUCCESS");
  CHECK(r.cancel_client_order_id == "fm000100000001");
  CHECK(r.cancel_order_id == 4293153);
  CHECK(r.cancel_executed_qty == "0.00040000");
  CHECK(r.client_order_id == "fm000100000005");
  CHECK(r.order_id == 4293160);

  auto err = padded_fixture("binance/ws_api_error.json");
  REQUIRE(d.decode(err.view(), r) == ParseStatus::Ok);
  CHECK(r.is_error);
  CHECK(r.status == 400);
  CHECK(r.code == -2010);
  CHECK(r.msg == "Order would immediately match and take.");
  auto rl = padded_fixture("binance/ws_api_error_rate_limit.json");
  REQUIRE(d.decode(rl.view(), r) == ParseStatus::Ok);
  CHECK(r.status == 429);
  CHECK(r.code == -1003);
  CHECK(r.retry_after_ms == 1789295220000);
  CHECK(r.rate.used_weight == 6100);
  auto sub = padded_fixture("binance/ws_api_user_stream_subscribe_ok.json");
  REQUIRE(d.decode(sub.view(), r) == ParseStatus::Ok);
  CHECK(r.subscription_id == 0);
  auto oo = padded_fixture("binance/ws_api_open_orders.json");
  REQUIRE(d.decode(oo.view(), r) == ParseStatus::Ok);
  CHECK(r.result_is_array);
  std::vector<std::string> ids;
  REQUIRE(d.decode_open_orders(oo.view(), false, [&](const OpenOrderRecord& o) {
    ids.emplace_back(o.client_order_id);
    CHECK(o.symbol == "BTCUSDT");
  }) == ParseStatus::Ok);
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == "fm000100000001");
  CHECK(ids[1] == "web_manual_1");
  auto rest = padded_fixture("binance/open_orders.json");
  int n = 0;
  REQUIRE(d.decode_open_orders(rest.view(), true, [&](const OpenOrderRecord& o) {
    ++n;
    CHECK(o.executed_qty == "0.00040000");
    CHECK(o.status == "PARTIALLY_FILLED");
    CHECK(o.side == "BUY");
    CHECK(o.type == "LIMIT_MAKER");
    CHECK(o.tif == "GTC");
    CHECK(o.price == "70000.00000000");
    CHECK(o.orig_qty == "0.00100000");
    CHECK(o.order_id == 4293153);
  }) == ParseStatus::Ok);
  CHECK(n == 1);
  auto ev = padded_fixture("binance/exec_report_new.json");
  CHECK(d.decode(ev.view(), r) == ParseStatus::Ignored);
  const PaddedJson garbage("nope");
  CHECK(d.decode(garbage.view(), r) == ParseStatus::Malformed);
}

TEST_CASE("binance.error_map: every documented code maps and actions are right") {
  using fastmm::venues::VenueAction;
  struct Case {
    int code;
    std::string_view msg;
    RejectReason reason;
    VenueAction action;
  };
  const Case cases[] = {
      {-1003, "", RejectReason::VenueRateLimit, VenueAction::RateLimit},
      {-1015, "", RejectReason::VenueRateLimit, VenueAction::RateLimit},
      {-1021, "", RejectReason::VenueReject, VenueAction::ResyncClock},
      {-1022, "", RejectReason::VenueReject, VenueAction::Fatal},
      {-2014, "", RejectReason::VenueReject, VenueAction::Fatal},
      {-2015, "", RejectReason::VenueReject, VenueAction::Fatal},
      {-1013,
       "Filter failure: PRICE_FILTER",
       RejectReason::InvalidTick,
       VenueAction::DisableInstrument},
      {-1013, "Filter failure: LOT_SIZE", RejectReason::InvalidLot, VenueAction::DisableInstrument},
      {-1013,
       "Filter failure: MIN_NOTIONAL",
       RejectReason::BelowMinNotional,
       VenueAction::DisableInstrument},
      {-1013,
       "Filter failure: NOTIONAL",
       RejectReason::BelowMinNotional,
       VenueAction::DisableInstrument},
      {-1013, "Filter failure: MAX_NUM_ORDERS", RejectReason::MaxOpenOrders, VenueAction::None},
      {-1111, "", RejectReason::InvalidTick, VenueAction::DisableInstrument},
      {-2010,
       "Order would immediately match and take.",
       RejectReason::PostOnlyWouldCross,
       VenueAction::None},
      {-2010,
       "Account has insufficient balance for requested action.",
       RejectReason::InsufficientBalance,
       VenueAction::None},
      {-2010, "Duplicate order sent.", RejectReason::DuplicateId, VenueAction::Reconcile},
      {-2010, "Filter failure: LOT_SIZE", RejectReason::InvalidLot, VenueAction::DisableInstrument},
      {-2010, "something new", RejectReason::VenueReject, VenueAction::None},
      {-2011, "Unknown order sent.", RejectReason::VenueUnknownOrder, VenueAction::Reconcile},
      {-2011, "whatever", RejectReason::VenueReject, VenueAction::Reconcile},
      {-2013, "", RejectReason::VenueUnknownOrder, VenueAction::Reconcile},
      {-1006, "", RejectReason::VenueReject, VenueAction::Reconcile},
      {-1007, "", RejectReason::VenueReject, VenueAction::Reconcile},
      {-1000, "", RejectReason::VenueReject, VenueAction::Backoff},
      {-1001, "", RejectReason::VenueReject, VenueAction::Backoff},
      {-1008, "", RejectReason::VenueReject, VenueAction::Backoff},
      {-1002, "", RejectReason::VenueReject, VenueAction::Fatal},
      {-1100, "", RejectReason::VenueReject, VenueAction::None},
      {-1102, "", RejectReason::VenueReject, VenueAction::None},
      {-1121, "", RejectReason::VenueReject, VenueAction::None},
      {-2021, "", RejectReason::VenueReject, VenueAction::Reconcile},
      {-2022,
       "Order would immediately match and take.",
       RejectReason::PostOnlyWouldCross,
       VenueAction::None},
      {-2026, "", RejectReason::VenueUnknownOrder, VenueAction::None},
  };
  for (const Case& c : cases) {
    const ErrorMapping m = map_error(c.code, c.msg);
    CAPTURE(c.code);
    CAPTURE(c.msg);
    CHECK(m.known);
    CHECK(m.reason == c.reason);
    CHECK(m.action == c.action);
  }
  const ErrorMapping unknown = map_error(-9999);
  CHECK_FALSE(unknown.known);
  CHECK(unknown.reason == RejectReason::VenueReject);
  CHECK(map_http_status(418).action == VenueAction::HardStop);
  CHECK(map_http_status(429).action == VenueAction::RateLimit);
  CHECK(map_http_status(429).reason == RejectReason::VenueRateLimit);
  CHECK(map_http_status(401).action == VenueAction::Fatal);
  CHECK(map_http_status(503).action == VenueAction::Backoff);
  CHECK(map_http_status(409).action == VenueAction::Reconcile);
  CHECK_FALSE(map_http_status(200).known);
}

TEST_CASE("binance.rest_decoder: exchangeInfo filters, server time, errors, listenKey") {
  ExchangeInfo info;
  const std::string err =
      decode_exchange_info(fastmm::test::fixture("binance/exchange_info.json"), info);
  REQUIRE(err.empty());
  REQUIRE(info.symbols.size() == 1);
  const SymbolFilters& f = info.symbols[0];
  CHECK(f.symbol == "BTCUSDT");
  CHECK(f.status == "TRADING");
  CHECK(f.base_asset == "BTC");
  CHECK(f.quote_asset == "USDT");
  CHECK(f.tick == Price::from_decimal("0.01").value());
  CHECK(f.step == Qty::from_decimal("0.00001").value());
  CHECK(f.min_qty == Qty::from_decimal("0.00001").value());
  CHECK(f.max_qty == Qty::from_int(9000));
  CHECK(f.min_notional == Notional::from_int(5));
  CHECK(f.max_notional == Notional::from_int(9000000));
  CHECK(f.post_only_allowed);
  CHECK(info.server_time_ms == 1789295119614);
  REQUIRE(info.rate_limits.size() == 4);
  CHECK(info.rate_limits[0].type == "REQUEST_WEIGHT");
  CHECK(info.rate_limits[0].limit == 6000);
  CHECK(info.rate_limits[0].window_ns() == 60'000'000'000LL);
  CHECK(info.rate_limits[1].type == "ORDERS");
  CHECK(info.rate_limits[1].window_ns() == 10'000'000'000LL);
  CHECK(info.rate_limits[1].limit == 50);
  std::int64_t t = 0;
  CHECK(decode_server_time(fastmm::test::fixture("binance/server_time.json"), t).empty());
  CHECK(t == 1789295119833);
  int code = 0;
  std::string msg;
  CHECK(decode_rest_error(fastmm::test::fixture("binance/error_rest.json"), code, msg));
  CHECK(code == -1102);
  CHECK(msg.starts_with("Mandatory parameter 'signature'"));
  CHECK_FALSE(decode_rest_error(fastmm::test::fixture("binance/server_time.json"), code, msg));
  std::string lk;
  CHECK(decode_listen_key(fastmm::test::fixture("binance/listen_key.json"), lk).empty());
  CHECK(lk.size() == 64);
  CHECK_FALSE(
      decode_exchange_info("{\"symbols\":[{\"symbol\":\"X\",\"filters\":[]}]}", info).empty());
}
