#include "fastmm/venues/binance_usdm/binance_usdm_order_encoder.hpp"

#include "venue_test_util.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_error_map.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_rest_decoder.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance_usdm;
using fastmm::venues::test::TestUniverse;

namespace {

constexpr std::string_view kKey = "test-key";
constexpr std::string_view kSecret = "test-secret";
constexpr std::int64_t kTs = 1789467600000;

Signer signer() {
  binance::Credentials c;
  c.api_key = std::string(kKey);
  c.secret.value = std::string(kSecret);
  return Signer(c);
}
ClientOrderId id(const char* s) {
  return decode_cl_ord_id(s).value();
}
std::string hmac(std::string_view payload) {
  return std::string(net::hmac_sha256_hex(kSecret, payload).view());
}

struct NewOrder {
  OutNewOrderMsg m{};
  NewOrder(OrderType type, TimeInForce tif, const char* px, const char* qty, bool reduce_only) {
    init_header(m, EventType::OutNewOrder, InstrumentId{0}, VenueId{0});
    m.cl_ord_id = id("fm000100000001");
    m.side = Side::Buy;
    m.type = type;
    m.tif = tif;
    m.price = Price::from_decimal(px).value();
    m.qty = Qty::from_decimal(qty).value();
    m.reduce_only = reduce_only ? 1 : 0;
  }
  [[nodiscard]] OrderCommand cmd() const { return *OrderCommand::from(m.hdr); }
};

}  // namespace

TEST_CASE(
    "binance_usdm.encoder: order.place golden frame, GTX post-only, signature over sorted params") {
  TestUniverse u;
  const Signer s = signer();
  BinanceUsdmOrderEncoder enc(s, u.symbols, 3000);
  char buf[kMaxRequestBytes];
  const NewOrder post_only(OrderType::PostOnly, TimeInForce::Gtc, "76980.1", "0.001", false);
  const std::size_t n = enc.encode_ws(post_only.cmd(), nullptr, kTs, buf);
  REQUIRE(n > 0);
  const std::string payload =
      "apiKey=test-key&newClientOrderId=fm000100000001&newOrderRespType=ACK&price=76980.1"
      "&quantity=0.001&recvWindow=3000&side=BUY&symbol=BTCUSDT&timeInForce=GTX"
      "&timestamp=1789467600000&type=LIMIT";
  const std::string expected =
      R"({"id":"nfm000100000001","method":"order.place","params":{"apiKey":"test-key","newClientOrderId":"fm000100000001","newOrderRespType":"ACK","price":"76980.1","quantity":"0.001","recvWindow":3000,"side":"BUY","symbol":"BTCUSDT","timeInForce":"GTX","timestamp":1789467600000,"type":"LIMIT","signature":")" +
      hmac(payload) + R"("}})";
  CHECK(std::string_view(buf, n) == expected);

  // reduceOnly sits between recvWindow and side; IOC and FOK map directly; MARKET has neither
  // price nor timeInForce.
  const NewOrder ioc(OrderType::Limit, TimeInForce::Ioc, "76980.1", "0.002", true);
  const std::size_t n2 = enc.encode_ws(ioc.cmd(), nullptr, kTs, buf);
  REQUIRE(n2 > 0);
  const std::string_view v2(buf, n2);
  CHECK(v2.find(R"("recvWindow":3000,"reduceOnly":"true","side":"BUY")") != std::string_view::npos);
  CHECK(v2.find(R"("timeInForce":"IOC")") != std::string_view::npos);
  const NewOrder fok(OrderType::Limit, TimeInForce::Fok, "76980.1", "0.002", false);
  const std::size_t n3 = enc.encode_ws(fok.cmd(), nullptr, kTs, buf);
  REQUIRE(n3 > 0);
  CHECK(std::string_view(buf, n3).find(R"("timeInForce":"FOK")") != std::string_view::npos);
  CHECK(std::string_view(buf, n3).find("reduceOnly") == std::string_view::npos);
  const NewOrder market(OrderType::Market, TimeInForce::Gtc, "0", "0.002", true);
  const std::size_t n4 = enc.encode_ws(market.cmd(), nullptr, kTs, buf);
  REQUIRE(n4 > 0);
  const std::string_view v4(buf, n4);
  CHECK(v4.find("price") == std::string_view::npos);
  CHECK(v4.find("timeInForce") == std::string_view::npos);
  CHECK(v4.find(R"("type":"MARKET")") != std::string_view::npos);
  CHECK(v4.find(R"("reduceOnly":"true")") != std::string_view::npos);
}

