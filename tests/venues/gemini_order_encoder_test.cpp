// Gemini: signing (HMAC-SHA384 against RFC 4231), the WebSocket order requests, the REST payloads,
// the error map, the REST decoders on recorded and documented bodies, and the orders@account
// decoder on the documented events (https://developer.gemini.com/websocket/streams.md,
// https://developer.gemini.com/specs/openapi/rest.yaml, read 2026-09-30).
#include "fastmm/venues/gemini/gemini_order_encoder.hpp"

#include "venue_test_util.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/gemini/gemini_auth.hpp"
#include "fastmm/venues/gemini/gemini_error_map.hpp"
#include "fastmm/venues/gemini/gemini_private_parser.hpp"
#include "fastmm/venues/gemini/gemini_rest_decoder.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::gemini;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kGemini{1};

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(make_instrument("BTCGUSDPERP", 1, "BTC", "GUSD")));  // id 0
    REQUIRE(symbols.build(instruments));
  }
};

Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}
Price px(const char* s) {
  return Price::from_decimal(s).value();
}

// The header value `name` of a "Name: value\r\n" block.
std::string header(const std::string& block, const std::string& name) {
  const std::size_t p = block.find(name + ": ");
  if (p == std::string::npos) return {};
  const std::size_t start = p + name.size() + 2;
  return block.substr(start, block.find("\r\n", start) - start);
}

PrivateDecodeResult decode(GeminiPrivateParser& p, const std::string& frame, Scratch& s) {
  const PaddedJson j(frame);
  return p.decode(j.view(), Timestamp{1}, Cycles{}, s.span());
}

}  // namespace

TEST_CASE("gemini.auth: HMAC-SHA384 and the signed REST and WebSocket headers") {
  // RFC 4231 test case 2.
  CHECK(net::hmac_sha384_hex("Jefe", "what do ya want for nothing?").view() ==
        "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e"
        "8e2240ca5e69e2c78b3239ecfab21649");
  Signer signer(Credentials{"account-key", Secret<std::string>{"s3cret"}});
  REQUIRE(signer.usable());
  const std::string payload = R"({"request":"/v1/orders","nonce":1790730000})";
  const std::string h = signer.rest_headers(payload);
  CHECK(header(h, "X-GEMINI-APIKEY") == "account-key");
  CHECK(header(h, "X-GEMINI-PAYLOAD") == net::base64_encode(payload));
  CHECK(header(h, "X-GEMINI-SIGNATURE") ==
        std::string(net::hmac_sha384_hex("s3cret", net::base64_encode(payload)).view()));
  CHECK(header(h, "Content-Type") == "text/plain");
  CHECK(header(h, "Cache-Control") == "no-cache");
  // The WebSocket upgrade: the payload is base64 of the nonce's text.
  const std::string w = signer.ws_headers(1790730000);
  CHECK(header(w, "X-GEMINI-NONCE") == "1790730000");
  CHECK(header(w, "X-GEMINI-PAYLOAD") == net::base64_encode(std::string_view("1790730000")));
  CHECK(
      header(w, "X-GEMINI-SIGNATURE") ==
      std::string(net::hmac_sha384_hex("s3cret", net::base64_encode(std::string_view("1790730000")))
                      .view()));
  CHECK_FALSE(Signer(Credentials{"k", {}}).usable());
}

