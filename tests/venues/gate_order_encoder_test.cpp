// Gate signing against an independent HMAC-SHA512 implementation (Python hmac/hashlib), golden
// WebSocket API and REST requests, the response decoder on the docs' examples, the REST decoders
// on recorded replies, and the error label map.
#include "fastmm/venues/gate/gate_order_encoder.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/gate/gate_error_map.hpp"
#include "fastmm/venues/gate/gate_rest_decoder.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::gate;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::padded_fixture;

namespace {

constexpr VenueId kGate{3};
constexpr std::int64_t kTs = 1791000000;  // Unix seconds

struct GateUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  InstrumentId nvda;
  GateUniverse() {
    Instrument n = make_instrument("NVDA_USDT", 3, "NVDA", "USDT");
    n.asset_class = AssetClass::Perpetual;
    n.lot = Qty::from_int(1);
    nvda = instruments.add(n).value();
    REQUIRE(symbols.build(instruments));
  }
};

Signer test_signer() {
  Credentials c;
  c.api_key = "test-key";
  c.secret.value = "test-secret";
  return Signer(c);
}

OutNewOrderMsg new_order(InstrumentId inst,
                         Side side,
                         OrderType type,
                         TimeInForce tif,
                         const char* price,
                         const char* qty,
                         std::uint64_t id = 1) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, inst, kGate);
  n.cl_ord_id = ClientOrderId{id};
  n.side = side;
  n.type = type;
  n.tif = tif;
  n.price = Price::from_decimal(price).value();
  n.qty = Qty::from_decimal(qty).value();
  return n;
}

}  // namespace

TEST_CASE("gate.auth: signatures match an independent HMAC-SHA512 implementation") {
  // Expected values computed with Python hmac/hashlib over the documented pre-sign strings:
  // METHOD\npath\nquery\nhex(SHA512(body))\nts, "channel=..&event=..&time=..", and
  // "api\nfutures.login\n\n<ts>".
  const Signer s = test_signer();
  CHECK(s.sign_rest("GET",
                    "/api/v4/futures/usdt/orders",
                    "status=open&contract=NVDA_USDT&limit=100",
                    {},
                    kTs)
            .view() ==
        "c3bffb55a805c8a1d0d534fc3fc2751f79434a80e3c95516a388e353b1d5edec2045907d52a960d4bfe544066"
        "2743f2cfabdd718508818172e11aa71f9be5b2f");
  const std::string body =
      R"({"contract":"NVDA_USDT","size":3,"price":"234.1","tif":"poc","text":"t-fm000000000001"})";
  CHECK(s.sign_rest("POST", "/api/v4/futures/usdt/orders", {}, body, kTs).view() ==
        "e38ee613b8db4bbf9dbdadf6778dc57504ed446b354230491e69cdcfc731e20613ed88329d205f9f60edae013"
        "1d92034a720085786298b8dc7703f9e5f7342de");
  CHECK(s.sign_ws_subscribe("futures.orders", "subscribe", kTs).view() ==
        "2ff6abe3aeb5b89715f895e56f9eefa9991ee3307a678057fa6413f6240ec959be8af61aa59a6329c129416c2"
        "c34157ee3409705ea2ad88b420bbab7f39659dc");
  CHECK(s.sign_ws_api("futures.login", {}, kTs).view() ==
        "1098f69bb0431f53e2a86a5dee10ca7d8bec8adc9da07a17c963dda7ff832315673bb51cd79e1969e983571a8"
        "1e0aa9605707123133cba04521ba5be03215239");
  CHECK(net::sha512_hex({}).view() ==
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d28"
        "77eec2f63b931bd47417a81a538327af927da3e");
  const std::string h = s.rest_headers("POST", "/api/v4/futures/usdt/orders", {}, body, kTs);
  CHECK(h ==
        "KEY: test-key\r\nTimestamp: 1791000000\r\n"
        "SIGN: e38ee613b8db4bbf9dbdadf6778dc57504ed446b354230491e69cdcfc731e20613ed88329d205f9f60e"
        "dae0131d92034a720085786298b8dc7703f9e5f7342de\r\n"
        "Content-Type: application/json\r\n");
  CHECK_FALSE(Credentials{}.usable());
}

