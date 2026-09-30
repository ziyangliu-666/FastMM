// Coinbase Advanced Trade: CDP JWTs, order requests, REST replies, the error maps, the public-feed
// decoder and feed on frames recorded from production (2026-09-30,
// tests/fixtures/coinbase/fixtures.meta.json), and the user channel on the envelopes recorded from
// the live account and on orders in the AsyncAPI schema's shape.
#include "fake_venue_util.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/coinbase/advanced_auth.hpp"
#include "fastmm/venues/coinbase/advanced_error_map.hpp"
#include "fastmm/venues/coinbase/advanced_md_feed.hpp"
#include "fastmm/venues/coinbase/advanced_md_parser.hpp"
#include "fastmm/venues/coinbase/advanced_rest.hpp"
#include "fastmm/venues/coinbase/advanced_user_parser.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::coinbase;
using fastmm::venues::test::Collected;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kVenue{1};

// The RFC 6979 A.2.5 P-256 key, as a .env file holds it: one line, "\n" escapes, quoted.
constexpr const char* kEnvPem =
    "\"-----BEGIN EC PRIVATE "
    "KEY-----\\nMHcCAQEEIMmvqdhFunUWa1whV2ex1pNOUMPbNuibEnuKYisSD2choAoGCCqGSM49"
    "\\nAwEHoUQDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7Yfps5mliLmDyn7Z5A/"
    "4QCLi8\\nmaQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ=="
    "\\n-----END EC PRIVATE KEY-----\\n\"";
constexpr const char* kPublicPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7\n"
    "Yfps5mliLmDyn7Z5A/4QCLi8maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==\n"
    "-----END PUBLIC KEY-----\n";
constexpr const char* kKeyName = "organizations/org-1/apiKeys/key-1";

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(venues::test::make_instrument("BTC-USD", 1, "BTC", "USD")));  // 0
    REQUIRE(instruments.add(venues::test::make_instrument("ETH-USD", 1, "ETH", "USD")));  // 1
    REQUIRE(symbols.build(instruments));
  }
};

std::string unb64url(std::string_view s) {
  std::string out(s.size(), '\0');
  const std::size_t n = net::base64url_decode(
      s, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(out.data()), out.size()));
  REQUIRE(n != SIZE_MAX);
  out.resize(n);
  return out;
}

std::vector<std::string> recorded_frames() {
  const std::string raw = fastmm::test::fixture("coinbase/advanced_md_stream.jsonl");
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t end = raw.find('\n', pos);
    if (end == std::string::npos) end = raw.size();
    const std::string line = raw.substr(pos, end - pos);
    pos = end + 1;
    const std::size_t tab = line.find('\t');
    if (tab != std::string::npos) out.push_back(line.substr(tab + 1));
  }
  return out;
}

OrderCommand new_cmd(OrderType type, TimeInForce tif) {
  OrderCommand c;
  c.kind = OrderCommandKind::New;
  c.instrument = InstrumentId{0};
  c.venue = kVenue;
  c.cl_ord_id = make_cl_ord_id(1, 1);
  c.side = Side::Buy;
  c.type = type;
  c.tif = tif;
  c.price = Price::from_decimal("60000.1").value();
  c.qty = Qty::from_decimal("0.001").value();
  return c;
}

std::string user_order(const char* status,
                       const char* cum,
                       const char* tif = "GOOD_UNTIL_CANCELLED",
                       const char* client = "fm000100000001") {
  return std::string(
             R"({"channel":"user","client_id":"","timestamp":"2026-09-30T02:37:14.089808906Z","sequence_num":7,"events":[{"type":"update","orders":[{"avg_price":"60000.1","cancel_reason":"","client_order_id":")") +
         client + R"(","completion_percentage":"0","cumulative_quantity":")" + cum +
         R"(","leaves_quantity":"0.3","limit_price":"60000.1","number_of_fills":"1","order_id":"11111111-2222-4333-8444-555555555555","order_side":"SELL","order_type":"LIMIT","post_only":"true","product_id":"BTC-USD","reject_reason":"","status":")" +
         status + R"(","time_in_force":")" + tif +
         R"(","total_fees":"0","creation_time":"2026-09-30T02:37:14.0Z"}],"positions":{}}]})";
}

}  // namespace