TEST_CASE("gemini.encoder: order.place, order.cancel, no amend") {
  Universe u;
  const GeminiOrderEncoder enc(u.symbols);
  char buf[kMaxRequestBytes];
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, kGemini);
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Sell;
  n.type = OrderType::PostOnly;
  n.price = px("83500.5");
  n.qty = qty("0.0012");
  std::size_t len = enc.encode_ws(*OrderCommand::from(n.hdr), {}, buf);
  CHECK(
      std::string_view(buf, len) ==
      R"({"id":"nfm000100000001","method":"order.place","params":{"symbol":"btcgusdperp","side":"SELL","type":"LIMIT","timeInForce":"MOC","price":"83500.5","quantity":"0.0012","clientOrderId":"fm000100000001"}})");
  n.type = OrderType::Limit;
  n.tif = TimeInForce::Ioc;
  n.side = Side::Buy;
  n.reduce_only = 1;  // no reduce-only on Gemini: sent as a plain order
  len = enc.encode_ws(*OrderCommand::from(n.hdr), {}, buf);
  CHECK(std::string_view(buf, len).find(R"("side":"BUY","type":"LIMIT","timeInForce":"IOC")") !=
        std::string_view::npos);
  CHECK(std::string_view(buf, len).find("reduce") == std::string_view::npos);
  n.tif = TimeInForce::Fok;
  len = enc.encode_ws(*OrderCommand::from(n.hdr), {}, buf);
  CHECK(std::string_view(buf, len).find(R"("timeInForce":"FOK")") != std::string_view::npos);
  n.type = OrderType::Market;
  n.tif = TimeInForce::Ioc;
  len = enc.encode_ws(*OrderCommand::from(n.hdr), {}, buf);
  CHECK(std::string_view(buf, len).find(R"("type":"MARKET","timeInForce":"IOC","quantity")") !=
        std::string_view::npos);

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, kGemini);
  c.cl_ord_id = n.cl_ord_id;
  len = enc.encode_ws(*OrderCommand::from(c.hdr), "73797746498585286", buf);
  CHECK(
      std::string_view(buf, len) ==
      R"({"id":"cfm000100000001","method":"order.cancel","params":{"orderId":73797746498585286}})");
  CHECK(enc.encode_ws(*OrderCommand::from(c.hdr), {}, buf) == 0);  // no venue id
  CHECK(enc.encode_ws(*OrderCommand::from(c.hdr), "12a", buf) == 0);

  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, InstrumentId{0}, kGemini);
  CHECK(enc.encode_ws(*OrderCommand::from(r.hdr), "1", buf) == 0);

  constexpr std::string_view kStreams[] = {"orders@account"};
  len = GeminiOrderEncoder::encode_subscribe("orders", kStreams, buf);
  CHECK(std::string_view(buf, len) ==
        R"({"id":"orders","method":"subscribe","params":["orders@account"]})");
  len = GeminiOrderEncoder::encode_method("time", "time", buf);
  CHECK(std::string_view(buf, len) == R"({"id":"time","method":"time"})");
}

TEST_CASE("gemini.encoder: REST payloads name their path and nonce") {
  RestRequest r = GeminiOrderEncoder::my_trades(1790730000, "btcgusdperp", 1790729000123, 500);
  CHECK(r.target == "/v1/mytrades");
  CHECK(
      r.payload ==
      R"({"request":"/v1/mytrades","nonce":1790730000,"symbol":"btcgusdperp","timestamp":1790729000123,"limit_trades":500})");
  r = GeminiOrderEncoder::funding_payments(1790730000, 1790720000000, 0);
  CHECK(r.path == "/v1/perpetuals/fundingPayment");
  CHECK(r.target == "/v1/perpetuals/fundingPayment?since=1790720000000");
  CHECK(r.payload == R"({"request":"/v1/perpetuals/fundingPayment","nonce":1790730000})");
  r = GeminiOrderEncoder::funding_payments(1, 10, 20);
  CHECK(r.target == "/v1/perpetuals/fundingPayment?since=10&to=20");
  r = GeminiOrderEncoder::cancel_order(1790730000, "106817811");
  CHECK(r.payload == R"({"request":"/v1/order/cancel","nonce":1790730000,"order_id":106817811})");
  CHECK(GeminiOrderEncoder::cancel_session(5).payload ==
        R"({"request":"/v1/order/cancel/session","nonce":5})");
  CHECK(GeminiOrderEncoder::heartbeat(5).payload == R"({"request":"/v1/heartbeat","nonce":5})");
  CHECK(GeminiOrderEncoder::positions(5).target == "/v1/positions");
  CHECK(GeminiOrderEncoder::active_orders(5).target == "/v1/orders");
}