TEST_CASE("binance_usdm.encoder: order.cancel and order.modify by order id or venue client id") {
  TestUniverse u;
  const Signer s = signer();
  BinanceUsdmOrderEncoder enc(s, u.symbols, 3000);
  char buf[kMaxRequestBytes];

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{0});
  c.cl_ord_id = id("fm000100000003");
  {
    // Not acknowledged yet, and the order was modified before: the venue knows it by its first id.
    OrderShadow sh;
    sh.link_id = id("fm000100000001");
    const std::size_t n = enc.encode_ws(*OrderCommand::from(c.hdr), &sh, kTs, buf);
    REQUIRE(n > 0);
    const std::string payload =
        "apiKey=test-key&origClientOrderId=fm000100000001&recvWindow=3000&symbol=BTCUSDT"
        "&timestamp=1789467600000";
    CHECK(
        std::string_view(buf, n) ==
        R"({"id":"cfm000100000003","method":"order.cancel","params":{"apiKey":"test-key","origClientOrderId":"fm000100000001","recvWindow":3000,"symbol":"BTCUSDT","timestamp":1789467600000,"signature":")" +
            hmac(payload) + R"("}})");
  }
  c.venue_order_id.assign("8886774");
  {
    const std::size_t n = enc.encode_ws(*OrderCommand::from(c.hdr), nullptr, kTs, buf);
    REQUIRE(n > 0);
    CHECK(std::string_view(buf, n).find(R"("orderId":8886774,"recvWindow")") !=
          std::string_view::npos);
  }

  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  r.cl_ord_id = id("fm000100000004");
  r.orig_cl_ord_id = id("fm000100000003");
  r.price = Price::from_decimal("76990").value();
  r.qty = Qty::from_decimal("0.001").value();
  OrderShadow orig;
  orig.side = Side::Sell;
  orig.type = OrderType::PostOnly;
  orig.link_id = id("fm000100000001");
  orig.cum = Qty::from_decimal("0.0004").value();
  CHECK(enc.encode_ws(*OrderCommand::from(r.hdr), nullptr, kTs, buf) == 0);  // needs the original
  const std::size_t n = enc.encode_ws(*OrderCommand::from(r.hdr), &orig, kTs, buf);
  REQUIRE(n > 0);
  // quantity = new remaining 0.001 + 0.0004 already executed ("Modify Order" takes the total).
  const std::string payload =
      "apiKey=test-key&origClientOrderId=fm000100000001&price=76990&quantity=0.0014"
      "&recvWindow=3000&side=SELL&symbol=BTCUSDT&timestamp=1789467600000";
  CHECK(
      std::string_view(buf, n) ==
      R"({"id":"rfm000100000004","method":"order.modify","params":{"apiKey":"test-key","origClientOrderId":"fm000100000001","price":"76990","quantity":"0.0014","recvWindow":3000,"side":"SELL","symbol":"BTCUSDT","timestamp":1789467600000,"signature":")" +
          hmac(payload) + R"("}})");
}

