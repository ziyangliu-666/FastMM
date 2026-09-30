// DeribitPrivateParser on docs-schema private fixtures (user.orders, user.trades, order and auth
// responses, open orders) and recorded error responses.
#include "fastmm/venues/deribit/deribit_private_parser.hpp"

#include "venue_test_util.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::deribit;
using namespace fastmm::venues::test;

namespace {

constexpr VenueId kVenue{2};

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    // Both are Deribit `reversed` (inverse) instruments; the option is quoted in BTC.
    Instrument call = make_instrument("BTC-15SEP26-77000-C", kVenue.value, "BTC", "BTC");
    call.asset_class = AssetClass::Option;
    call.flags |= Instrument::kInverse;
    REQUIRE(instruments.add(call));
    Instrument perp = make_instrument("BTC-PERPETUAL", kVenue.value, "BTC", "USD");
    perp.asset_class = AssetClass::Perpetual;
    perp.flags |= Instrument::kInverse;
    perp.contract_multiplier = Qty::from_int(10);
    REQUIRE(instruments.add(perp));
    REQUIRE(symbols.build(instruments));
  }
};

PrivateDecodeResult decode(DeribitPrivateParser& p, const std::string& fixture, Scratch& s) {
  const PaddedJson j = padded_fixture(fixture);
  return p.decode(j.view(), Timestamp{42}, Cycles{7}, s.span());
}

const ClientOrderId kId = decode_cl_ord_id("fm000100000001").value();

}  // namespace

TEST_CASE("deribit.private_parser: user.orders open, cancelled, filled and foreign labels") {
  Universe u;
  DeribitPrivateParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  PrivateDecodeResult r = decode(p, "deribit/user_orders_open.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);
  const auto& ack = s.as<OrderAckMsg>();
  CHECK(ack.hdr.type == EventType::OrderAck);
  CHECK(ack.hdr.instrument == InstrumentId{0});
  CHECK(ack.cl_ord_id == kId);
  CHECK(ack.venue_order_id.view() == "42710123456");
  CHECK(ack.hdr.exch_ts.ns == 1789345400123LL * 1'000'000);

  r = decode(p, "deribit/user_orders_cancelled.json", s);
  REQUIRE(r.ok());
  const auto& cx = s.as<OrderCancelAckMsg>();
  CHECK(cx.hdr.type == EventType::OrderCancelAck);
  CHECK(cx.cl_ord_id == kId);
  CHECK(cx.cum_qty == qt("0.5"));

  r = decode(p, "deribit/user_orders_filled.json", s);  // array data, aggregated channel
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.count == 0);

  r = decode(p, "deribit/user_orders_foreign_label.json", s);
  REQUIRE(r.ok());
  CHECK_FALSE(s.as<OrderAckMsg>().cl_ord_id.valid());
  CHECK(p.stats().foreign_ids == 1);
}