TEST_CASE("gate.encoder: order_place / order_cancel / order_amend frames and the login") {
  GateUniverse u;
  const Signer s = test_signer();
  GateOrderEncoder enc(s, u.symbols);
  char buf[kMaxRequestBytes];

  OutNewOrderMsg n =
      new_order(u.nvda, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, "234.1", "3");
  std::size_t len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  CHECK(
      std::string_view(buf, len) ==
      R"({"time":1791000000,"channel":"futures.order_place","event":"api","payload":{"req_id":"nfm000000000001",)"
      R"("req_param":{"contract":"NVDA_USDT","size":3,"price":"234.1","tif":"poc","text":"t-fm000000000001"}}})");

  // A sell is a negative size; a market order is price 0 with tif ioc; reduce-only when asked.
  n = new_order(u.nvda, Side::Sell, OrderType::Market, TimeInForce::Ioc, "0", "1.5", 2);
  n.reduce_only = 1;
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  CHECK(
      std::string_view(buf, len).find(
          R"("req_param":{"contract":"NVDA_USDT","size":-1.5,"price":"0","tif":"ioc","text":"t-fm000000000002","reduce_only":true}})") !=
      std::string_view::npos);

  // Cancel by our text id until the venue named the order, then by the venue's id.
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, u.nvda, kGate);
  c.cl_ord_id = ClientOrderId{1};
  len = enc.encode_ws(*OrderCommand::from(c.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  CHECK(
      std::string_view(buf, len) ==
      R"({"time":1791000000,"channel":"futures.order_cancel","event":"api","payload":{"req_id":"cfm000000000001",)"
      R"("req_param":{"order_id":"t-fm000000000001"}}})");
  OrderShadow shadow{};
  shadow.instrument = u.nvda;
  shadow.side = Side::Buy;
  shadow.link_id = ClientOrderId{1};
  shadow.venue_id.assign("74046514");
  len = enc.encode_ws(*OrderCommand::from(c.hdr), &shadow, kTs, buf);
  REQUIRE(len > 0);
  CHECK(std::string_view(buf, len).find(R"("req_param":{"order_id":"74046514"})") !=
        std::string_view::npos);

  // Amend: the venue's id, the signed total size and the new price.
  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, u.nvda, kGate);
  r.cl_ord_id = ClientOrderId{5};
  r.orig_cl_ord_id = ClientOrderId{1};
  r.price = Price::from_decimal("233.9").value();
  r.qty = Qty::from_decimal("4").value();
  len = enc.encode_ws(*OrderCommand::from(r.hdr), &shadow, kTs, buf);
  REQUIRE(len > 0);
  CHECK(
      std::string_view(buf, len) ==
      R"({"time":1791000000,"channel":"futures.order_amend","event":"api","payload":{"req_id":"rfm000000000005",)"
      R"("req_param":{"order_id":"74046514","size":4,"price":"233.9"}}})");
  CHECK(enc.encode_ws(*OrderCommand::from(r.hdr), nullptr, kTs, buf) == 0);

  len = enc.encode_ws_login(kTs, "login-t", buf);
  REQUIRE(len > 0);
  CHECK(
      std::string_view(buf, len) ==
      R"({"time":1791000000,"channel":"futures.login","event":"api","payload":{"api_key":"test-key",)"
      R"("signature":"1098f69bb0431f53e2a86a5dee10ca7d8bec8adc9da07a17c963dda7ff832315673bb51cd79e1969e983571a81e0aa9605707123133cba04521ba5be03215239",)"
      R"("timestamp":"1791000000","req_id":"login-t"}})");

  const std::string_view payload[2] = {"110284739", "!all"};
  len = enc.encode_subscribe("futures.orders", payload, kTs, buf);
  REQUIRE(len > 0);
  CHECK(
      std::string_view(buf, len) ==
      R"({"time":1791000000,"channel":"futures.orders","event":"subscribe","payload":["110284739","!all"],)"
      R"("auth":{"method":"api_key","KEY":"test-key",)"
      R"("SIGN":"2ff6abe3aeb5b89715f895e56f9eefa9991ee3307a678057fa6413f6240ec959be8af61aa59a6329c129416c2c34157ee3409705ea2ad88b420bbab7f39659dc"}})");

  len = GateOrderEncoder::encode_ping(kTs, buf);
  CHECK(std::string_view(buf, len) == R"({"time":1791000000,"channel":"futures.ping"})");
}

