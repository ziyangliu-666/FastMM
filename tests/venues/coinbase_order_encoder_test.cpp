// Coinbase Exchange signing, wire ids and times, REST requests, the error map and the REST
// decoders. Signatures are checked against vectors computed with Python's hmac module
// (base64(HMAC-SHA256(base64decode(secret), prehash))); the request formats follow
// https://docs.cdp.coinbase.com/exchange/rest-api/authentication and the orders reference (read
// 2026-09-30).
#include "fastmm/venues/coinbase/coinbase_order_encoder.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/coinbase/coinbase_auth.hpp"
#include "fastmm/venues/coinbase/coinbase_error_map.hpp"
#include "fastmm/venues/coinbase/coinbase_rest_decoder.hpp"
#include "fastmm/venues/coinbase/coinbase_wire.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::coinbase;

namespace {

// base64 of the bytes 0..63.
constexpr const char* kSecret =
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0+Pw==";

Credentials creds() {
  Credentials c;
  c.api_key = "key-1";
  c.secret.value = kSecret;
  c.passphrase.value = "pass-1";
  return c;
}

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(venues::test::make_instrument("BTC-USD", 1, "BTC", "USD")));  // id 0
    REQUIRE(symbols.build(instruments));
  }
};

OrderCommand new_cmd(OrderType type, TimeInForce tif) {
  OrderCommand c;
  c.kind = OrderCommandKind::New;
  c.instrument = InstrumentId{0};
  c.venue = VenueId{1};
  c.cl_ord_id = make_cl_ord_id(1, 1);
  c.side = Side::Buy;
  c.type = type;
  c.tif = tif;
  c.price = Price::from_decimal("60000.1").value();
  c.qty = Qty::from_decimal("0.001").value();
  return c;
}

}  // namespace

TEST_CASE("coinbase.auth: signatures match an independent HMAC implementation") {
  const Signer s(creds());
  REQUIRE(s.usable());
  CHECK(s.sign_rest("1789299703.123",
                    "GET",
                    "/orders?status=open&status=pending&status=active&limit=1000",
                    "")
            .view() == "3teBlL3TL0GqMxBvSIgY+X+IG+CXlsBGti7kI6fczBc=");
  CHECK(s.sign_ws("1789299703").view() == "01mHniQBpEBdSPvOaISYnOAnToNRJyvhSaeCcay+f6Y=");
  // The headers sign the exact bytes sent: the encoder's body, the timestamp in the header.
  Universe u;
  const CoinbaseOrderEncoder enc(u.symbols);
  OrderRequest rq;
  REQUIRE(enc.encode_new(new_cmd(OrderType::PostOnly, TimeInForce::Gtc), rq));
  const std::string h = s.rest_headers(1789299703123, "POST", rq.target(), rq.payload());
  CHECK(h ==
        "CB-ACCESS-KEY: key-1\r\nCB-ACCESS-SIGN: oWzgHF0nCM2tas9B1lOwkZ+YDHQlxYDZZkAiy0aBaWQ=\r\n"
        "CB-ACCESS-TIMESTAMP: 1789299703.123\r\nCB-ACCESS-PASSPHRASE: pass-1\r\n"
        "Content-Type: application/json\r\n");
  CHECK(epoch_seconds(1789299703005, true).view() == "1789299703.005");
  CHECK(epoch_seconds(1789299703999, false).view() == "1789299703");
  // A secret that is not base64 is a configuration error, not a missing key.
  Credentials bad = creds();
  bad.secret.value = "not base64!";
  CHECK_FALSE(Signer(bad).usable());
  CHECK(Signer(bad).secret_malformed());
  CHECK_FALSE(Signer(Credentials{}).usable());
  CHECK_FALSE(Signer(Credentials{}).secret_malformed());
}

TEST_CASE("coinbase.wire: client_oid is a lowercase UUIDv4 that round-trips FastMM's id") {
  const ClientOrderId id = make_cl_ord_id(0x1234, 0x89abcdef);
  const ClientOid oid = encode_client_oid(id);
  CHECK(oid.view() == "666d0000-0000-4000-8000-123489abcdef");
  // Version 4 and variant 1 (8, 9, a or b) where RFC 4122 puts them.
  CHECK(oid.view()[14] == '4');
  CHECK(oid.view()[19] == '8');
  CHECK(decode_client_oid(oid.view()) == id);
  CHECK(decode_client_oid("d50ec974-76a2-454b-66f135b1ea8c") == std::nullopt);
  CHECK(decode_client_oid("666d0000-0000-4000-8000-123489ABCDEF") == std::nullopt);
  CHECK(decode_client_oid("666d0000-0000-4000-8000-000000000000") == std::nullopt);
  CHECK(decode_client_oid("") == std::nullopt);
  // Order ids: the low 64 bits of the UUID.
  const auto k = order_key("d50ec984-77a8-460a-b958-66f114b0de9b");
  REQUIRE(k);
  CHECK(k->value == 0xb95866f114b0de9bULL);
  CHECK_FALSE(order_key("d50ec98477a8460ab95866f114b0de9b"));
  CHECK_FALSE(order_key("312"));
}

