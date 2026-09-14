// Deribit request encoding (golden JSON-RPC frames), tick schedules, exact JSON numbers, the error
// map and the credit bucket.
#include "fastmm/venues/deribit/deribit_order_encoder.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/deribit/deribit_credits.hpp"
#include "fastmm/venues/deribit/deribit_error_map.hpp"
#include "fastmm/venues/deribit/deribit_json.hpp"
#include "fastmm/venues/deribit/deribit_md_parser.hpp"

#include <array>
#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::deribit;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  std::array<TickSchedule, kMaxInstruments> ticks{};
  Universe() {
    Instrument call = fastmm::venues::test::make_instrument("BTC-15SEP26-77000-C", 2, "BTC", "BTC");
    call.asset_class = AssetClass::Option;
    call.tick = px("0.0001");
    call.lot = qt("0.1");
    REQUIRE(instruments.add(call));  // id 0
    Instrument perp = fastmm::venues::test::make_instrument("BTC-PERPETUAL", 2, "BTC", "USD");
    perp.asset_class = AssetClass::Perpetual;
    perp.tick = px("0.5");
    perp.lot = qt("1");
    perp.contract_multiplier = Qty::from_int(10);
    REQUIRE(instruments.add(perp));  // id 1
    REQUIRE(symbols.build(instruments));
    ticks[0].base = px("0.0001");
    REQUIRE(ticks[0].steps.push_back(TickStep{px("0.005"), px("0.0005")}));
    ticks[1].base = px("0.5");
  }
};

OutNewOrderMsg new_order(
    const char* id, Side side, OrderType type, const char* price, const char* qty) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{2});
  n.cl_ord_id = decode_cl_ord_id(id).value();
  n.side = side;
  n.type = type;
  n.tif = TimeInForce::Gtc;
  n.price = px(price);
  n.qty = qt(qty);
  return n;
}

std::string encode(const DeribitOrderEncoder& e, const EventHeader& h, const OrderShadow* shadow) {
  char buf[kMaxRequestBytes];
  const auto cmd = OrderCommand::from(h);
  REQUIRE(cmd.has_value());
  const std::size_t n = e.encode(*cmd, shadow, "tok", buf);
  return std::string(buf, n);
}

}  // namespace