TEST_CASE("deribit.private_parser: user.trades become fills") {
  Universe u;
  DeribitPrivateParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const PrivateDecodeResult r = decode(p, "deribit/user_trades.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);
  const auto& f = s.as<OrderFillMsg>();
  CHECK(f.hdr.type == EventType::OrderFill);
  CHECK(f.cl_ord_id == kId);
  CHECK(f.venue_order_id.view() == "42710123456");
  CHECK(f.exec_id.view() == "267259001");
  CHECK(f.price == px("0.006"));
  CHECK(f.qty == qt("0.5"));
  CHECK(f.cum_qty.is_zero());  // the venue fills cum/leaves from its order shadow
  CHECK(f.fee == Notional::from_decimal("0.00015").value());
  CHECK(f.side == Side::Buy);
  CHECK(f.liquidity == Liquidity::Maker);
  CHECK(f.hdr.exch_ts.ns == 1789345401234LL * 1'000'000);
}

TEST_CASE("deribit.private_parser: fee_asset comes from fee_currency, never from the scratch") {
  Universe u;
  DeribitPrivateParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  // The venue reuses one scratch buffer for every frame; a stale byte must not decide how the
  // engine books the fee (Base makes it rewrite the filled quantity, Other drops the fee).
  std::memset(s.buf, 0xFF, sizeof s.buf);
  PrivateDecodeResult r = decode(p, "deribit/user_trades.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);
  // BTC options are quoted in BTC (base == quote), so their BTC fee is a quote amount.
  CHECK(s.as<OrderFillMsg>().fee_asset == FeeAsset::Quote);

  // An inverse future is quoted in USD and charges the fee in BTC: neither a quote amount nor a
  // number of contracts, so the engine counts it instead of booking it.
  static constexpr std::string_view kPerpTrade =
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.trades.future.BTC.raw",)"
      R"("data":[{"trade_id":"267259002","timestamp":1789345401300,"order_id":"42710123457",)"
      R"("label":"fm000100000001","instrument_name":"BTC-PERPETUAL","fee_currency":"BTC",)"
      R"("fee":0.0000012,"direction":"buy","amount":100.0,"price":76950.5,"liquidity":"T"}]}})";
  const PaddedJson perp(kPerpTrade);
  r = p.decode(perp.view(), Timestamp{42}, Cycles{7}, s.span());
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);
  const auto& f = s.as<OrderFillMsg>();
  CHECK(f.fee == Notional::from_decimal("0.0000012").value());
  CHECK(f.fee_asset == FeeAsset::Other);
  CHECK(f.qty == qt("10"));  // 100 USD / 10 USD per contract
}

TEST_CASE("deribit.private_parser: order, auth, subscribe and error responses") {
  Universe u;
  DeribitPrivateParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  PrivateDecodeResult r = decode(p, "deribit/auth_ok.json", s);
  CHECK(r.frame == FrameKind::Response);
  CHECK(r.rpc.id == 1);
  REQUIRE(r.auth.present);
  CHECK(r.auth.access_token == "1789345400000.1MbQ-J_4.CBP-OqOwFakeAccessToken");
  CHECK(r.auth.refresh_token == "1789345400000.1GP4rQd0.A9Wa78FakeRefreshToken");
  CHECK(r.auth.expires_in == 900);

  r = decode(p, "deribit/buy_ok.json", s);
  CHECK(r.rpc.id_text == "nfm000100000001");
  REQUIRE(r.order.present);
  CHECK(r.order.order_id == "42710123456");
  CHECK(r.order.order_state == "open");
  CHECK(r.order.label == "fm000100000001");
  CHECK(r.order.instrument_name == "BTC-15SEP26-77000-C");
  CHECK(r.order.amount == qt("1"));

  r = decode(p, "deribit/edit_ok.json", s);
  CHECK(r.rpc.id_text == "rfm000100000002");
  REQUIRE(r.order.present);
  CHECK(r.order.amount == qt("2"));

  r = decode(p, "deribit/cancel_ok.json", s);  // private/cancel returns the order itself
  CHECK(r.rpc.id_text == "cfm000100000002");
  REQUIRE(r.order.present);
  CHECK(r.order.order_state == "cancelled");
  CHECK(r.order.filled_amount == qt("0.5"));

  r = decode(p, "deribit/cancel_by_label_ok.json", s);
  CHECK(r.result_int == 1);
  CHECK_FALSE(r.order.present);

  r = decode(p, "deribit/private_subscribe_ok.json", s);
  CHECK(r.rpc.id == 6);
  CHECK(r.result_items == 2);

  r = decode(p, "deribit/post_only_reject.json", s);
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.rpc.id_text == "nfm000100000004");
  CHECK(r.rpc.error_code == 11054);
  CHECK(r.rpc.error_message == "post_only_reject");

  r = decode(p, "deribit/rpc_invalid_credentials.json", s);
  CHECK(r.rpc.error_code == 13004);
  CHECK_FALSE(r.auth.present);

  r = decode(p, "deribit/heartbeat_test_request.json", s);
  CHECK(r.frame == FrameKind::TestRequest);
  CHECK(p.stats().heartbeats == 1);

  // Notifications for other channels and unknown instruments are ignored.
  const PaddedJson other(
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.mmp_trigger.btc","data":{"frozen_until":0}}})");
  CHECK(p.decode(other.view(), Timestamp{}, Cycles{}, s.span()).status == ParseStatus::Ignored);
  const PaddedJson unknown(
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.orders.future.ETH.raw","data":{"order_id":"1","order_state":"open","label":"fm000100000001","instrument_name":"ETH-PERPETUAL"}}})");
  CHECK(p.decode(unknown.view(), Timestamp{}, Cycles{}, s.span()).status == ParseStatus::Ignored);
  CHECK(p.stats().unknown_symbol == 1);
  const PaddedJson bad(
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.trades.option.BTC.raw","data":[{"order_id":"1","instrument_name":"BTC-15SEP26-77000-C"}]}})");
  CHECK(p.decode(bad.view(), Timestamp{}, Cycles{}, s.span()).status == ParseStatus::Malformed);
}

TEST_CASE("deribit.private_parser: get_open_orders_by_currency records") {
  Universe u;
  DeribitPrivateParser p(u.symbols, u.instruments, kVenue);
  std::vector<OpenOrderRecord> recs;
  std::vector<std::string> labels;
  const PaddedJson j = padded_fixture("deribit/open_orders.json");
  REQUIRE(p.decode_open_orders(j.view(), [&](const OpenOrderRecord& o) {
    recs.push_back(o);
    labels.emplace_back(o.label);
  }) == ParseStatus::Ok);
  REQUIRE(recs.size() == 2);
  CHECK(labels[0] == "fm000100000001");
  CHECK(recs[0].price == px("0.006"));
  CHECK(recs[0].amount == qt("2"));
  CHECK(recs[0].filled_amount == qt("0.5"));
  CHECK(recs[1].direction == "sell");
  CHECK(recs[1].amount == qt("100"));
  CHECK(recs[1].filled_amount == qt("20"));
  const PaddedJson err = padded_fixture("deribit/rpc_unauthorized.json");
  CHECK(p.decode_open_orders(err.view(), [](const OpenOrderRecord&) {}) == ParseStatus::Error);
}

namespace {
Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}
}  // namespace