TEST_CASE("coinbase.wire: RFC 3339 times with any fraction") {
  CHECK(parse_time_ns("2026-09-30T01:41:50.644756Z") == 1790732510644756000);
  CHECK(parse_time_ns("2026-09-30T01:41:50Z") == 1790732510000000000);
  CHECK(parse_time_ns("2014-11-07T08:19:27.028459Z") == 1415348367028459000);
  CHECK(parse_time_ns("2026-09-30 01:41:50.201625597Z") == 1790732510201625597);
  CHECK(parse_time_ns("2026-09-30T01:41:50") == -1);
  CHECK(parse_time_ns("yesterday") == -1);
  CHECK(format_time_ms(1790732510644).view() == "2026-09-30T01:41:50.644Z");
  CHECK(parse_time_ns(format_time_ms(1790732510644).view()) == 1790732510644000000);
}

TEST_CASE("coinbase.encoder: POST /orders bodies and the cancel path are byte-exact") {
  Universe u;
  const CoinbaseOrderEncoder enc(u.symbols);
  OrderRequest rq;
  REQUIRE(enc.encode_new(new_cmd(OrderType::PostOnly, TimeInForce::Gtc), rq));
  CHECK(rq.method == "POST");
  CHECK(rq.target() == "/orders");
  CHECK(
      rq.payload() ==
      R"({"client_oid":"666d0000-0000-4000-8000-000100000001","product_id":"BTC-USD","side":"buy","type":"limit","price":"60000.1","size":"0.001","time_in_force":"GTC","post_only":true,"stp":"dc"})");
  OrderCommand ioc = new_cmd(OrderType::Limit, TimeInForce::Ioc);
  ioc.side = Side::Sell;
  REQUIRE(CoinbaseOrderEncoder(u.symbols, Stp::Co).encode_new(ioc, rq));
  CHECK(
      rq.payload() ==
      R"({"client_oid":"666d0000-0000-4000-8000-000100000001","product_id":"BTC-USD","side":"sell","type":"limit","price":"60000.1","size":"0.001","time_in_force":"IOC","stp":"co"})");
  REQUIRE(enc.encode_new(new_cmd(OrderType::Limit, TimeInForce::Fok), rq));
  CHECK(rq.payload().find(R"("time_in_force":"FOK")") != std::string_view::npos);
  REQUIRE(enc.encode_new(new_cmd(OrderType::Market, TimeInForce::Ioc), rq));
  CHECK(
      rq.payload() ==
      R"({"client_oid":"666d0000-0000-4000-8000-000100000001","product_id":"BTC-USD","side":"buy","type":"market","size":"0.001","stp":"dc"})");

  OrderCommand cancel;
  cancel.kind = OrderCommandKind::Cancel;
  cancel.instrument = InstrumentId{0};
  cancel.cl_ord_id = make_cl_ord_id(1, 1);
  REQUIRE(enc.encode_cancel(cancel, rq));
  CHECK(rq.method == "DELETE");
  CHECK(rq.target() == "/orders/client:666d0000-0000-4000-8000-000100000001?product_id=BTC-USD");
  CHECK(rq.payload().empty());
  // Wrong kinds and unknown instruments are refused.
  CHECK_FALSE(enc.encode_cancel(new_cmd(OrderType::Limit, TimeInForce::Gtc), rq));
  CHECK_FALSE(enc.encode_new(cancel, rq));
  OrderCommand unknown = new_cmd(OrderType::Limit, TimeInForce::Gtc);
  unknown.instrument = InstrumentId{7};
  CHECK_FALSE(enc.encode_new(unknown, rq));

  CHECK(CoinbaseOrderEncoder::open_orders_path({}) ==
        "/orders?status=open&status=pending&status=active&limit=1000");
  CHECK(CoinbaseOrderEncoder::open_orders_path("abc") ==
        "/orders?status=open&status=pending&status=active&limit=1000&after=abc");
  CHECK(CoinbaseOrderEncoder::fills_path("BTC-USD", 1790732510644, 0, {}, 100) ==
        "/fills?product_id=BTC-USD&start_date=2026-09-30T01:41:50.644Z&limit=100");
  CHECK(CoinbaseOrderEncoder::fills_path("BTC-USD", 1790732510644, 1790732570644, "77", 100) ==
        "/fills?product_id=BTC-USD&start_date=2026-09-30T01:41:50.644Z&end_date=2026-09-30T01:42:"
        "50.644Z&after=77&limit=100");
  CHECK(CoinbaseOrderEncoder::cancel_all_path("BTC-USD") == "/orders?product_id=BTC-USD");
  CHECK(CoinbaseOrderEncoder::order_path("d50ec984-77a8-460a-b958-66f114b0de9b") ==
        "/orders/d50ec984-77a8-460a-b958-66f114b0de9b");
}