TEST_CASE("coinbase_advanced.auth: an ES256 JWT that verifies, from a .env-style key") {
  CHECK(normalize_pem("'a\\nb'") == "a\nb\n");
  CHECK(normalize_pem("a\nb\n") == "a\nb\n");
  CdpCredentials c;
  c.key_name = kKeyName;
  c.private_key.value = kEnvPem;
  const CdpJwtSigner signer(c);
  REQUIRE(signer.usable());
  CHECK(CdpJwtSigner::rest_uri("GET",
                               "api.coinbase.com",
                               "/api/v3/brokerage/orders/historical/batch?order_status=OPEN") ==
        "GET api.coinbase.com/api/v3/brokerage/orders/historical/batch");
  const std::string jwt = signer.sign_with_nonce(1790000000,
                                                 "GET api.coinbase.com/api/v3/brokerage/accounts",
                                                 "00112233445566778899aabbccddeeff");
  const std::size_t d1 = jwt.find('.');
  const std::size_t d2 = jwt.rfind('.');
  REQUIRE(d1 != std::string::npos);
  REQUIRE(d2 > d1);
  CHECK(
      unb64url(std::string_view(jwt).substr(0, d1)) ==
      R"({"alg":"ES256","kid":"organizations/org-1/apiKeys/key-1","nonce":"00112233445566778899aabbccddeeff","typ":"JWT"})");
  CHECK(
      unb64url(std::string_view(jwt).substr(d1 + 1, d2 - d1 - 1)) ==
      R"({"sub":"organizations/org-1/apiKeys/key-1","iss":"cdp","nbf":1790000000,"exp":1790000120,"uri":"GET api.coinbase.com/api/v3/brokerage/accounts"})");
  const std::string sig = unb64url(std::string_view(jwt).substr(d2 + 1));
  const net::EcdsaP256Key pub = net::EcdsaP256Key::from_public_pem(kPublicPem);
  CHECK(pub.verify(std::string_view(jwt).substr(0, d2),
                   std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(sig.data()),
                                                 sig.size())));
  // A WebSocket JWT has no uri; every JWT has a fresh nonce.
  const std::string ws = signer.sign(1790000000, {});
  CHECK(unb64url(std::string_view(ws).substr(ws.find('.') + 1, ws.rfind('.') - ws.find('.') - 1))
            .find("uri") == std::string::npos);
  CHECK(signer.sign(1790000000, "x") != signer.sign(1790000000, "x"));
  // Not an EC P-256 key: configuration error.
  CdpCredentials bad = c;
  bad.private_key.value = fastmm::test::fixture("binance/ed25519-test-private.pem");
  CHECK(CdpJwtSigner(bad).key_malformed());
  CHECK_FALSE(CdpJwtSigner(bad).usable());
  CHECK(CdpJwtSigner(bad).sign(1, "x").empty());
  CHECK_FALSE(CdpJwtSigner(CdpCredentials{}).key_malformed());
}

TEST_CASE("coinbase_advanced.encoder: order bodies and request paths are byte-exact") {
  Universe u;
  const AdvancedOrderEncoder enc(u.symbols);
  OrderRequest rq;
  REQUIRE(enc.encode_new(new_cmd(OrderType::PostOnly, TimeInForce::Gtc), rq));
  CHECK(rq.method == "POST");
  CHECK(rq.target() == "/api/v3/brokerage/orders");
  CHECK(
      rq.payload() ==
      R"({"client_order_id":"fm000100000001","product_id":"BTC-USD","side":"BUY","order_configuration":{"limit_limit_gtc":{"base_size":"0.001","limit_price":"60000.1","post_only":true}}})");
  OrderCommand ioc = new_cmd(OrderType::Limit, TimeInForce::Ioc);
  ioc.side = Side::Sell;
  REQUIRE(enc.encode_new(ioc, rq));
  CHECK(
      rq.payload() ==
      R"({"client_order_id":"fm000100000001","product_id":"BTC-USD","side":"SELL","order_configuration":{"sor_limit_ioc":{"base_size":"0.001","limit_price":"60000.1"}}})");
  REQUIRE(enc.encode_new(new_cmd(OrderType::Limit, TimeInForce::Fok), rq));
  CHECK(rq.payload().find(R"({"limit_limit_fok":{)") != std::string_view::npos);
  REQUIRE(enc.encode_new(new_cmd(OrderType::Limit, TimeInForce::Gtc), rq));
  CHECK(rq.payload().find(R"({"limit_limit_gtc":{"base_size":"0.001","limit_price":"60000.1"}})") !=
        std::string_view::npos);
  REQUIRE(enc.encode_new(new_cmd(OrderType::Market, TimeInForce::Ioc), rq));
  CHECK(rq.payload().find(R"("order_configuration":{"market_market_ioc":{"base_size":"0.001"}})") !=
        std::string_view::npos);
  OrderCommand cancel = new_cmd(OrderType::Limit, TimeInForce::Gtc);
  cancel.kind = OrderCommandKind::Cancel;
  CHECK_FALSE(enc.encode_new(cancel, rq));

  const std::vector<std::string> ids = {"a", "b"};
  CHECK(AdvancedOrderEncoder::cancel_body(ids) == R"({"order_ids":["a","b"]})");
  CHECK(AdvancedOrderEncoder::cancel_path() == "/api/v3/brokerage/orders/batch_cancel");
  const std::vector<std::string> products = {"BTC-USD", "ETH-USD"};
  CHECK(AdvancedOrderEncoder::open_orders_path(products, {}) ==
        "/api/v3/brokerage/orders/historical/batch?order_status=OPEN&product_ids=BTC-USD&"
        "product_ids=ETH-USD&limit=250");
  CHECK(AdvancedOrderEncoder::open_orders_path(products, "c1").ends_with("&cursor=c1"));
  CHECK(AdvancedOrderEncoder::fills_path(products, 1790732510644, 0, {}, 100) ==
        "/api/v3/brokerage/orders/historical/fills?product_ids=BTC-USD&product_ids=ETH-USD&"
        "limit=100&start_sequence_timestamp=2026-09-30T01:41:50.644Z");
  CHECK(AdvancedOrderEncoder::order_fills_path("o-1") ==
        "/api/v3/brokerage/orders/historical/fills?order_ids=o-1&limit=100");
  CHECK(AdvancedOrderEncoder::order_path("o-1") == "/api/v3/brokerage/orders/historical/o-1");
  CHECK(AdvancedOrderEncoder::product_path("BTC-USD") ==
        "/api/v3/brokerage/market/products/BTC-USD");
}

