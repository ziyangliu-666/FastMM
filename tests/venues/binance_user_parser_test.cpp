#include "fastmm/venues/binance/binance_user_parser.hpp"

#include "venue_test_util.hpp"

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {
const Timestamp kRecv{1'700'000'000'000'000'000LL};
const Cycles kT0{42};
const ClientOrderId kId1 = decode_cl_ord_id("fm000100000001").value();
}  // namespace

TEST_CASE("binance.user: executionReport NEW -> OrderAckMsg") {
  TestUniverse u;
  BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const auto r =
      p.decode(padded_fixture("binance/exec_report_new.json").view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::Ack);
  CHECK(r.count == 1);
  CHECK(r.len == sizeof(OrderAckMsg));
  const auto& m = s.as<OrderAckMsg>();
  CHECK(m.hdr.type == EventType::OrderAck);
  CHECK(m.hdr.len == sizeof(OrderAckMsg));
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == VenueId{0});
  CHECK(m.hdr.recv_ts == kRecv);
  CHECK(m.hdr.t0_cycles == kT0);
  CHECK(m.hdr.exch_ts.ns == 1789295199990LL * 1'000'000);  // T (transaction time)
  CHECK(m.cl_ord_id == kId1);
  CHECK(m.venue_order_id.view() == "4293153");
}

TEST_CASE("binance.user: executionReport TRADE -> OrderFillMsg") {
  TestUniverse u;
  BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const auto r =
      p.decode(padded_fixture("binance/exec_report_trade.json").view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::Fill);
  const auto& m = s.as<OrderFillMsg>();
  CHECK(m.hdr.type == EventType::OrderFill);
  CHECK(m.hdr.len == sizeof(OrderFillMsg));
  CHECK(m.cl_ord_id == kId1);
  CHECK(m.venue_order_id.view() == "4293153");
  CHECK(m.exec_id.view() == "388600");
  CHECK(m.price == Price::from_int(70000));
  CHECK(m.qty == Qty::from_decimal("0.0004").value());
  CHECK(m.cum_qty == Qty::from_decimal("0.0004").value());
  CHECK(m.leaves_qty == Qty::from_decimal("0.0006").value());
  CHECK(m.fee == Notional::from_decimal("0.0000004").value());
  CHECK(m.fee_asset == FeeAsset::Base);  // N = "BTC": a buy pays commission in the base asset
  CHECK(m.side == Side::Buy);
  CHECK(m.liquidity == Liquidity::Maker);
  CHECK(m.hdr.exch_ts.ns == 1789295200990LL * 1'000'000);
}

TEST_CASE("binance.user: executionReport CANCELED uses C (original id) -> OrderCancelAckMsg") {
  TestUniverse u;
  BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const auto r =
      p.decode(padded_fixture("binance/exec_report_canceled.json").view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::CancelAck);
  const auto& m = s.as<OrderCancelAckMsg>();
  CHECK(m.hdr.type == EventType::OrderCancelAck);
  CHECK(m.cl_ord_id == kId1);  // "C", not the auto-generated "c"
  CHECK(m.venue_order_id.view() == "4293153");
  CHECK(m.cum_qty == Qty::from_decimal("0.0004").value());
  CHECK(p.stats().foreign_ids == 0);
}

TEST_CASE("binance.user: REJECTED / EXPIRED / TRADE_PREVENTION") {
  TestUniverse u;
  BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  auto r =
      p.decode(padded_fixture("binance/exec_report_rejected.json").view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::Reject);
  const auto& rj = s.as<OrderRejectMsg>();
  CHECK(rj.hdr.type == EventType::OrderReject);
  CHECK(rj.cl_ord_id == decode_cl_ord_id("fm000100000002").value());
  CHECK(rj.reason == RejectReason::InsufficientBalance);
  CHECK(rj.text.view() == "INSUFFICIENT_BALANCES");
  CHECK(rj.venue_code == 0);

  // raw listenKey-style payload (no envelope)
  r = p.decode(padded_fixture("binance/exec_report_expired.json").view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::Expired);
  const auto& ex = s.as<OrderExpiredMsg>();
  CHECK(ex.hdr.type == EventType::OrderExpired);
  CHECK(ex.cl_ord_id == decode_cl_ord_id("fm000100000003").value());
  CHECK(ex.venue_order_id.view() == "4293155");
  CHECK(ex.cum_qty.is_zero());

  const PaddedJson stp(
      R"({"stream":"lk","data":{"e":"executionReport","E":1,"s":"BTCUSDT","c":"fm000100000009","S":"SELL","o":"LIMIT","f":"GTC","q":"1","p":"1","C":"","x":"TRADE_PREVENTION","X":"EXPIRED_IN_MATCH","r":"NONE","i":7,"l":"0","z":"0.5","L":"0","n":"0","N":null,"T":2,"t":-1,"m":false}})");
  r = p.decode(stp.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::Expired);
  CHECK(s.as<OrderExpiredMsg>().cum_qty == Qty::from_decimal("0.5").value());
}