TEST_CASE("coinbase.error_map: HTTP statuses and messages") {
  CHECK(map_error(200).reason == RejectReason::None);
  CHECK(map_error(429, "Private rate limit exceeded").action == VenueAction::RateLimit);
  CHECK(map_error(429).reason == RejectReason::VenueRateLimit);
  CHECK(map_error(400, "request timestamp expired").action == VenueAction::ResyncClock);
  CHECK(map_error(401, "invalid signature").action == VenueAction::Fatal);
  CHECK(map_error(401).action == VenueAction::Fatal);
  CHECK(map_error(403, "Forbidden").action == VenueAction::Fatal);
  CHECK(map_error(400, "Insufficient funds").reason == RejectReason::InsufficientBalance);
  const ErrorMapping tick = map_error(400, "price is too accurate. Smallest unit is 0.01000000");
  CHECK(tick.reason == RejectReason::InvalidTick);
  CHECK(tick.action == VenueAction::DisableInstrument);
  CHECK(map_error(400, "size is too small. Minimum size is 0.00000001").reason ==
        RejectReason::InvalidLot);
  CHECK(map_error(400, "funds is too small").reason == RejectReason::BelowMinNotional);
  CHECK(map_error(404, "NotFound").reason == RejectReason::VenueUnknownOrder);
  CHECK(map_error(400, "Order already done").reason == RejectReason::VenueUnknownOrder);
  CHECK(map_error(503, "Service Unavailable").action == VenueAction::Backoff);
  CHECK(map_error(400, "something new").known);
  CHECK_FALSE(map_error(418, "teapot").known);
  CHECK(map_reject_reason("post only") == RejectReason::PostOnlyWouldCross);
  CHECK(map_reject_reason("") == RejectReason::VenueReject);
}