TEST_CASE("coinbase_advanced.rest: recorded product and time, orders, fills, replies") {
  ProductInfo p;
  REQUIRE(decode_adv_product(fastmm::test::fixture("coinbase/advanced_product_btc_usd.json"), p)
              .empty());
  CHECK(p.id == "BTC-USD");
  CHECK(p.base == "BTC");
  CHECK(p.quote == "USD");
  CHECK(p.status == "online");
  CHECK(p.tick == Price::from_decimal("0.01").value());
  CHECK(p.lot == Qty::from_decimal("0.00000001").value());
  CHECK(p.min_size == Qty::from_decimal("0.00000001").value());
  CHECK(p.min_funds == Notional::from_int(1));
  CHECK_FALSE(p.trading_disabled);
  CHECK(decode_adv_product(R"({"error":"NOT_FOUND","message":"product not found"})", p) ==
        "products: product not found");
  std::int64_t ms = 0;
  REQUIRE(decode_adv_time(fastmm::test::fixture("coinbase/advanced_time.json"), ms).empty());
  CHECK(ms == 1790735849298);

  CreateReply c;
  REQUIRE(
      decode_create_reply(
          R"({"success":true,"success_response":{"order_id":"11111111-2222-4333-8444-555555555555","product_id":"BTC-USD","side":"BUY","client_order_id":"fm000100000001"},"order_configuration":{}})",
          c)
          .empty());
  CHECK(c.success);
  CHECK(c.order_id == "11111111-2222-4333-8444-555555555555");
  CreateReply f;
  REQUIRE(
      decode_create_reply(
          R"({"success":false,"error_response":{"error":"INSUFFICIENT_FUND","message":"Insufficient balance in source account","error_details":"","preview_failure_reason":"PREVIEW_INSUFFICIENT_FUND","new_order_failure_reason":"UNKNOWN_FAILURE_REASON"}})",
          f)
          .empty());
  CHECK_FALSE(f.success);
  CHECK(f.failure_reason == "INSUFFICIENT_FUND");
  CHECK(f.message == "Insufficient balance in source account");
  CreateReply po;
  REQUIRE(
      decode_create_reply(
          R"({"success":false,"error_response":{"error":"UNKNOWN_FAILURE_REASON","message":"","new_order_failure_reason":"INVALID_LIMIT_PRICE_POST_ONLY"}})",
          po)
          .empty());
  CHECK(po.failure_reason == "INVALID_LIMIT_PRICE_POST_ONLY");

  std::vector<CancelResult> results;
  REQUIRE(
      decode_cancel_results(
          R"({"results":[{"success":true,"failure_reason":"UNKNOWN_CANCEL_FAILURE_REASON","order_id":"o-1"},{"success":false,"failure_reason":"UNKNOWN_CANCEL_ORDER","order_id":"o-2"}]})",
          results)
          .empty());
  REQUIRE(results.size() == 2);
  CHECK(results[0].success);
  CHECK(results[1].failure_reason == "UNKNOWN_CANCEL_ORDER");

  std::vector<AdvOrderRow> orders;
  std::string cursor;
  bool has_next = false;
  REQUIRE(
      decode_adv_orders(
          R"({"orders":[{"order_id":"o-1","product_id":"BTC-USD","side":"SELL","client_order_id":"fm000000000007","status":"OPEN","order_configuration":{"limit_limit_gtc":{"base_size":"0.3","limit_price":"60000.1","post_only":true}},"filled_size":"0.1"}],"sequence":"0","has_next":true,"cursor":"c-2"})",
          orders,
          cursor,
          has_next)
          .empty());
  REQUIRE(orders.size() == 1);
  CHECK(orders[0].price == Price::from_decimal("60000.1").value());
  CHECK(orders[0].size == Qty::from_decimal("0.3").value());
  CHECK(orders[0].filled_size == Qty::from_decimal("0.1").value());
  CHECK(has_next);
  CHECK(cursor == "c-2");
  // The live account's empty answer (2026-09-30).
  orders.clear();
  REQUIRE(
      decode_adv_orders(
          R"({"orders":[], "sequence":"0", "has_next":false, "cursor":"", "proof_token_required":false})",
          orders,
          cursor,
          has_next)
          .empty());
  CHECK(orders.empty());
  AdvOrderRow one;
  REQUIRE(decode_adv_order(
              R"({"order":{"order_id":"o-9","client_order_id":"x","product_id":"BTC-USD"}})", one)
              .empty());
  CHECK(one.order_id == "o-9");

  std::vector<AdvFillRow> fills;
  REQUIRE(
      decode_adv_fills(
          R"({"fills":[{"entry_id":"e","trade_id":"t-1","order_id":"o-1","trade_time":"2026-09-30T01:41:50.644Z","trade_type":"FILL","price":"60000.1","size":"0.1","commission":"0.024000040000000","product_id":"BTC-USD","sequence_timestamp":"2026-09-30T01:41:50.645Z","liquidity_indicator":"MAKER","size_in_quote":false,"user_id":"u","side":"SELL","retail_portfolio_id":"p"}],"cursor":"c"})",
          fills,
          cursor)
          .empty());
  REQUIRE(fills.size() == 1);
  CHECK(fills[0].trade_id == "t-1");
  CHECK(fills[0].commission == Notional::from_decimal("0.02400004").value());
  CHECK(fills[0].liquidity == "MAKER");
  CHECK(fills[0].time_ms == 1790732510644);
  std::size_t n = 0;
  REQUIRE(decode_adv_accounts(
              R"({"accounts":[{"uuid":"a"},{"uuid":"b"}],"has_next":false,"cursor":"","size":2})",
              n,
              has_next)
              .empty());
  CHECK(n == 2);
  CHECK(adv_error_message(R"({"error":"UNAUTHORIZED","code":16,"message":"Unauthorized"})") ==
        "Unauthorized");
}