TEST_CASE("gemini.error_map: reasons, WebSocket codes and statuses") {
  CHECK(map_reason("RateLimit").action == VenueAction::RateLimit);
  CHECK(map_reason("InvalidNonce").action == VenueAction::ResyncClock);
  CHECK(map_reason("InvalidSignature").action == VenueAction::Fatal);
  CHECK(map_reason("MissingRole").action == VenueAction::Fatal);
  CHECK(map_reason("RemoteAddressForbidden").action == VenueAction::HardStop);
  CHECK(map_reason("InsufficientFunds").reason == RejectReason::InsufficientBalance);
  CHECK(map_reason("LimitPriceOffTick").reason == RejectReason::InvalidTick);
  CHECK(map_reason("InvalidQuantity").reason == RejectReason::InvalidLot);
  CHECK(map_reason("DuplicateOrder").reason == RejectReason::DuplicateId);
  CHECK(map_reason("MakerOrCancelWouldTake").reason == RejectReason::PostOnlyWouldCross);
  CHECK(map_reason("OrderNotFound").action == VenueAction::Reconcile);
  CHECK_FALSE(map_reason("SomethingNew").known);
  CHECK(map_ws_code(-1003).action == VenueAction::RateLimit);
  CHECK(map_ws_code(-1002).action == VenueAction::Fatal);
  CHECK(map_ws_code(-1000).action == VenueAction::Backoff);
  CHECK(map_ws_code(-2010).action == VenueAction::None);
  CHECK(map_http_status(429).action == VenueAction::RateLimit);
  CHECK(map_http_status(403).action == VenueAction::Fatal);
  CHECK(map_http_status(503).action == VenueAction::Backoff);
  CHECK(is_expiry_reason("ImmediateOrCancelWouldPost"));
  CHECK_FALSE(is_expiry_reason(""));
}

TEST_CASE("gemini.rest_decoder: recorded symbol details, spot and perpetual") {
  SymbolDetails d;
  REQUIRE(decode_symbol_details(fastmm::test::fixture("gemini/symbol_details_btcgusdperp.json"), d)
              .empty());
  CHECK(d.symbol == "BTCGUSDPERP");
  CHECK(d.product_type == "swap");
  CHECK(d.contract_type == "linear");
  CHECK(d.base == "BTC");
  CHECK(d.quote == "GUSD");
  CHECK(d.collateral == "GUSD");
  CHECK(d.status == "open");
  CHECK(d.tick == px("0.5"));     // quote_increment
  CHECK(d.lot == qty("0.0001"));  // tick_size
  CHECK(d.min_qty == qty("0.0001"));
  REQUIRE(
      decode_symbol_details(fastmm::test::fixture("gemini/symbol_details_btcusd.json"), d).empty());
  CHECK(d.product_type == "spot");
  CHECK(d.tick == px("0.01"));
  CHECK(d.lot == qty("0.00000001"));  // "tick_size":1e-08 as a JSON number
  CHECK(d.min_qty == qty("0.00001"));
  CHECK(decode_symbol_details(R"({"result":"error","reason":"InvalidSymbol","message":"x"})", d)
            .find("InvalidSymbol") != std::string::npos);
}