TEST_CASE("coinbase.rest: recorded products and time, orders, fills, ids and errors") {
  ProductInfo p;
  REQUIRE(decode_product(fastmm::test::fixture("coinbase/product_btc_usd.json"), p).empty());
  CHECK(p.id == "BTC-USD");
  CHECK(p.base == "BTC");
  CHECK(p.quote == "USD");
  CHECK(p.status == "online");
  CHECK(p.tick == Price::from_decimal("0.01").value());
  CHECK(p.lot == Qty::from_decimal("0.00000001").value());
  CHECK(p.min_size.is_zero());  // no longer sent
  CHECK(p.min_funds == Notional::from_int(1));
  CHECK_FALSE(p.trading_disabled);
  ProductInfo sp;
  REQUIRE(
      decode_product(fastmm::test::fixture("coinbase/product_btc_usd_sandbox.json"), sp).empty());
  CHECK(sp.tick == p.tick);
  CHECK(decode_product(R"({"message":"NotFound"})", p) == "products: NotFound");

  std::int64_t ms = 0;
  REQUIRE(decode_server_time(fastmm::test::fixture("coinbase/server_time.json"), ms).empty());
  CHECK(ms == 1790733824122);

  std::vector<OrderRow> orders;
  REQUIRE(
      decode_orders(
          R"([{"id":"a9625b04-fc66-4999-a876-543c3684d702","price":"10.00000000","size":"1.00000000","product_id":"BTC-USD","profile_id":"p","side":"buy","type":"limit","time_in_force":"GTC","post_only":true,"created_at":"2020-03-11T20:48:46.622Z","fill_fees":"0","filled_size":"0.25000000","executed_value":"0","status":"open","settled":false,"client_oid":"666d0000-0000-4000-8000-000100000007"}])",
          orders)
          .empty());
  REQUIRE(orders.size() == 1);
  CHECK(orders[0].id == "a9625b04-fc66-4999-a876-543c3684d702");
  CHECK(orders[0].client_oid == "666d0000-0000-4000-8000-000100000007");
  CHECK(orders[0].filled_size == Qty::from_decimal("0.25").value());
  CHECK(orders[0].status == "open");
  OrderRow rejected;
  REQUIRE(
      decode_order(
          R"({"id":"b9625b04-fc66-4999-a876-543c3684d702","price":"1","size":"1","product_id":"BTC-USD","side":"buy","type":"limit","post_only":true,"status":"rejected","reject_reason":"post only"})",
          rejected)
          .empty());
  CHECK(rejected.reject_reason == "post only");
  CHECK(decode_order(R"({"message":"Insufficient funds"})", rejected) ==
        "order: Insufficient funds");

  std::vector<FillRow> fills;
  REQUIRE(
      decode_fills(
          R"([{"created_at":"2019-11-21T01:38:23.878Z","trade_id":78098253,"product_id":"BTC-USD","order_id":"41473628-db2c-464e-b9f4-82df7e4fb4f4","user_id":"u","profile_id":"p","liquidity":"T","price":"8087.38000000","size":"0.00601800","fee":"0.2433492642000000","side":"sell","settled":true,"usd_volume":"48.66985284","funding_currency":"USDC"}])",
          fills)
          .empty());
  REQUIRE(fills.size() == 1);
  CHECK(fills[0].trade_id == 78098253);
  CHECK(fills[0].liquidity == "T");
  CHECK(fills[0].fee == Notional::from_decimal("0.24334926").value());
  CHECK(fills[0].time_ms == 1574300303878);

  std::vector<std::string> ids;
  REQUIRE(decode_ids(R"(["a","b"])", ids).empty());
  REQUIRE(decode_ids(R"("c")", ids).empty());
  CHECK(ids == std::vector<std::string>{"a", "b", "c"});
  CHECK(error_message(R"({"message":"Invalid Price"})") == "Invalid Price");
  CHECK(error_message("<html>").empty());
}

TEST_CASE("coinbase.wire: balances with 16 decimals are truncated to 8") {
  CHECK(parse_balance("0.0000000000000000") == Notional{});
  CHECK(parse_balance("1.2345678999999999") == Notional::from_decimal("1.23456789").value());
  CHECK(parse_balance("102030.99") == Notional::from_decimal("102030.99").value());
  CHECK(parse_balance("0") == Notional{});
  CHECK_FALSE(parse_balance("1.23456789x"));
  CHECK_FALSE(parse_balance("1.2345678900000000x"));
  CHECK_FALSE(parse_balance(""));
}

TEST_CASE("coinbase.rest: accounts, the documented example") {
  // The example of apiAccount, GET /accounts (https://docs.cdp.coinbase.com/api-reference/
  // exchange-api/rest-api/accounts/get-all-account-profile, read 2026-09-30), and a BTC account
  // holding for an order in the same shape.
  std::vector<AccountRow> rows;
  REQUIRE(
      decode_accounts(
          R"([{"id":"7fd0abc0-e5ad-4cbb-8d54-f2b3f43364da","currency":"USD","balance":"0.0000000000000000","hold":"0.0000000000000000","available":"0","profile_id":"8058d771-2d88-4f0f-ab6e-299c153d4308","trading_enabled":true},{"id":"d50ec984-77a8-460a-b958-66f114b0de9b","currency":"BTC","balance":"1.2500000000000000","hold":"0.3000000000000001","available":"0.9499999999999999","profile_id":"8058d771-2d88-4f0f-ab6e-299c153d4308","trading_enabled":true,"pending_deposit":"0","display_name":"BTC"}])",
          rows)
          .empty());
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].id == "7fd0abc0-e5ad-4cbb-8d54-f2b3f43364da");
  CHECK(rows[0].currency == "USD");
  CHECK(rows[0].available.is_zero());
  CHECK(rows[0].hold.is_zero());
  CHECK(rows[0].trading_enabled);
  CHECK(rows[1].currency == "BTC");
  CHECK(rows[1].balance == Notional::from_decimal("1.25").value());
  CHECK(rows[1].hold == Notional::from_decimal("0.3").value());
  CHECK(rows[1].available == Notional::from_decimal("0.94999999").value());
  CHECK(decode_accounts(R"({"message":"Unauthorized."})", rows) == "accounts: Unauthorized.");
  CHECK(decode_accounts(R"([{"id":"x","currency":"BTC","hold":"1","available":"abc"}])", rows) ==
        "accounts: bad available or hold for BTC");
}