TEST_CASE("coinbase_advanced.rest: accounts to spot balances, the spec's example and paging") {
  CHECK(AdvancedOrderEncoder::accounts_path({}) == "/api/v3/brokerage/accounts?limit=250");
  CHECK(AdvancedOrderEncoder::accounts_path("789100") ==
        "/api/v3/brokerage/accounts?limit=250&cursor=789100");
  // An account made of the field examples of the Account and GetAccountsResponse schemas (OpenAPI
  // spec, List Accounts, read 2026-09-30), then a USD account of the same shape, and the accounts
  // spot orders cannot draw on: a vault, a US derivatives and an INTX one.
  std::vector<AdvBalanceRow> rows;
  std::string cursor;
  bool has_next = false;
  REQUIRE(
      decode_adv_balances(
          R"({"accounts":[{"uuid":"8bfc20d7-f7c6-4422-bf07-8243ca4169fe","name":"BTC Wallet","currency":"BTC","available_balance":{"value":"1.23","currency":"BTC"},"default":false,"active":true,"created_at":"2021-05-31T09:59:59Z","updated_at":"2021-05-31T09:59:59Z","deleted_at":"2021-05-31T09:59:59Z","type":"FIAT","ready":true,"hold":{"value":"1.23","currency":"BTC"},"retail_portfolio_id":"b87a2d3f-8a1e-49b3-a4ea-402d8c389aca","platform":"ACCOUNT_PLATFORM_CONSUMER"},)"
          R"({"uuid":"u-2","name":"USD Wallet","currency":"USD","available_balance":{"value":"250.1234567891","currency":"USD"},"default":true,"active":true,"type":"ACCOUNT_TYPE_FIAT","ready":true,"hold":{"value":"0","currency":"USD"},"platform":"ACCOUNT_PLATFORM_CONSUMER"},)"
          R"({"uuid":"u-3","currency":"BTC","available_balance":{"value":"5","currency":"BTC"},"type":"ACCOUNT_TYPE_VAULT","hold":{"value":"0","currency":"BTC"},"platform":"ACCOUNT_PLATFORM_CONSUMER"},)"
          R"({"uuid":"u-4","currency":"USD","available_balance":{"value":"7","currency":"USD"},"type":"ACCOUNT_TYPE_FIAT","hold":{"value":"0","currency":"USD"},"platform":"ACCOUNT_PLATFORM_CFM_CONSUMER"},)"
          R"({"uuid":"u-5","currency":"USDC","available_balance":{"value":"9","currency":"USDC"},"type":"ACCOUNT_TYPE_CRYPTO","hold":{"value":"0","currency":"USDC"},"platform":"ACCOUNT_PLATFORM_INTX"}],"has_next":true,"cursor":"789100","size":5})",
          rows,
          cursor,
          has_next)
          .empty());
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].currency == "BTC");
  CHECK(rows[0].available == Notional::from_decimal("1.23").value());
  CHECK(rows[0].hold == Notional::from_decimal("1.23").value());
  CHECK(rows[1].currency == "USD");
  CHECK(rows[1].available == Notional::from_decimal("250.12345678").value());  // truncated
  CHECK(rows[1].hold.is_zero());
  CHECK(has_next);
  CHECK(cursor == "789100");
  // The last page appends; an account without a hold object holds nothing.
  REQUIRE(
      decode_adv_balances(
          R"({"accounts":[{"uuid":"u-6","currency":"ETH","available_balance":{"value":"0.5","currency":"ETH"},"type":"ACCOUNT_TYPE_CRYPTO"}],"has_next":false,"cursor":"","size":1})",
          rows,
          cursor,
          has_next)
          .empty());
  REQUIRE(rows.size() == 3);
  CHECK(rows[2].currency == "ETH");
  CHECK(rows[2].hold.is_zero());
  CHECK_FALSE(has_next);
  CHECK(decode_adv_balances(
            R"({"error":"UNAUTHORIZED","message":"Unauthorized"})", rows, cursor, has_next) ==
        "accounts: Unauthorized");
  CHECK(
      decode_adv_balances(
          R"({"accounts":[{"uuid":"u","currency":"BTC","available_balance":{"value":"1e3"}}],"has_next":false})",
          rows,
          cursor,
          has_next) == "accounts: bad available_balance or hold for BTC");

  // The live account's reply (2026-09-30, read only; account and portfolio ids replaced): two
  // empty fiat accounts, no crypto account at all.
  rows.clear();
  REQUIRE(decode_adv_balances(
              fastmm::test::fixture("coinbase/advanced_accounts.json"), rows, cursor, has_next)
              .empty());
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].currency == "SGD");
  CHECK(rows[1].currency == "USD");
  CHECK(rows[1].available.is_zero());
  CHECK(rows[1].hold.is_zero());
  CHECK_FALSE(has_next);
  CHECK(cursor.empty());
}