TEST_CASE("gemini.rest_decoder: orders, positions, trades, funding, cancel results, errors") {
  std::vector<ActiveOrder> orders;
  REQUIRE(
      decode_active_orders(
          R"([{"order_id":"106817811","id":"106817811","symbol":"btcgusdperp","exchange":"gemini","avg_execution_price":"0","side":"buy","type":"exchange limit","timestamp":"1547220404","timestampms":1547220404836,"is_live":true,"is_cancelled":false,"executed_amount":"0.0002","remaining_amount":"0.0008","client_order_id":"fm000000000007","options":["maker-or-cancel"],"price":"83000.5","original_amount":"0.001"}])",
          orders)
          .empty());
  REQUIRE(orders.size() == 1);
  CHECK(orders[0].order_id == "106817811");
  CHECK(orders[0].client_order_id == "fm000000000007");
  CHECK(orders[0].price == px("83000.5"));
  CHECK(orders[0].original == qty("0.001"));
  CHECK(orders[0].executed == qty("0.0002"));

  std::vector<PositionRow> pos;
  REQUIRE(
      decode_positions(
          R"({"openPositions":[{"symbol":"btcgusdperp","instrument_type":"perp","quantity":"-0.0025","notional_value":"-208.5","realised_pnl":"0","unrealised_pnl":"1.2","average_cost":"83412.123456789","mark_price":"83400"}]})",
          pos)
          .empty());
  REQUIRE(pos.size() == 1);
  CHECK(pos[0].qty == qty("-0.0025"));
  CHECK(pos[0].avg_px == px("83412.12345679"));  // rounded to 8 decimals
  pos.clear();
  REQUIRE(decode_positions(R"([{"symbol":"btcgusdperp","instrument_type":"perp","quantity":"1"}])",
                           pos)
              .empty());  // the example's bare array
  CHECK(pos.size() == 1);

  std::vector<TradeRow> trades;
  REQUIRE(
      decode_trades(
          R"([{"price":"83633.00","amount":"0.0010","timestamp":1790729903,"timestampms":1790729903021,"type":"Sell","aggressor":false,"fee_currency":"GUSD","fee_amount":"0.0167","tid":1893456012054189,"order_id":"107317524","client_order_id":"fm000100000001","exchange":"gemini","is_auction_fill":false,"break":"","symbol":"BTCGUSDPERP"}])",
          trades)
          .empty());
  REQUIRE(trades.size() == 1);
  CHECK(trades[0].tid == "1893456012054189");
  CHECK_FALSE(trades[0].buy);
  CHECK_FALSE(trades[0].aggressor);
  CHECK(trades[0].fee == Notional::from_decimal("0.0167").value());
  CHECK(trades[0].time_ms == 1790729903021);

  std::vector<FundingRow> funding;
  REQUIRE(
      decode_funding(
          R"([{"eventType":"Hourly Funding Transfer","hourlyFundingTransfer":{"eventType":"Hourly Funding Transfer","timestamp":1683734406746,"assetCode":"GUSD","action":"Debit","quantity":{"currency":"GUSD","value":"4.78958"},"instrumentSymbol":"BTCGUSDPERP"}},{"eventType":"Hourly Funding Transfer","hourlyFundingTransfer":{"timestamp":1683738006746,"assetCode":"GUSD","action":"Credit","quantity":{"currency":"GUSD","value":"1.5"},"instrumentSymbol":"BTCGUSDPERP"}}])",
          funding)
          .empty());
  REQUIRE(funding.size() == 2);
  CHECK(funding[0].amount == Notional::from_decimal("-4.78958").value());
  CHECK(funding[1].amount == Notional::from_decimal("1.5").value());
  CHECK(funding[0].symbol == "BTCGUSDPERP");
  CHECK(funding[0].asset == "GUSD");

  std::size_t n = 0;
  std::vector<std::string> rejects;
  REQUIRE(
      decode_cancel_result(
          R"({"result":"ok","details":{"cancelledOrders":[330429106,330429079],"cancelRejects":[330429082]}})",
          n,
          rejects)
          .empty());
  CHECK(n == 2);
  CHECK(rejects == std::vector<std::string>{"330429082"});
  std::string reason;
  std::string message;
  CHECK(decode_error(
      R"({"result":"error","reason":"RateLimit","message":"slow down"})", reason, message));
  CHECK(reason == "RateLimit");
  CHECK_FALSE(decode_error(R"({"result":"ok"})", reason, message));
}