TEST_CASE("deribit.json: exact JSON number parsing with exponents") {
  CHECK(json_number_to_raw("76914.0") == 7'691'400'000'000);
  CHECK(json_number_to_raw("1.0002e6") == 100'020'000'000'000);  // recorded book amount
  CHECK(json_number_to_raw("6.3e4") == 6'300'000'000'000);
  CHECK(json_number_to_raw("2.8e-4") == 28'000);
  CHECK(json_number_to_raw("-0.00015") == -15'000);
  CHECK(json_number_to_raw("1E+2") == 10'000'000'000);
  CHECK(json_number_to_raw("0") == 0);
  CHECK(json_number_to_raw("0.000000015") == 2);  // 1.5 raw units: half away from zero
  CHECK(json_number_to_raw("1e-9") == 0);
  CHECK(json_number_to_raw("9762322500") == 976'232'250'000'000'000);  // recorded open interest
  CHECK(json_number_to_raw("92233720368.54775807") == std::numeric_limits<std::int64_t>::max());
  CHECK_FALSE(json_number_to_raw("92233720369").has_value());
  CHECK(json_number_to_raw("10.0 ") == 1'000'000'000);
  CHECK_FALSE(json_number_to_raw("null").has_value());
  CHECK_FALSE(json_number_to_raw("").has_value());
  CHECK_FALSE(json_number_to_raw(".5").has_value());
  CHECK_FALSE(json_number_to_raw("1.").has_value());
  CHECK_FALSE(json_number_to_raw("1e").has_value());
  CHECK_FALSE(json_number_to_raw("1.2.3").has_value());
  CHECK_FALSE(json_number_to_raw("\"1\"").has_value());
  CHECK(amount_to_contracts(qt("1000200"), Qty::from_int(10)) == qt("100020"));
  CHECK(amount_to_contracts(qt("0.1"), Qty::from_int(1)) == qt("0.1"));
  CHECK(contracts_to_amount(qt("845"), Qty::from_int(10)) == qt("8450"));
  CHECK(trade_id_of("267258393") == 267258393ULL);
  CHECK(trade_id_of("ETH-12345") != trade_id_of("ETH-12346"));
}

TEST_CASE("deribit.encoder: tick schedule rounds passively onto the stepped grid") {
  TickSchedule t;
  t.base = px("0.0001");
  REQUIRE(t.steps.push_back(TickStep{px("0.005"), px("0.0005")}));
  CHECK(t.tick_for(px("0.0049")) == px("0.0001"));
  CHECK(t.tick_for(px("0.005")) == px("0.0005"));
  CHECK(t.round(px("0.00523"), Side::Buy) == px("0.005"));
  CHECK(t.round(px("0.00523"), Side::Sell) == px("0.0055"));
  CHECK(t.round(px("0.00497"), Side::Buy) == px("0.0049"));
  CHECK(t.round(px("0.00497"), Side::Sell) == px("0.005"));  // ceil 0.0050 is on both grids
  CHECK(t.round(px("0.004999"), Side::Sell) == px("0.005"));
  CHECK(t.round(px("0.0123"), Side::Buy) == px("0.012"));
  CHECK(t.round(px("0.0065"), Side::Sell) == px("0.0065"));
}

TEST_CASE("deribit.encoder: private/buy, private/sell, private/edit, private/cancel frames") {
  Universe u;
  const DeribitOrderEncoder enc(u.symbols, u.instruments, u.ticks, true);

  OutNewOrderMsg n = new_order("fm000100000001", Side::Buy, OrderType::PostOnly, "0.00523", "0.5");
  CHECK(
      encode(enc, n.hdr, nullptr) ==
      R"({"jsonrpc":"2.0","id":"nfm000100000001","method":"private/buy","params":{"instrument_name":"BTC-15SEP26-77000-C","contracts":0.5,"type":"limit","price":0.005,"time_in_force":"good_til_cancelled","post_only":true,"reject_post_only":true,"label":"fm000100000001","access_token":"tok"}})");

  OutNewOrderMsg ioc = new_order("fm000100000002", Side::Sell, OrderType::Limit, "0.00523", "1");
  ioc.tif = TimeInForce::Ioc;
  CHECK(
      encode(enc, ioc.hdr, nullptr) ==
      R"({"jsonrpc":"2.0","id":"nfm000100000002","method":"private/sell","params":{"instrument_name":"BTC-15SEP26-77000-C","contracts":1,"type":"limit","price":0.0055,"time_in_force":"immediate_or_cancel","post_only":false,"label":"fm000100000002","access_token":"tok"}})");

  OutNewOrderMsg mkt = new_order("fm000100000003", Side::Sell, OrderType::Market, "0", "10");
  mkt.hdr.instrument = InstrumentId{1};
  mkt.reduce_only = 1;
  CHECK(
      encode(enc, mkt.hdr, nullptr) ==
      R"({"jsonrpc":"2.0","id":"nfm000100000003","method":"private/sell","params":{"instrument_name":"BTC-PERPETUAL","contracts":10,"type":"market","post_only":false,"label":"fm000100000003","reduce_only":true,"access_token":"tok"}})");

  const DeribitOrderEncoder repricing(u.symbols, u.instruments, u.ticks, false);
  CHECK(encode(repricing, n.hdr, nullptr).find("reject_post_only") == std::string::npos);

  OrderShadow shadow;
  shadow.instrument = InstrumentId{0};
  shadow.side = Side::Buy;
  shadow.type = OrderType::PostOnly;
  shadow.label = decode_cl_ord_id("fm000100000001").value();
  shadow.venue_order_id.assign("42710123456");
  shadow.qty = qt("0.5");

  OutReplaceMsg rp{};
  init_header(rp, EventType::OutReplace, InstrumentId{0}, VenueId{2});
  rp.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
  rp.orig_cl_ord_id = shadow.label;
  rp.price = px("0.006");
  rp.qty = qt("2");
  CHECK(
      encode(enc, rp.hdr, &shadow) ==
      R"({"jsonrpc":"2.0","id":"rfm000100000002","method":"private/edit","params":{"order_id":"42710123456","contracts":2,"price":0.006,"post_only":true,"reject_post_only":true,"access_token":"tok"}})");
  CHECK(encode(enc, rp.hdr, nullptr).empty());  // an edit needs the shadow
  OrderShadow unacked = shadow;
  unacked.venue_order_id = VenueOrderId{};
  CHECK(encode(enc, rp.hdr, &unacked).empty());  // ... and the venue's order id

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{2});
  c.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
  c.venue_order_id.assign("42710123456");
  CHECK(
      encode(enc, c.hdr, nullptr) ==
      R"({"jsonrpc":"2.0","id":"cfm000100000002","method":"private/cancel","params":{"order_id":"42710123456","access_token":"tok"}})");
  c.venue_order_id = VenueOrderId{};
  CHECK(
      encode(enc, c.hdr, &shadow) ==
      R"({"jsonrpc":"2.0","id":"cfm000100000002","method":"private/cancel","params":{"order_id":"42710123456","access_token":"tok"}})");
  CHECK(
      encode(enc, c.hdr, &unacked) ==
      R"({"jsonrpc":"2.0","id":"cfm000100000002","method":"private/cancel_by_label","params":{"label":"fm000100000001","currency":"BTC","access_token":"tok"}})");
  CHECK(enc.venue_price(InstrumentId{1}, px("76914.3"), Side::Sell) == px("76914.5"));
}

TEST_CASE("deribit.encoder: session control frames and the REST kill switch") {
  char buf[kMaxRequestBytes];
  auto s = [&](std::size_t n) { return std::string(buf, n); };
  CHECK(
      s(DeribitOrderEncoder::encode_auth(1, "cid", "sec", buf)) ==
      R"({"jsonrpc":"2.0","id":1,"method":"public/auth","params":{"grant_type":"client_credentials","client_id":"cid","client_secret":"sec"}})");
  CHECK(
      s(DeribitOrderEncoder::encode_auth_refresh(2, "rt", buf)) ==
      R"({"jsonrpc":"2.0","id":2,"method":"public/auth","params":{"grant_type":"refresh_token","refresh_token":"rt"}})");
  CHECK(s(DeribitOrderEncoder::encode_set_heartbeat(3, 5, buf)) ==
        R"({"jsonrpc":"2.0","id":3,"method":"public/set_heartbeat","params":{"interval":10}})");
  CHECK(s(DeribitOrderEncoder::encode_test(4, buf)) ==
        R"({"jsonrpc":"2.0","id":4,"method":"public/test","params":{}})");
  const std::array<std::string, 2> channels{"user.orders.option.BTC.raw",
                                            "user.trades.option.BTC.raw"};
  CHECK(
      s(DeribitOrderEncoder::encode_subscribe(6, true, channels, "tok", buf)) ==
      R"({"jsonrpc":"2.0","id":6,"method":"private/subscribe","params":{"channels":["user.orders.option.BTC.raw","user.trades.option.BTC.raw"],"access_token":"tok"}})");
  CHECK(s(DeribitOrderEncoder::encode_subscribe(9, false, channels, "tok", buf))
            .find("access_token") == std::string::npos);
  CHECK(
      s(DeribitOrderEncoder::encode_enable_cancel_on_disconnect(5, "tok", buf)) ==
      R"({"jsonrpc":"2.0","id":5,"method":"private/enable_cancel_on_disconnect","params":{"scope":"connection","access_token":"tok"}})");
  CHECK(
      s(DeribitOrderEncoder::encode_open_orders(100, "BTC", "tok", buf)) ==
      R"({"jsonrpc":"2.0","id":100,"method":"private/get_open_orders_by_currency","params":{"currency":"BTC","access_token":"tok"}})");
  CHECK(
      s(DeribitOrderEncoder::encode_cancel_all_by_instrument(200, "BTC-PERPETUAL", "tok", buf)) ==
      R"({"jsonrpc":"2.0","id":200,"method":"private/cancel_all_by_instrument","params":{"instrument_name":"BTC-PERPETUAL","access_token":"tok"}})");
  CHECK(DeribitOrderEncoder::rest_cancel_all_target("BTC-25DEC26-76000-C") ==
        "/private/cancel_all_by_instrument?instrument_name=BTC-25DEC26-76000-C");
  CHECK(DeribitOrderEncoder::rest_cancel_all_target("A B") ==
        "/private/cancel_all_by_instrument?instrument_name=A%20B");
  Credentials creds;
  creds.client_id = "id";
  creds.client_secret.value = "secret";
  CHECK(DeribitOrderEncoder::basic_auth_header(creds) == "Authorization: Basic aWQ6c2VjcmV0\r\n");
  CHECK(DeribitOrderEncoder::encode_auth(1, "cid", "sec", std::span<char>(buf, 20)) ==
        0);  // overflow
}

TEST_CASE("deribit.error_map: complete-reference codes and message fallbacks") {
  CHECK(map_error(11054).reason == RejectReason::PostOnlyWouldCross);
  CHECK(map_error(13004).action == VenueAction::Fatal);
  CHECK(map_error(10028).action == VenueAction::RateLimit);
  CHECK(map_error(10028).reason == RejectReason::VenueRateLimit);
  CHECK(map_error(10004).action == VenueAction::Reconcile);
  CHECK(map_error(10009).reason == RejectReason::InsufficientBalance);
  CHECK(map_error(10043).reason == RejectReason::InvalidTick);
  CHECK(map_error(10007).reason == RejectReason::PriceCollar);
  CHECK(map_error(11044).reason == RejectReason::VenueUnknownOrder);
  CHECK(map_error(13019).action == VenueAction::DisableInstrument);
  CHECK(map_error(10047).action == VenueAction::Backoff);
  CHECK(map_error(-32602).known);
  const ErrorMapping unknown = map_error(99999, "post_only_reject");
  CHECK_FALSE(unknown.known);
  CHECK(unknown.reason == RejectReason::PostOnlyWouldCross);
  CHECK(map_error(99999).reason == RejectReason::VenueReject);
  CHECK(needs_reauth(13009));
  CHECK(needs_reauth(10000));
  CHECK_FALSE(needs_reauth(13004));
}

TEST_CASE("deribit.credits: leaky bucket with burst and refill") {
  constexpr std::int64_t kMs = 1'000'000;
  CreditBucket b = CreditBucket::from_rate(5, 20);  // Tier 4 matching engine
  std::int64_t now = 1000 * kMs;
  for (int i = 0; i < 20; ++i) CHECK(b.try_consume(CreditBucket::kRequestCost, now));
  CHECK_FALSE(b.try_consume(CreditBucket::kRequestCost, now));
  now += 100 * kMs;  // half a request refilled
  CHECK_FALSE(b.try_consume(CreditBucket::kRequestCost, now));
  now += 100 * kMs;
  CHECK(b.try_consume(CreditBucket::kRequestCost, now));
  now += 10'000 * kMs;
  CHECK(b.available(now) == 20'000);  // capped at the pool
  b.drain(now);
  CHECK_FALSE(b.try_consume(CreditBucket::kRequestCost, now));
  b.consume(CreditBucket::kRequestCost, now);  // cancels are never refused
  CHECK(b.available(now) == -1000);
  CreditBucket off;
  CHECK(off.try_consume(1'000'000, now));
}