TEST_CASE("coinbase_advanced.error_map: order, cancel and HTTP failures") {
  CHECK(map_order_failure("INSUFFICIENT_FUND").reason == RejectReason::InsufficientBalance);
  CHECK(map_order_failure("INVALID_LIMIT_PRICE_POST_ONLY").reason ==
        RejectReason::PostOnlyWouldCross);
  CHECK(map_order_failure("INVALID_PRICE_PRECISION").action == VenueAction::DisableInstrument);
  CHECK(map_order_failure("INVALID_SIZE_PRECISION").reason == RejectReason::InvalidLot);
  CHECK(map_order_failure("DUPLICATE_CLIENT_ORDER_ID").reason == RejectReason::DuplicateId);
  CHECK(map_order_failure("GEOFENCING_RESTRICTION").action == VenueAction::Fatal);
  CHECK(map_order_failure("UNTRADABLE_PRODUCT").reason == RejectReason::InstrumentDisabled);
  CHECK_FALSE(map_order_failure("").known);
  CHECK(map_cancel_failure("UNKNOWN_CANCEL_ORDER").reason == RejectReason::VenueUnknownOrder);
  CHECK(map_cancel_failure("ORDER_IS_FULLY_FILLED").reason == RejectReason::VenueUnknownOrder);
  CHECK(map_http(401).action == VenueAction::Fatal);
  CHECK(map_http(429).action == VenueAction::RateLimit);
  CHECK(map_http(503).action == VenueAction::Backoff);
  CHECK(map_http(404).reason == RejectReason::VenueUnknownOrder);
}