TEST_CASE("gate.encoder: REST requests and their signed headers") {
  GateUniverse u;
  const Signer s = test_signer();
  GateOrderEncoder enc(s, u.symbols);
  RestRequest rr;

  OutNewOrderMsg n =
      new_order(u.nvda, Side::Buy, OrderType::PostOnly, TimeInForce::Gtc, "234.1", "3");
  REQUIRE(enc.encode_rest(*OrderCommand::from(n.hdr), nullptr, rr));
  CHECK(rr.method == "POST");
  CHECK(rr.path == "/api/v4/futures/usdt/orders");
  CHECK(
      rr.body ==
      R"({"contract":"NVDA_USDT","size":3,"price":"234.1","tif":"poc","text":"t-fm000000000001"})");
  CHECK(rr.is_order);
  // The headers sign the exact method, path, query and body.
  const std::string h = enc.rest_headers(rr, kTs);
  CHECK(h.find("SIGN: e38ee613b8db4bbf9dbdadf6778dc57504ed446b354230491e69cdcfc731e206") !=
        std::string::npos);

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, u.nvda, kGate);
  c.cl_ord_id = ClientOrderId{1};
  REQUIRE(enc.encode_rest(*OrderCommand::from(c.hdr), nullptr, rr));
  CHECK(rr.method == "DELETE");
  CHECK(rr.target() == "/api/v4/futures/usdt/orders/t-fm000000000001");
  CHECK(rr.body.empty());

  OrderShadow shadow{};
  shadow.side = Side::Sell;
  shadow.link_id = ClientOrderId{1};
  shadow.venue_id.assign("74046514");
  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, u.nvda, kGate);
  r.cl_ord_id = ClientOrderId{5};
  r.orig_cl_ord_id = ClientOrderId{1};
  r.price = Price::from_decimal("240").value();
  r.qty = Qty::from_decimal("2").value();
  REQUIRE(enc.encode_rest(*OrderCommand::from(r.hdr), &shadow, rr));
  CHECK(rr.method == "PUT");
  CHECK(rr.target() == "/api/v4/futures/usdt/orders/74046514");
  CHECK(rr.body == R"({"size":-2,"price":"240"})");

  REQUIRE(enc.encode_rest_cancel_all("NVDA_USDT", rr));
  CHECK(rr.method == "DELETE");
  CHECK(rr.target() == "/api/v4/futures/usdt/orders?contract=NVDA_USDT");
  CHECK_FALSE(enc.encode_rest_cancel_all({}, rr));

  enc.encode_rest_open_orders("NVDA_USDT", 100, 0, rr);
  CHECK(rr.target() == "/api/v4/futures/usdt/orders?status=open&contract=NVDA_USDT&limit=100");
  enc.encode_rest_open_orders("NVDA_USDT", 100, 200, rr);
  CHECK(rr.query == "status=open&contract=NVDA_USDT&limit=100&offset=200");
  enc.encode_rest_positions(rr);
  CHECK(rr.target() == "/api/v4/futures/usdt/positions?holding=true");
  enc.encode_rest_accounts(rr);
  CHECK(rr.target() == "/api/v4/futures/usdt/accounts");
  enc.encode_rest_my_trades({}, 1791000000, 1791086400, 1000, 1000, rr);
  CHECK(rr.target() ==
        "/api/v4/futures/usdt/"
        "my_trades_timerange?from=1791000000&to=1791086400&limit=1000&offset=1000");
  enc.encode_rest_fee("NVDA_USDT", rr);
  CHECK(rr.target() == "/api/v4/futures/usdt/fee?contract=NVDA_USDT");
  REQUIRE(enc.encode_rest_countdown(30, {}, rr));
  CHECK(rr.method == "POST");
  CHECK(rr.target() == "/api/v4/futures/usdt/countdown_cancel_all");
  CHECK(rr.body == R"({"timeout":30})");
  GateOrderEncoder usd1(s, u.symbols, "usd1");
  usd1.encode_rest_accounts(rr);
  CHECK(rr.target() == "/api/v4/futures/usd1/accounts");
}