TEST_CASE("binance_usdm.encoder: REST requests sign the exact query sent") {
  TestUniverse u;
  const Signer s = signer();
  BinanceUsdmOrderEncoder enc(s, u.symbols, 5000);
  const NewOrder post_only(OrderType::PostOnly, TimeInForce::Gtc, "76980.1", "0.001", false);
  RestRequest rr;
  REQUIRE(enc.encode_rest(post_only.cmd(), nullptr, kTs, rr));
  CHECK(rr.method == "POST");
  CHECK(rr.path == "/fapi/v1/order");
  CHECK(rr.is_order);
  const std::string unsigned_query =
      "newClientOrderId=fm000100000001&newOrderRespType=ACK&price=76980.1&quantity=0.001"
      "&recvWindow=5000&side=BUY&symbol=BTCUSDT&timeInForce=GTX&timestamp=1789467600000&type=LIMIT";
  CHECK(rr.query.view() == unsigned_query + "&signature=" + hmac(unsigned_query));

  RestRequest ca;
  REQUIRE(enc.encode_rest_cancel_all("BTCUSDT", kTs, ca));
  CHECK(ca.method == "DELETE");
  CHECK(ca.path == "/fapi/v1/allOpenOrders");
  const std::string ca_query = "recvWindow=5000&symbol=BTCUSDT&timestamp=1789467600000";
  CHECK(ca.query.view() == ca_query + "&signature=" + hmac(ca_query));

  RestRequest oo;
  REQUIRE(enc.encode_rest_open_orders({}, kTs, oo));
  CHECK(oo.path == "/fapi/v1/openOrders");
  CHECK(oo.weight == 40);
  RestRequest pr;
  REQUIRE(enc.encode_rest_position_risk({}, kTs, pr));
  CHECK(pr.path == "/fapi/v3/positionRisk");
  CHECK(pr.weight == 5);

  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{0});
  r.cl_ord_id = id("fm000100000004");
  r.orig_cl_ord_id = id("fm000100000003");
  r.venue_order_id.assign("8886774");
  r.price = Price::from_decimal("76990").value();
  r.qty = Qty::from_decimal("0.001").value();
  OrderShadow orig;
  RestRequest mod;
  REQUIRE(enc.encode_rest(*OrderCommand::from(r.hdr), &orig, kTs, mod));
  CHECK(mod.method == "PUT");
  CHECK(mod.query.view().starts_with("orderId=8886774&price=76990&quantity=0.001&"));
}

TEST_CASE("binance_usdm.error_map: documented codes map to reasons and actions") {
  struct Case {
    int code;
    RejectReason reason;
    VenueAction action;
  };
  const std::vector<Case> cases = {
      {-1003, RejectReason::VenueRateLimit, VenueAction::RateLimit},
      {-1015, RejectReason::VenueRateLimit, VenueAction::RateLimit},
      {-1021, RejectReason::VenueReject, VenueAction::ResyncClock},
      {-5028, RejectReason::VenueReject, VenueAction::ResyncClock},
      {-1022, RejectReason::VenueReject, VenueAction::Fatal},
      {-2015, RejectReason::VenueReject, VenueAction::Fatal},
      {-4061, RejectReason::VenueReject, VenueAction::Fatal},
      {-1007, RejectReason::VenueReject, VenueAction::Reconcile},
      {-2011, RejectReason::VenueUnknownOrder, VenueAction::Reconcile},
      {-2013, RejectReason::VenueUnknownOrder, VenueAction::Reconcile},
      {-4116, RejectReason::DuplicateId, VenueAction::Reconcile},
      {-5022, RejectReason::PostOnlyWouldCross, VenueAction::None},
      {-2019, RejectReason::InsufficientBalance, VenueAction::None},
      {-2018, RejectReason::InsufficientBalance, VenueAction::None},
      {-2022, RejectReason::VenueReject, VenueAction::None},
      {-4118, RejectReason::VenueReject, VenueAction::None},
      {-4164, RejectReason::BelowMinNotional, VenueAction::None},
      {-4014, RejectReason::InvalidTick, VenueAction::DisableInstrument},
      {-4023, RejectReason::InvalidLot, VenueAction::DisableInstrument},
      {-4024, RejectReason::PriceCollar, VenueAction::None},
      {-2025, RejectReason::MaxOpenOrders, VenueAction::None},
      {-1008, RejectReason::VenueReject, VenueAction::Backoff},
  };
  for (const Case& c : cases) {
    CAPTURE(c.code);
    const ErrorMapping m = map_error(c.code);
    CHECK(m.known);
    CHECK(m.reason == c.reason);
    CHECK(m.action == c.action);
  }
  CHECK_FALSE(map_error(-9999).known);
  CHECK(map_error(-2010, "Order would immediately match and take.").reason ==
        RejectReason::PostOnlyWouldCross);
  CHECK(map_http_status(418).action == VenueAction::HardStop);
  CHECK(map_http_status(429).action == VenueAction::RateLimit);
  CHECK(map_http_status(503).action == VenueAction::Reconcile);
  CHECK(map_http_status(403).action == VenueAction::Backoff);
}