TEST_CASE("coinbase_advanced.md_parser: recorded level2, trades, heartbeats and subscriptions") {
  Universe u;
  AdvancedMdParser p(u.symbols, kVenue);
  Scratch s;
  std::size_t snaps = 0;
  std::size_t deltas = 0;
  std::size_t trades = 0;
  std::uint64_t last_seq = 0;
  bool first = true;
  for (const std::string& f : recorded_frames()) {
    const PaddedJson j(f);
    const AdvancedMdResult r = p.decode(j.view(), Timestamp{1}, Cycles{2}, s.span());
    REQUIRE(r.status != ParseStatus::Malformed);
    REQUIRE(r.status != ParseStatus::UnknownSymbol);
    REQUIRE(r.has_sequence);
    if (!first) CHECK(r.sequence == last_seq + 1);  // one connection, nothing lost
    first = false;
    last_seq = r.sequence;
    if (r.status != ParseStatus::Ok) continue;
    std::uint32_t off = 0;
    for (std::uint32_t i = 0; i < r.count; ++i) {
      const auto* h = reinterpret_cast<const EventHeader*>(s.buf + off);
      off += h->len;
      if (h->type == EventType::BookSnapshot) {
        ++snaps;
        const auto& m = *reinterpret_cast<const BookDeltaMsg*>(h);
        CHECK(m.bid_count == 150);
        CHECK(m.ask_count == 150);
        CHECK(m.bids()[0].price > m.bids()[1].price);
        CHECK(m.asks()[0].price < m.asks()[1].price);
        CHECK(m.bids()[0].price < m.asks()[0].price);
      } else if (h->type == EventType::BookDelta) {
        ++deltas;
      } else if (h->type == EventType::Trade) {
        ++trades;
      }
    }
  }
  CHECK(snaps >= 1);
  CHECK(deltas > 20);
  CHECK(trades > 0);
  CHECK(p.stats().malformed == 0);

  const PaddedJson trade(
      R"({"channel":"market_trades","client_id":"","timestamp":"2026-09-30T01:41:27.834Z","sequence_num":5,"events":[{"type":"update","trades":[{"product_id":"ETH-USD","trade_id":"846149798","price":"2668.35","size":"0.03710506","time":"2026-09-30T01:41:25.375232Z","side":"SELL"},{"product_id":"ETH-USD","trade_id":"846149799","price":"2668.36","size":"1","time":"2026-09-30T01:41:25.375232Z","side":"BUY"}]}]})");
  AdvancedMdResult r = p.decode(trade.view(), Timestamp{1}, Cycles{2}, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 2);
  const auto& t0 = s.as<TradeMsg>();
  CHECK(t0.hdr.instrument == InstrumentId{1});
  CHECK(t0.aggressor == Side::Buy);  // "the maker's side": a SELL maker, a buying taker
  CHECK(t0.trade_id == 846149798);
  CHECK(reinterpret_cast<const TradeMsg*>(s.buf + sizeof(TradeMsg))->aggressor == Side::Sell);
  const PaddedJson snap_trades(
      R"({"channel":"market_trades","client_id":"","timestamp":"2026-09-30T01:41:27.834Z","sequence_num":6,"events":[{"type":"snapshot","trades":[{"product_id":"ETH-USD","trade_id":"1","price":"1","size":"1","time":"2026-09-30T01:41:25Z","side":"BUY"}]}]})");
  r = p.decode(snap_trades.view(), Timestamp{1}, Cycles{2}, s.span());
  CHECK(r.status == ParseStatus::Ignored);  // trades from before the subscription
  CHECK(r.sequence == 6);
  const PaddedJson hb(
      R"({"channel":"heartbeats","client_id":"","timestamp":"2026-09-30T02:37:15.05Z","sequence_num":3,"events":[{"current_time":"2026-09-30 02:37:15.057778606 +0000 UTC m=+21697.195984854","heartbeat_counter":21697}]})");
  CHECK(p.decode(hb.view(), Timestamp{1}, Cycles{2}, s.span()).control ==
        AdvancedControl::Heartbeat);
  const PaddedJson err(R"({"type":"error","message":"failure to subscribe"})");
  r = p.decode(err.view(), Timestamp{1}, Cycles{2}, s.span());
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.msg == "failure to subscribe");
  // An unsorted snapshot side: Malformed, naming its product.
  const PaddedJson unsorted(
      R"({"channel":"l2_data","client_id":"","timestamp":"2026-09-30T01:41:25Z","sequence_num":9,"events":[{"type":"snapshot","product_id":"BTC-USD","updates":[{"side":"bid","event_time":"2026-09-30T01:41:24Z","price_level":"100","new_quantity":"1"},{"side":"bid","event_time":"2026-09-30T01:41:24Z","price_level":"101","new_quantity":"1"}]}]})");
  r = p.decode(unsorted.view(), Timestamp{1}, Cycles{2}, s.span());
  CHECK(r.status == ParseStatus::Malformed);
  CHECK(r.instrument == InstrumentId{0});
  const PaddedJson unknown(
      R"({"channel":"l2_data","client_id":"","timestamp":"2026-09-30T01:41:25Z","sequence_num":10,"events":[{"type":"update","product_id":"SOL-USD","updates":[{"side":"bid","event_time":"2026-09-30T01:41:24Z","price_level":"100","new_quantity":"1"}]}]})");
  CHECK(p.decode(unknown.view(), Timestamp{1}, Cycles{2}, s.span()).status ==
        ParseStatus::UnknownSymbol);
  const PaddedJson bad_side(
      R"({"channel":"l2_data","client_id":"","timestamp":"2026-09-30T01:41:25Z","sequence_num":11,"events":[{"type":"update","product_id":"BTC-USD","updates":[{"side":"ask","event_time":"2026-09-30T01:41:24Z","price_level":"100","new_quantity":"1"}]}]})");
  CHECK(p.decode(bad_side.view(), Timestamp{1}, Cycles{2}, s.span()).status ==
        ParseStatus::Malformed);
}