TEST_CASE("gemini.private_parser: the documented order events and replies") {
  Universe u;
  GeminiPrivateParser p(u.symbols, kGemini);
  Scratch s;
  // NEW: the ack, with the venue's id.
  PrivateDecodeResult r = decode(
      p,
      R"({"e":"orderUpdate","E":1759291847686856569,"s":"BTCGUSDPERP","i":73797746498585286,"c":"fm000100000001","S":"BUY","o":"LIMIT","X":"NEW","p":"83000.50","q":"0.0010","z":"0.0010","T":1759291847686856569})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& ack = s.as<OrderAckMsg>();
  CHECK(ack.hdr.type == EventType::OrderAck);
  CHECK(ack.hdr.instrument == InstrumentId{0});
  CHECK(ack.cl_ord_id == decode_cl_ord_id("fm000100000001").value());
  CHECK(ack.venue_order_id.view() == "73797746498585286");
  CHECK(ack.hdr.exch_ts == Timestamp{1759291847686856569});
  // PARTIALLY_FILLED: Z is this execution, z what is left.
  r = decode(
      p,
      R"({"e":"orderUpdate","E":1759291847700000000,"s":"BTCGUSDPERP","i":73797746498585286,"c":"fm000100000001","S":"BUY","o":"LIMIT","X":"PARTIALLY_FILLED","p":"83000.50","q":"0.0010","z":"0.0006","Z":"0.0004","L":"83000.50","a":"83000.50","t":1893456012054189,"m":true,"T":1759291847700000000})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& f = s.as<OrderFillMsg>();
  CHECK(f.hdr.type == EventType::OrderFill);
  CHECK(f.exec_id.view() == "1893456012054189");
  CHECK(f.qty == qty("0.0004"));
  CHECK(f.price == px("83000.5"));
  CHECK(f.leaves_qty == qty("0.0006"));
  CHECK(f.cum_qty == qty("0.0004"));
  CHECK(f.side == Side::Buy);
  CHECK(f.liquidity == Liquidity::Maker);
  CHECK(f.fee.is_zero());
  // FILLED with the fee; zero fields left out (z).
  r = decode(
      p,
      R"({"e":"orderUpdate","E":1,"s":"BTCGUSDPERP","i":73797746498585286,"c":"fm000100000001","S":"BUY","X":"FILLED","q":"0.0010","Z":"0.0006","L":"83000.00","t":1893456012054190,"n":"0.01","m":false,"T":2})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(s.as<OrderFillMsg>().leaves_qty.is_zero());
  CHECK(s.as<OrderFillMsg>().cum_qty == qty("0.001"));
  CHECK(s.as<OrderFillMsg>().fee == Notional::from_decimal("0.01").value());
  CHECK(s.as<OrderFillMsg>().liquidity == Liquidity::Taker);
  // CANCELED on request (the documented example: no side, quantities left out).
  r = decode(
      p,
      R"({"e":"orderUpdate","E":1759291847731455006,"s":"BTCGUSDPERP","i":73797746498585286,"c":"fm000100000001","X":"CANCELED","T":1759291847731455006})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(s.as<OrderCancelAckMsg>().hdr.type == EventType::OrderCancelAck);
  CHECK(s.as<OrderCancelAckMsg>().cum_qty.is_zero());
  // A post-only that would take: accepted, then CANCELED with the reason -> expired.
  r = decode(
      p,
      R"({"e":"orderUpdate","E":1,"s":"BTCGUSDPERP","i":5,"c":"fm000100000002","X":"CANCELED","r":"MakerOrCancelWouldTake","T":2})",
      s);
  CHECK(s.as<OrderExpiredMsg>().hdr.type == EventType::OrderExpired);
  // A fully filled IOC ends CANCELED; Z is the cumulative.
  r = decode(
      p,
      R"({"e":"orderUpdate","E":1,"s":"BTCGUSDPERP","i":6,"c":"fm000100000003","X":"CANCELED","Z":"0.0010","r":"ImmediateOrCancelWouldPost","T":2})",
      s);
  CHECK(s.as<OrderExpiredMsg>().cum_qty == qty("0.001"));
  // REJECTED.
  r = decode(
      p,
      R"({"e":"orderUpdate","E":1,"s":"BTCGUSDPERP","i":7,"c":"fm000100000004","X":"REJECTED","r":"InsufficientFunds","T":2})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(s.as<OrderRejectMsg>().reason == RejectReason::InsufficientBalance);
  CHECK(s.as<OrderRejectMsg>().text.view() == "InsufficientFunds");
  // A foreign client id: no FastMM id.
  r = decode(
      p,
      R"({"e":"orderUpdate","E":1,"s":"BTCGUSDPERP","i":8,"c":"my-order-1","X":"NEW","T":2})",
      s);
  CHECK_FALSE(s.as<OrderAckMsg>().cl_ord_id.valid());
  CHECK(p.stats().foreign_ids == 1);
  // Replies.
  r = decode(
      p, R"({"id":"nfm000100000001","status":200,"result":{"orderId":"73797746498585286"}})", s);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.control.id == "nfm000100000001");
  CHECK(r.control.order_id == "73797746498585286");
  r = decode(
      p,
      R"({"id":"nfm000100000005","status":400,"error":{"code":-2010,"msg":"InsufficientFunds"}})",
      s);
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.control.status == 400);
  CHECK(r.control.error_code == -2010);
  CHECK(r.control.msg == "InsufficientFunds");
  // A balance update is not an order event.
  CHECK(decode(p, R"({"e":"balanceUpdate","E":1,"u":1,"B":[{"a":"GUSD","f":"1","c":"1"}]})", s)
            .status == ParseStatus::Ignored);
}