TEST_CASE("gate.decoder: WebSocket API replies (docs examples) and REST order replies") {
  GateResponseDecoder dec;
  ApiResponse r;
  {
    const auto f = padded_fixture("gate/api_login_ok.json");
    REQUIRE(dec.decode_ws(f.view(), r) == ParseStatus::Ok);
    CHECK(r.request_id == "login-t");
    CHECK(r.channel == "futures.login");
    CHECK(r.status == 200);
    CHECK(r.success);
    CHECK_FALSE(r.ack);
    CHECK(r.uid == "110284739");
  }
  {
    const auto f = padded_fixture("gate/api_login_error.json");
    REQUIRE(dec.decode_ws(f.view(), r) == ParseStatus::Ok);
    CHECK_FALSE(r.success);
    CHECK(r.status == 401);
    CHECK(r.label == "INVALID_KEY");
    CHECK(r.message == "Invalid key provided");
  }
  {
    const auto f = padded_fixture("gate/api_order_place_ack.json");
    REQUIRE(dec.decode_ws(f.view(), r) == ParseStatus::Ok);
    CHECK(r.ack);
    CHECK(r.request_id == "nfm000000000001");
    CHECK(r.remain == 99);
    CHECK(r.limit == 100);
    CHECK(r.reset_ms == 1736408263764);
  }
  {
    const auto f = padded_fixture("gate/api_order_place_ok.json");
    REQUIRE(dec.decode_ws(f.view(), r) == ParseStatus::Ok);
    CHECK_FALSE(r.ack);
    CHECK(r.success);
    CHECK(r.order_id == "74046514");
    CHECK(r.text == "t-fm000000000001");
    CHECK(r.order_status == "open");
    CHECK(r.size == Qty::from_int(3));
    CHECK(r.left == Qty::from_int(3));
    CHECK(r.bid);
    CHECK(r.price == Price::from_decimal("234.1").value());
    CHECK(r.response_time_ms == 1681195484360);
    const auto req = parse_request_id(r.request_id);
    REQUIRE(req);
    CHECK(req->first == RequestKind::New);
    CHECK(req->second == ClientOrderId{1});
  }
  {
    const auto f = padded_fixture("gate/api_order_place_error.json");
    REQUIRE(dec.decode_ws(f.view(), r) == ParseStatus::Ok);
    CHECK_FALSE(r.success);
    CHECK(r.status == 400);
    CHECK(r.label == "ORDER_POC_IMMEDIATE");
    CHECK(map_label(r.label).reason == RejectReason::PostOnlyWouldCross);
  }
  {
    const auto f = padded_fixture("gate/api_rate_limited.json");
    REQUIRE(dec.decode_ws(f.view(), r) == ParseStatus::Ok);
    CHECK(r.status == 429);
    CHECK(r.label == "TOO_MANY_REQUESTS");
    CHECK(r.reset_ms == 1677816785084);
    CHECK(map_label(r.label).action == VenueAction::RateLimit);
  }
  {
    const auto f = padded_fixture("gate/api_order_cancel_ok.json");
    REQUIRE(dec.decode_ws(f.view(), r) == ParseStatus::Ok);
    CHECK(r.success);
    CHECK(r.order_status == "finished");
    CHECK(r.finish_as == "cancelled");
    CHECK(r.size - r.left == Qty::from_int(1));
  }
  // Channel pushes and control frames are not API replies.
  {
    const auto f = padded_fixture("gate/book_ticker.json");
    CHECK(dec.decode_ws(f.view(), r) == ParseStatus::Ignored);
    const auto p = padded_fixture("gate/pong.json");
    CHECK(dec.decode_ws(p.view(), r) == ParseStatus::Ignored);
  }
  {
    const auto f = padded_fixture("gate/rest_order_created.json");
    REQUIRE(dec.decode_rest(f.view(), 201, r) == ParseStatus::Ok);
    CHECK(r.success);
    CHECK(r.order_id == "74046514");
    CHECK(r.size == Qty::from_int(3));
    CHECK(r.create_time_ms == 1681195484462);
    const auto e = padded_fixture("gate/rest_error_not_found.json");
    REQUIRE(dec.decode_rest(e.view(), 404, r) == ParseStatus::Ok);
    CHECK_FALSE(r.success);
    CHECK(r.label == "ORDER_NOT_FOUND");
    CHECK(map_label(r.label).reason == RejectReason::VenueUnknownOrder);
    CHECK(map_label(r.label).action == VenueAction::Reconcile);
  }
}