TEST_CASE("binance_usdm.rest_decoder: exchangeInfo filters, positions, mode, config, balance") {
  ExchangeInfo info;
  const std::vector<std::string> wanted = {"btcusdt"};
  REQUIRE(
      decode_exchange_info(fastmm::test::fixture("binance_usdm/exchange_info.json"), info, wanted)
          .empty());
  REQUIRE(info.symbols.size() == 1);
  const SymbolInfo& b = info.symbols[0];
  CHECK(b.symbol == "BTCUSDT");
  CHECK(b.status == "TRADING");
  CHECK(b.contract_type == "PERPETUAL");
  CHECK(b.margin_asset == "USDT");
  CHECK(b.tick == Price::from_decimal("0.10").value());
  CHECK(b.step == Qty::from_decimal("0.0001").value());
  CHECK(b.min_qty == Qty::from_decimal("0.0001").value());
  CHECK(b.max_qty == Qty::from_decimal("1000").value());
  CHECK(b.min_notional == Notional::from_int(50));
  CHECK(b.gtx_allowed);
  CHECK(info.server_time_ms > 0);
  REQUIRE(info.rate_limits.size() == 3);
  CHECK(info.rate_limits[0].type == "REQUEST_WEIGHT");
  ExchangeInfo all;
  REQUIRE(
      decode_exchange_info(fastmm::test::fixture("binance_usdm/exchange_info.json"), all).empty());
  CHECK(all.symbols.size() == 2);

  std::vector<PositionRecord> pos;
  REQUIRE(
      decode_position_risk(fastmm::test::fixture("binance_usdm/position_risk.json"), pos).empty());
  REQUIRE(pos.size() == 1);
  CHECK(pos[0].symbol == "BTCUSDT");
  CHECK(pos[0].position_side == "BOTH");
  CHECK(pos[0].qty == Qty::from_decimal("-0.002").value());
  CHECK(pos[0].entry_price == Price::from_decimal("76975.3").value());
  CHECK_FALSE(decode_position_risk("{}", pos).empty());

  bool dual = true;
  REQUIRE(decode_position_mode(R"({"dualSidePosition":false})", dual).empty());
  CHECK_FALSE(dual);
  std::vector<SymbolConfig> cfg;
  REQUIRE(
      decode_symbol_config(
          R"([{"symbol":"BTCUSDT","marginType":"CROSSED","isAutoAddMargin":false,"leverage":20,"maxNotionalValue":"10000000"}])",
          cfg)
          .empty());
  REQUIRE(cfg.size() == 1);
  CHECK(cfg[0].leverage == 20);
  CHECK(cfg[0].margin_type == "CROSSED");
  std::vector<BalanceRecord> bal;
  REQUIRE(
      decode_balance(
          R"([{"accountAlias":"SgsR","asset":"USDT","balance":"5000.00000000","crossWalletBalance":"5000.00000000","crossUnPnl":"0.00000000","availableBalance":"4990.5","maxWithdrawAmount":"4990.5","marginAvailable":true,"updateTime":1789467600000}])",
          bal)
          .empty());
  REQUIRE(bal.size() == 1);
  CHECK(bal[0].available == Notional::from_decimal("4990.5").value());
}