TEST_CASE("coinbase_advanced.md_feed: the recorded session syncs, a sequence gap resyncs") {
  Universe u;
  RecordingSink sink(32U << 20);
  std::vector<InstrumentId> requests;
  AdvancedMdFeed feed(
      u.symbols,
      kVenue,
      sink.sink,
      ResubscribeRequester{[](void* ctx, InstrumentId id) noexcept {
                             static_cast<std::vector<InstrumentId>*>(ctx)->push_back(id);
                           },
                           &requests});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  const auto subs = feed.subscription_payloads();
  REQUIRE(subs.size() == 3);
  CHECK(subs[0] == R"({"type":"subscribe","product_ids":["BTC-USD"],"channel":"level2"})");
  CHECK(subs[1] == R"({"type":"subscribe","product_ids":["BTC-USD"],"channel":"market_trades"})");
  CHECK(subs[2] == R"({"type":"subscribe","channel":"heartbeats"})");
  feed.on_connected();
  const std::int64_t now = steady_now().ns;
  std::uint64_t last_seq = 0;
  for (const std::string& f : recorded_frames()) {
    const PaddedJson j(f);
    static_cast<void>(feed.on_message(j.view(), now));
    last_seq = feed.last().sequence;
  }
  CHECK(feed.synced_count() == 1);
  CHECK(feed.resync_count() == 0);
  CHECK(feed.stats().sequence_gaps == 0);
  CHECK(requests.empty());
  Collected c;
  c.take(sink);
  CHECK(c.count(EventType::BookSnapshot) == 1);
  CHECK(c.count(EventType::ConnectionState) == 0);
  // A message skipped: every book resyncs and is resubscribed.
  const PaddedJson gap(
      R"({"channel":"heartbeats","client_id":"","timestamp":"2026-09-30T02:37:15.05Z","sequence_num":)" +
      std::to_string(last_seq + 2) + R"(,"events":[{"current_time":"x","heartbeat_counter":1}]})");
  static_cast<void>(feed.on_message(gap.view(), now + 5'000'000'000));
  CHECK(feed.stats().sequence_gaps == 1);
  CHECK(feed.synced_count() == 0);
  REQUIRE(requests.size() == 1);
  c.take(sink);
  const auto* st = c.last<ConnectionStateMsg>(EventType::ConnectionState);
  REQUIRE(st != nullptr);
  CHECK(st->state == ConnState::Resyncing);
  CHECK(st->reason_code == static_cast<std::int32_t>(SyncReason::SequenceGap));
  const auto re = feed.resubscribe_payloads(InstrumentId{0});
  CHECK(re[0] == R"({"type":"unsubscribe","product_ids":["BTC-USD"],"channel":"level2"})");
  CHECK(re[1] == R"({"type":"subscribe","product_ids":["BTC-USD"],"channel":"level2"})");
}

TEST_CASE("coinbase_advanced.user_parser: order states, fills due, the recorded envelopes") {
  Universe u;
  AdvancedUserParser p(u.symbols, kVenue);
  Scratch s;
  AdvancedUserResult r;
  auto decode = [&](const std::string& frame) {
    const PaddedJson j(frame);
    p.decode(j.view(), Timestamp{1}, Cycles{2}, s.span(), r);
  };
  // Recorded from the live account (2026-09-30): the empty snapshot, the acknowledgements, a
  // heartbeat.
  decode(
      R"({"channel":"user","client_id":"","timestamp":"2026-09-30T02:37:14.089808906Z","sequence_num":0,"events":[{"type":"snapshot","orders":[],"positions":{"perpetual_futures_positions":[],"expiring_futures_positions":[],"prediction_market_positions":[]}}]})");
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.sequence == 0);
  decode(
      R"({"channel":"subscriptions","client_id":"","timestamp":"2026-09-30T02:37:14.08981798Z","sequence_num":1,"events":[{"subscriptions":{"user":["00000000-0000-0000-0000-000000000000"]}}]})");
  CHECK(r.control == UserControl::Subscriptions);
  decode(
      R"({"channel":"heartbeats","client_id":"","timestamp":"2026-09-30T02:37:15.057895263Z","sequence_num":3,"events":[{"current_time":"2026-09-30 02:37:15.057778606 +0000 UTC m=+21697.195984854","heartbeat_counter":21697}]})");
  CHECK(r.control == UserControl::Heartbeat);

  // OPEN: acknowledged once.
  decode(user_order("OPEN", "0"));
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  CHECK(s.as<OrderAckMsg>().hdr.type == EventType::OrderAck);
  CHECK(s.as<OrderAckMsg>().cl_ord_id == make_cl_ord_id(1, 1));
  CHECK(s.as<OrderAckMsg>().venue_order_id.view() == "11111111-2222-4333-8444-555555555555");
  CHECK(r.due_count == 0);
  decode(user_order("OPEN", "0"));
  CHECK(r.count == 0);
  // A fill shows as a larger cumulative quantity: its executions are due, once per increase.
  decode(user_order("OPEN", "0.1"));
  CHECK(r.count == 0);
  REQUIRE(r.due_count == 1);
  CHECK(r.due[0].cl == make_cl_ord_id(1, 1));
  CHECK(r.due[0].cum == Qty::from_decimal("0.1").value());
  CHECK(r.due[0].order_id.view() == "11111111-2222-4333-8444-555555555555");
  decode(user_order("OPEN", "0.1"));
  CHECK(r.due_count == 0);
  // Cancelled with what filled.
  decode(user_order("CANCELLED", "0.1"));
  REQUIRE(r.count == 1);
  CHECK(s.as<OrderCancelAckMsg>().hdr.type == EventType::OrderCancelAck);
  CHECK(s.as<OrderCancelAckMsg>().cum_qty == Qty::from_decimal("0.1").value());
  CHECK(p.tracked() == 0);
  // An IOC's remainder: expired; a FAILED order: rejected.
  decode(user_order("CANCELLED", "0", "IMMEDIATE_OR_CANCEL", "fm000100000002"));
  REQUIRE(r.count == 2);  // its ack, then the end
  CHECK(reinterpret_cast<const EventHeader*>(s.buf + sizeof(OrderAckMsg))->type ==
        EventType::OrderExpired);
  std::string failed = user_order("FAILED", "0", "GOOD_UNTIL_CANCELLED", "fm000100000003");
  const std::string empty_reason = R"("reject_reason":"")";
  failed.replace(
      failed.find(empty_reason), empty_reason.size(), R"("reject_reason":"INSUFFICIENT_FUND")");
  decode(failed);
  REQUIRE(r.count == 1);
  CHECK(s.as<OrderRejectMsg>().reason == RejectReason::InsufficientBalance);
  // Another client's order: nothing.
  decode(user_order("OPEN", "0.2", "GOOD_UNTIL_CANCELLED", "web-order-1"));
  CHECK(r.count == 0);
  CHECK(r.due_count == 0);
  CHECK(p.stats().foreign == 1);
  decode(R"({"type":"error","message":"authentication failure"})");
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.msg == "authentication failure");
  decode("{");
  CHECK(r.status == ParseStatus::Malformed);
}
