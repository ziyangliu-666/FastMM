// GatePrivateParser on the private channels' frames (docs examples rewritten with FastMM text
// ids): order statuses to order events, usertrades to fills with the fee in the settle currency,
// positions in single mode only, balances to a refresh request and funding payments, foreign ids.
#include "fastmm/venues/gate/gate_private_parser.hpp"

#include "venue_test_util.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::gate;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kGate{3};

struct GateUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  InstrumentId nvda;
  InstrumentId btc;
  GateUniverse() {
    Instrument n = make_instrument("NVDA_USDT", 3, "NVDA", "USDT");
    n.asset_class = AssetClass::Perpetual;
    n.lot = Qty::from_int(1);
    Instrument b = make_instrument("BTC_USDT", 3, "BTC", "USDT");
    b.asset_class = AssetClass::Perpetual;
    b.lot = Qty::from_int(1);
    nvda = instruments.add(n).value();
    btc = instruments.add(b).value();
    REQUIRE(symbols.build(instruments));
  }
};

}  // namespace

TEST_CASE("gate.private_parser: orders channel statuses map to order events") {
  GateUniverse u;
  GatePrivateParser p(u.symbols, u.instruments, kGate);
  Scratch s;
  {
    const auto f = padded_fixture("gate/private_order_open.json");
    const auto r = p.decode(f.view(), Timestamp{7}, Cycles{9}, s.span());
    REQUIRE(r.ok());
    CHECK(r.count == 1);
    CHECK(r.order_kind == OrderEventKind::Ack);
    const auto& m = s.as<OrderAckMsg>();
    CHECK(m.hdr.type == EventType::OrderAck);
    CHECK(m.hdr.instrument == u.nvda);
    CHECK(m.cl_ord_id == ClientOrderId{1});
    CHECK(m.venue_order_id.view() == "74046514");
    CHECK(m.hdr.exch_ts.ns == 1791009980101LL * 1'000'000);
    CHECK(m.hdr.recv_ts.ns == 7);
  }
  {
    // finished / cancelled with one of three contracts filled: cum 1.
    const auto f = padded_fixture("gate/private_order_cancelled.json");
    const auto r = p.decode(f.view(), Timestamp{7}, Cycles{9}, s.span());
    REQUIRE(r.ok());
    const auto& m = s.as<OrderCancelAckMsg>();
    CHECK(m.hdr.type == EventType::OrderCancelAck);
    CHECK(m.cl_ord_id == ClientOrderId{1});
    CHECK(m.cum_qty == Qty::from_int(1));
    CHECK(m.hdr.exch_ts.ns == 1791009990100LL * 1'000'000);
  }
  {
    // finished / ioc (integer sizes, no decimal header): the remainder expired; cum 6 of 10.
    const auto f = padded_fixture("gate/private_order_ioc.json");
    const auto r = p.decode(f.view(), Timestamp{7}, Cycles{9}, s.span());
    REQUIRE(r.ok());
    const auto& m = s.as<OrderExpiredMsg>();
    CHECK(m.hdr.type == EventType::OrderExpired);
    CHECK(m.hdr.instrument == u.btc);
    CHECK(m.cl_ord_id == ClientOrderId{2});
    CHECK(m.cum_qty == Qty::from_int(6));
  }
  {
    // finished / filled: nothing (the usertrades carry the fills).
    const auto f = padded_fixture("gate/private_order_filled.json");
    const auto r = p.decode(f.view(), Timestamp{7}, Cycles{9}, s.span());
    CHECK(r.status == ParseStatus::Ignored);
  }
  {
    // open with a part filled: the fill's echo, nothing.
    const PaddedJson part(
        R"({"channel":"futures.orders","event":"update","time":1,"time_ms":1000,"result":[{"contract":"NVDA_USDT","id":74046514,"left":"2","price":"234.1","size":"3","status":"open","finish_as":"","text":"t-fm000000000001","create_time_ms":1000,"update_time":1500}]})");
    const auto r = p.decode(part.view(), Timestamp{7}, Cycles{9}, s.span());
    CHECK(r.status == ParseStatus::Ignored);
  }
  {
    // A foreign text id still produces the event, with an invalid client id.
    const PaddedJson foreign(
        R"({"channel":"futures.orders","event":"update","time":1,"time_ms":1000,"result":[{"contract":"NVDA_USDT","id":99,"left":"5","price":"230","size":"5","status":"open","finish_as":"","text":"web","create_time_ms":1000}]})");
    const auto r = p.decode(foreign.view(), Timestamp{7}, Cycles{9}, s.span());
    REQUIRE(r.ok());
    CHECK_FALSE(s.as<OrderAckMsg>().cl_ord_id.valid());
    CHECK(p.stats().foreign_ids == 1);
  }
  {
    const PaddedJson unknown(
        R"({"channel":"futures.orders","event":"update","time":1,"time_ms":1000,"result":[{"contract":"NOSUCH_USDT","id":99,"left":"5","price":"230","size":"5","status":"open","text":"t-fm000000000009"}]})");
    const auto r = p.decode(unknown.view(), Timestamp{7}, Cycles{9}, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(p.stats().unknown_symbol == 1);
    const PaddedJson malformed(
        R"({"channel":"futures.orders","event":"update","time":1,"time_ms":1000,"result":[{"contract":"NVDA_USDT","id":99,"price":"230","status":"open"}]})");
    CHECK(p.decode(malformed.view(), Timestamp{7}, Cycles{9}, s.span()).status ==
          ParseStatus::Malformed);
  }
}

TEST_CASE("gate.private_parser: usertrades -> fills, positions, balances, control") {
  GateUniverse u;
  GatePrivateParser p(u.symbols, u.instruments, kGate);
  Scratch s;
  {
    const auto f = padded_fixture("gate/private_usertrade.json");
    const auto r = p.decode(f.view(), Timestamp{7}, Cycles{9}, s.span());
    REQUIRE(r.ok());
    CHECK(r.count == 2);
    CHECK(r.order_kind == OrderEventKind::Fill);
    const auto& m = s.as<OrderFillMsg>();
    CHECK(m.hdr.type == EventType::OrderFill);
    CHECK(m.hdr.instrument == u.nvda);
    CHECK(m.cl_ord_id == ClientOrderId{1});
    CHECK(m.venue_order_id.view() == "74046514");
    CHECK(m.exec_id.view() == "3335259");
    CHECK(m.price == Price::from_decimal("234.1").value());
    CHECK(m.qty == Qty::from_int(1));
    CHECK(m.side == Side::Buy);
    CHECK(m.liquidity == Liquidity::Maker);
    CHECK(m.fee == Notional::from_decimal("0.04682").value());
    CHECK(m.fee_asset == FeeAsset::Quote);
    CHECK(m.cum_qty.is_zero());  // the connector fills these from its shadow
    CHECK(m.leaves_qty.is_zero());
    CHECK(m.hdr.exch_ts.ns == 1791009985300LL * 1'000'000);
    const auto& m2 = *reinterpret_cast<const OrderFillMsg*>(s.span().data() + sizeof(OrderFillMsg));
    CHECK(m2.exec_id.view() == "3335260");
    CHECK(m2.side == Side::Sell);
    CHECK(m2.qty == Qty::from_int(2));
    CHECK(m2.liquidity == Liquidity::Taker);
    CHECK(m2.fee == Notional::from_decimal("-0.001").value());  // a rebate
    CHECK_FALSE(m2.cl_ord_id.valid());
  }
  {
    // One single-mode position; the dual-mode row is counted, not decoded.
    const auto f = padded_fixture("gate/private_position.json");
    const auto r = p.decode(f.view(), Timestamp{7}, Cycles{9}, s.span());
    REQUIRE(r.ok());
    CHECK(r.count == 1);
    CHECK(r.order_kind == OrderEventKind::Position);
    const auto& m = s.as<PositionUpdateMsg>();
    CHECK(m.hdr.instrument == u.nvda);
    CHECK(m.qty == -Qty::from_int(1));
    CHECK(m.avg_px == Price::from_decimal("234.10000001").value());  // 8 decimals kept
    CHECK(m.hdr.exch_ts.ns == 1791009986100LL * 1'000'000);
    CHECK(p.stats().positions == 2);
    CHECK(p.stats().dual_positions == 1);
  }
  {
    // Balances: a fee entry asks for a refresh; a funding entry naming the contract is a payment.
    const auto f = padded_fixture("gate/private_balance.json");
    const auto r = p.decode(f.view(), Timestamp{7}, Cycles{9}, s.span());
    REQUIRE(r.ok());
    CHECK(r.balance_changed);
    CHECK(r.count == 1);
    const auto& m = s.as<FundingMsg>();
    CHECK(m.hdr.type == EventType::Funding);
    CHECK(m.hdr.instrument == u.nvda);
    CHECK(m.amount == Notional::from_decimal("0.002").value());
    CHECK(m.asset.view() == "USDT");
    CHECK(m.funding_id.view() == "1791014400000:NVDA_USDT");
    CHECK(m.hdr.exch_ts.ns == 1791014400000LL * 1'000'000);
    CHECK(p.stats().funding == 1);
    CHECK(p.stats().balances == 2);
  }
  {
    const PaddedJson sub(
        R"({"time":1,"time_ms":1000,"channel":"futures.orders","event":"subscribe","result":{"status":"success"}})");
    auto r = p.decode(sub.view(), Timestamp{7}, Cycles{9}, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(r.control == ControlOp::Subscribe);
    CHECK(r.control_success);
    CHECK(r.channel == "futures.orders");
    const PaddedJson bad(
        R"({"time":1,"time_ms":1000,"channel":"futures.orders","event":"subscribe","error":{"code":4,"message":"authentication fail"},"result":{"status":"fail"}})");
    r = p.decode(bad.view(), Timestamp{7}, Cycles{9}, s.span());
    CHECK(r.status == ParseStatus::Error);
    CHECK_FALSE(r.control_success);
    CHECK(r.error_code == 4);
    const auto login = padded_fixture("gate/api_login_ok.json");
    r = p.decode(login.view(), Timestamp{7}, Cycles{9}, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(r.control == ControlOp::Api);
    const auto pong = padded_fixture("gate/pong.json");
    r = p.decode(pong.view(), Timestamp{7}, Cycles{9}, s.span());
    CHECK(r.control == ControlOp::Pong);
    // A public push on this connection is not private data.
    const auto bt = padded_fixture("gate/book_ticker.json");
    CHECK(p.decode(bt.view(), Timestamp{7}, Cycles{9}, s.span()).status == ParseStatus::Ignored);
  }
  CHECK(cl_ord_id_of_text("t-fm000000000001") == ClientOrderId{1});
  CHECK_FALSE(cl_ord_id_of_text("api"));
  CHECK_FALSE(cl_ord_id_of_text("t-mine"));
}