TEST_CASE("deribit.private_parser: user.portfolio is the currency's balance") {
  // The AsyncAPI example of user.portfolio.(currency) (deribit_asyncapi.json, read 2026-09-30).
  Universe u;
  DeribitPrivateParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const PrivateDecodeResult r = decode(p, "deribit/user_portfolio_btc.json", s);
  CHECK(r.status == ParseStatus::Ok);
  CHECK(r.count == 0);  // no order event
  REQUIRE(r.portfolio.present);
  CHECK(r.portfolio.currency == "BTC");
  CHECK(r.portfolio.fields.free == nt("301.38036328"));      // available_funds
  CHECK(r.portfolio.fields.locked == nt("1.24639592"));      // initial_margin
  CHECK(r.portfolio.fields.total == nt("302.60065765"));     // balance
  CHECK(r.portfolio.fields.equity == nt("302.6188592"));     // equity
  CHECK(r.portfolio.fields.maintenance == nt("0.8854841"));  // maintenance_margin
  CHECK_FALSE(r.portfolio.has_account);                      // cross_collateral_enabled false

  // Cross collateral on: the total_*_usd fields value the whole account in USD (the fields as the
  // channel documents them; the example has cross collateral off).
  const PaddedJson cross(
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.portfolio.btc","data":{"currency":"BTC","balance":1.5,"equity":1.52,"available_funds":1.2,"initial_margin":0.3,"maintenance_margin":0.2,"margin_balance":1.5,"cross_collateral_enabled":true,"total_equity_usd":171000.5,"total_initial_margin_usd":30000,"total_maintenance_margin_usd":20000,"total_margin_balance_usd":170000.25,"delta_total_map":{"btc_usd":0.1}}}})");
  const PrivateDecodeResult c = p.decode(cross.view(), Timestamp{}, Cycles{}, s.span());
  REQUIRE(c.portfolio.present);
  REQUIRE(c.portfolio.has_account);
  CHECK(c.portfolio.account.free == nt("140000.25"));
  CHECK(c.portfolio.account.locked == nt("30000"));
  CHECK(c.portfolio.account.total == nt("170000.25"));
  CHECK(c.portfolio.account.equity == nt("171000.5"));
  CHECK(c.portfolio.account.maintenance == nt("20000"));

  // Without the fields the mapping needs it is malformed, not a zero balance.
  const PaddedJson partial(
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.portfolio.btc","data":{"currency":"BTC","equity":1.0}}})");
  const PrivateDecodeResult m = p.decode(partial.view(), Timestamp{}, Cycles{}, s.span());
  CHECK(m.status == ParseStatus::Malformed);
  CHECK_FALSE(m.portfolio.present);
}

TEST_CASE("deribit.private_parser: get_account_summaries rows and usOut") {
  // The OpenAPI example of /private/get_account_summaries (deribit_openapi.json, read
  // 2026-09-30); it carries no usIn/usOut.
  Universe u;
  DeribitPrivateParser p(u.symbols, u.instruments, kVenue);
  std::vector<PortfolioRecord> rows;
  std::vector<std::string> names;
  std::int64_t us_out = -1;
  const PaddedJson j = padded_fixture("deribit/account_summaries.json");
  REQUIRE(p.decode_account_summaries(j.view(), us_out, [&](const PortfolioRecord& r) {
    rows.push_back(r);
    names.emplace_back(r.currency);
  }) == ParseStatus::Ok);
  CHECK(us_out == 0);
  REQUIRE(rows.size() == 2);
  CHECK(names == std::vector<std::string>{"BTC", "ETH"});
  CHECK(rows[0].fields.free == nt("301.38059622"));
  CHECK(rows[0].fields.locked == nt("1.24669592"));
  CHECK(rows[0].fields.total == nt("302.60065765"));
  CHECK(rows[0].fields.equity == nt("302.61869214"));
  CHECK(rows[0].fields.maintenance == nt("0.8857841"));
  CHECK(rows[1].fields.free == nt("99.999598"));
  CHECK(rows[1].fields.locked == nt("0.000402"));
  CHECK(rows[1].fields.total == nt("100"));
  CHECK(rows[1].fields.equity == nt("100"));
  CHECK(rows[1].fields.maintenance.is_zero());

  // usOut stands in the JSON-RPC envelope (json-rpc-overview article), after the result here.
  const PaddedJson stamped(
      R"({"jsonrpc":"2.0","id":8,"result":{"summaries":[{"currency":"BTC","balance":1,"equity":1,"available_funds":1,"initial_margin":0,"maintenance_margin":0}]},"usIn":1790730000000100,"usOut":1790730000000450,"usDiff":350,"testnet":true})");
  CHECK(p.decode_account_summaries(stamped.view(), us_out, [](const PortfolioRecord&) {}) ==
        ParseStatus::Ok);
  CHECK(us_out == 1790730000000450);
  const PaddedJson err = padded_fixture("deribit/rpc_unauthorized.json");
  CHECK(p.decode_account_summaries(err.view(), us_out, [](const PortfolioRecord&) {}) ==
        ParseStatus::Error);
}