TEST_CASE("gate.rest_decoder: contracts, account, positions, open orders, trades, fees, time") {
  std::vector<ContractInfo> contracts;
  REQUIRE(decode_contracts(fastmm::test::fixture("gate/rest_contracts.json"), contracts).empty());
  CHECK(contracts.size() == 3);
  ContractInfo nvda;
  REQUIRE(decode_contract(fastmm::test::fixture("gate/rest_contract_nvda.json"), nvda).empty());
  CHECK(nvda.name == "NVDA_USDT");
  CHECK(nvda.type == "direct");
  CHECK(nvda.status == "trading");
  CHECK(nvda.tick == Price::from_decimal("0.01").value());
  CHECK(nvda.min_size == Qty::from_int(1));
  CHECK(nvda.max_size == Qty::from_int(1000000));
  CHECK(nvda.quanto_multiplier.is_positive());
  CHECK(nvda.funding_interval_s == 28800);
  CHECK(nvda.maker_fee_rate == doctest::Approx(-0.0001));
  CHECK_FALSE(nvda.in_delisting);

  AccountInfo a;
  REQUIRE(decode_account(fastmm::test::fixture("gate/rest_accounts.json"), a).empty());
  CHECK(a.user == "110284739");
  CHECK(a.currency == "USDT");
  CHECK_FALSE(a.in_dual_mode);
  CHECK(a.fields.total.is_zero());
  const std::string acct2 =
      R"({"user":42,"total":"100.5","available":"80","order_margin":"5.5","position_margin":"15","unrealised_pnl":"-1.25","maintenance_margin":"0.75","currency":"USDT","in_dual_mode":true})";
  REQUIRE(decode_account(acct2, a).empty());
  CHECK(a.user == "42");
  CHECK(a.in_dual_mode);
  CHECK(a.fields.free == Notional::from_decimal("80").value());
  CHECK(a.fields.locked == Notional::from_decimal("20.5").value());
  CHECK(a.fields.total == Notional::from_decimal("100.5").value());
  CHECK(a.fields.equity == Notional::from_decimal("99.25").value());
  CHECK(a.fields.maintenance == Notional::from_decimal("0.75").value());

  std::vector<PositionRecord> positions;
  REQUIRE(decode_positions(fastmm::test::fixture("gate/rest_positions.json"), positions).empty());
  REQUIRE(positions.size() == 1);
  CHECK(positions[0].contract == "NVDA_USDT");
  CHECK(positions[0].size == -Qty::from_int(1));
  CHECK(positions[0].mode == "single");
  CHECK(positions[0].entry_price == Price::from_decimal("234.1").value());
  positions.clear();
  REQUIRE(decode_positions("[]", positions).empty());
  CHECK(positions.empty());

  std::vector<OpenOrderRecord> orders;
  REQUIRE(decode_open_orders(fastmm::test::fixture("gate/rest_open_orders.json"), orders).empty());
  REQUIRE(orders.size() == 2);
  CHECK(orders[0].id == "74046514");
  CHECK(orders[0].text == "t-fm000000000001");
  CHECK(orders[0].size == Qty::from_int(3));
  CHECK(orders[0].left == Qty::from_int(2));
  CHECK(orders[0].price == Price::from_decimal("234.1").value());
  CHECK(orders[0].create_time_ms == 1681195484462);
  CHECK(orders[1].size == -Qty::from_int(5));
  CHECK(orders[1].text == "web");

  std::vector<TradeRecord> trades;
  REQUIRE(decode_my_trades(fastmm::test::fixture("gate/rest_my_trades.json"), trades).empty());
  REQUIRE(trades.size() == 2);
  CHECK(trades[0].id == "3335260");
  CHECK(trades[0].order_id == "88");
  CHECK(trades[0].size == -Qty::from_int(2));
  CHECK_FALSE(trades[0].maker);
  CHECK(trades[0].time_ms == 1791009985301);
  CHECK(trades[1].maker);
  CHECK(trades[1].fee == Notional::from_decimal("0.04682").value());
  CHECK(trades[1].text == "t-fm000000000001");

  std::vector<std::pair<std::string, FeeRates>> fees;
  REQUIRE(decode_fees(fastmm::test::fixture("gate/rest_fee.json"), fees).empty());
  REQUIRE(fees.size() == 1);
  CHECK(fees[0].first == "NVDA_USDT");
  CHECK(fees[0].second.maker == doctest::Approx(0.0002));
  CHECK(fees[0].second.taker == doctest::Approx(0.0005));

  std::int64_t ms = 0;
  REQUIRE(decode_server_time(fastmm::test::fixture("gate/rest_order_book.json"), ms).empty());
  CHECK(ms == 1791009940824);
  REQUIRE(decode_server_time(R"({"server_time":1791009940824})", ms).empty());
  CHECK(ms == 1791009940824);

  std::string label;
  std::string msg;
  CHECK(decode_error(fastmm::test::fixture("gate/rest_error_not_found.json"), label, msg));
  CHECK(label == "ORDER_NOT_FOUND");
  CHECK_FALSE(decode_error("[]", label, msg));
  CHECK_FALSE(
      decode_contracts(fastmm::test::fixture("gate/rest_error_not_found.json"), contracts).empty());
}