TEST_CASE("binance.user: foreign client ids, unknown symbol, malformed, ignored events") {
  TestUniverse u;
  BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const PaddedJson manual(
      R"({"e":"executionReport","E":1,"s":"BTCUSDT","c":"web_abc","S":"BUY","o":"LIMIT","f":"GTC","q":"1","p":"1","C":"","x":"NEW","X":"NEW","r":"NONE","i":9,"l":"0","z":"0","L":"0","n":"0","N":null,"T":2,"t":-1,"m":false}})");
  auto r = p.decode(manual.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK_FALSE(s.as<OrderAckMsg>().cl_ord_id.valid());
  CHECK(p.stats().foreign_ids == 1);

  const PaddedJson unknown(
      R"({"e":"executionReport","E":1,"s":"DOGEUSDT","c":"fm000100000001","S":"BUY","o":"LIMIT","f":"GTC","q":"1","p":"1","C":"","x":"NEW","X":"NEW","r":"NONE","i":9,"l":"0","z":"0","L":"0","n":"0","N":null,"T":2,"t":-1,"m":false}})");
  CHECK(p.decode(unknown.view(), kRecv, kT0, s.span()).status == ParseStatus::UnknownSymbol);
  CHECK(p.decode(padded_fixture("binance/exec_report_malformed.json").view(), kRecv, kT0, s.span())
            .status == ParseStatus::Malformed);
  CHECK(p.decode(padded_fixture("binance/ws_api_order_place_ok.json").view(), kRecv, kT0, s.span())
            .status == ParseStatus::Ignored);
  const PaddedJson bal(
      R"({"subscriptionId":0,"event":{"e":"balanceUpdate","E":1,"a":"BTC","d":"1","T":1}})");
  CHECK(p.decode(bal.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  const PaddedJson garbage("{");
  CHECK(p.decode(garbage.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
}

TEST_CASE("binance.user: outboundAccountPosition -> PositionUpdateMsg per base asset") {
  TestUniverse u;
  BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const auto r = p.decode(
      padded_fixture("binance/outbound_account_position.json").view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::Position);
  CHECK(r.count == 1);  // BTC matches BTCUSDT on venue 0 only (ETHUSDT base is ETH)
  CHECK(r.len == sizeof(PositionUpdateMsg));
  const auto& m = s.as<PositionUpdateMsg>();
  CHECK(m.hdr.type == EventType::PositionUpdate);
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.qty == Qty::from_decimal("1.2355").value());  // free 1.2345 + locked 0.001
  CHECK(m.avg_px.is_zero());
  CHECK(m.hdr.exch_ts.ns == 1789295201001LL * 1'000'000);
}

TEST_CASE("binance.user: the commission asset of a fill is classified as base, quote or other") {
  TestUniverse u;
  BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const auto fill_with = [&](const char* n, const char* asset) {
    const std::string json =
        std::string(
            R"({"e":"executionReport","E":1,"s":"BTCUSDT","c":"fm000100000001","S":"SELL","o":"LIMIT_MAKER","f":"GTC","q":"0.001","p":"70000","C":"","x":"TRADE","X":"FILLED","r":"NONE","i":9,"l":"0.001","z":"0.001","L":"70000","n":")") +
        n + R"(","N":)" + asset + R"(,"T":2,"t":11,"m":true})";
    const PaddedJson padded(json);
    REQUIRE(p.decode(padded.view(), kRecv, kT0, s.span()).status == ParseStatus::Ok);
    return s.as<OrderFillMsg>();
  };
  auto m = fill_with("0.07", R"("USDT")");
  CHECK(m.fee_asset == FeeAsset::Quote);
  CHECK(m.fee == Notional::from_decimal("0.07").value());
  m = fill_with("0.0001", R"("BNB")");
  CHECK(m.fee_asset == FeeAsset::Other);
  m = fill_with("0", "null");
  CHECK(m.fee_asset == FeeAsset::Quote);
  CHECK(m.fee.is_zero());
}