TEST_CASE("gate.error_map: documented labels") {
  CHECK(map_label("INVALID_SIGNATURE").action == VenueAction::Fatal);
  CHECK(map_label("IP_FORBIDDEN").action == VenueAction::Fatal);
  CHECK(map_label("REQUEST_EXPIRED").action == VenueAction::ResyncClock);
  CHECK(map_label("TOO_MANY_REQUESTS").reason == RejectReason::VenueRateLimit);
  CHECK(map_label("SERVER_ERROR").action == VenueAction::Backoff);
  CHECK(map_label("INSUFFICIENT_AVAILABLE").reason == RejectReason::InsufficientBalance);
  CHECK(map_label("ORDER_POC_IMMEDIATE").reason == RejectReason::PostOnlyWouldCross);
  CHECK(map_label("CONTRACT_NOT_FOUND").action == VenueAction::DisableInstrument);
  CHECK(map_label("SIZE_TOO_SMALL").reason == RejectReason::InvalidLot);
  CHECK(map_label("SIZE_TOO_LARGE").reason == RejectReason::MaxOrderQty);
  CHECK(map_label("PRICE_TOO_DEVIATED").reason == RejectReason::PriceCollar);
  CHECK(map_label("INVALID_PRECISION", "price precision").reason == RejectReason::InvalidTick);
  CHECK(map_label("INVALID_PRECISION", "size").reason == RejectReason::InvalidLot);
  CHECK(map_label("ORDER_FINISHED").reason == RejectReason::VenueUnknownOrder);
  CHECK(map_label("ORDER_FINISHED").action == VenueAction::None);
  CHECK(map_label("POSITION_DUAL_MODE").action == VenueAction::Fatal);
  CHECK(map_label("DUPLICATE_REQUEST").reason == RejectReason::DuplicateId);
  CHECK_FALSE(map_label("SOMETHING_NEW").known);
  CHECK(map_label("SOMETHING_NEW").reason == RejectReason::VenueReject);
  CHECK(map_http_status(429).action == VenueAction::RateLimit);
  CHECK(map_http_status(401).action == VenueAction::Fatal);
  CHECK(map_http_status(503).action == VenueAction::Backoff);
  CHECK(finish_as_expired("ioc"));
  CHECK(finish_as_expired("stp"));
  CHECK_FALSE(finish_as_expired("cancelled"));
  CHECK_FALSE(finish_as_expired("filled"));
}
