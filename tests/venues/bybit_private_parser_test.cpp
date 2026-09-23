#include "fastmm/venues/bybit/bybit_private_parser.hpp"

#include "venue_test_util.hpp"

#include <string_view>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {
constexpr VenueId kBybit{1};
const InstrumentId kBtc{2};
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}
MdDecodeResult decode(BybitPrivateParser& p, const char* fixture, Scratch& s) {
  const auto j = padded_fixture(fixture);
  return p.decode(j.view(), Timestamp{11}, Cycles{12}, s.span());
}
}  // namespace

TEST_CASE("bybit.private_parser: order topic statuses map to order events") {
  TestUniverse u;
  BybitPrivateParser p(u.symbols, u.instruments, kBybit);
  Scratch s;

  auto r = decode(p, "bybit/private_order_new.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);
  const auto& ack = s.as<OrderAckMsg>();
  CHECK(ack.hdr.type == EventType::OrderAck);
  CHECK(ack.hdr.instrument == kBtc);
  CHECK(ack.cl_ord_id == decode_cl_ord_id("fm000100000001").value());
  CHECK(ack.venue_order_id.view() == "2012345678901234567");
  CHECK(ack.hdr.exch_ts.ns == 1789299700457LL * 1'000'000);
  CHECK(ack.hdr.recv_ts.ns == 11);

  r = decode(p, "bybit/private_order_cancelled.json", s);
  REQUIRE(r.ok());
  const auto& cx = s.as<OrderCancelAckMsg>();
  CHECK(cx.hdr.type == EventType::OrderCancelAck);
  CHECK(cx.cum_qty == qty("0.0004"));

  r = decode(p, "bybit/private_order_rejected.json", s);
  REQUIRE(r.ok());
  const auto& rj = s.as<OrderRejectMsg>();
  CHECK(rj.hdr.type == EventType::OrderReject);
  CHECK(rj.reason == RejectReason::PostOnlyWouldCross);
  CHECK(rj.text.view() == "EC_PostOnlyWillTakeLiquidity");

  // Filled: the fill itself arrives on `execution`, the order update is not forwarded.
  r = decode(p, "bybit/private_order_filled.json", s);
  CHECK(r.status == ParseStatus::Ignored);
}

TEST_CASE("bybit.private_parser: execution -> fill, wallet -> position, control frames") {
  TestUniverse u;
  BybitPrivateParser p(u.symbols, u.instruments, kBybit);
  Scratch s;
  auto r = decode(p, "bybit/private_execution.json", s);
  REQUIRE(r.ok());
  const auto& f = s.as<OrderFillMsg>();
  CHECK(f.hdr.type == EventType::OrderFill);
  CHECK(f.cl_ord_id == decode_cl_ord_id("fm000100000003").value());
  CHECK(f.exec_id.view() == "0ab1bdf7-4219-438b-b30a-32ec863018f7");
  CHECK(f.price == Price::from_decimal("77140.5").value());
  CHECK(f.qty == qty("0.001"));
  CHECK(f.cum_qty == qty("0.001"));
  CHECK(f.leaves_qty.is_zero());
  CHECK(f.fee == Notional::from_decimal("0.000001").value());
  CHECK(f.side == Side::Buy);
  CHECK(f.fee_asset == FeeAsset::Base);  // a spot buy without feeCurrency: fee in BTC
  CHECK(f.liquidity == Liquidity::Taker);
  CHECK(f.hdr.exch_ts.ns == 1789299703453LL * 1'000'000);

  // An execution without execFee must report a zero fee, not the previous fill's: the venue
  // decodes every frame into the same scratch buffer.
  static constexpr std::string_view kNoFee =
      R"({"topic":"execution","id":"e2","creationTime":1789299703470,"data":[{"category":"spot",)"
      R"("symbol":"BTCUSDT","execId":"ex-nofee","execPrice":"77140.5","execQty":"0.001",)"
      R"("execType":"Trade","orderId":"2012345678901234569","orderLinkId":"fm000100000003",)"
      R"("orderQty":"0.001","side":"Buy","leavesQty":"0","execTime":"1789299703463",)"
      R"("isMaker":false}]})";
  {
    const PaddedJson j(kNoFee);
    r = p.decode(j.view(), Timestamp{11}, Cycles{12}, s.span());
    REQUIRE(r.ok());
    CHECK(s.as<OrderFillMsg>().fee.is_zero());
    CHECK(s.as<OrderFillMsg>().fee_asset == FeeAsset::Quote);  // no fee: nothing to convert
  }

  r = decode(p, "bybit/private_wallet.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);  // BTC -> BTCUSDT on venue 1; USDT is no instrument's base
  const auto& pos = s.as<PositionUpdateMsg>();
  CHECK(pos.hdr.instrument == kBtc);
  CHECK(pos.qty == qty("1.001"));  // spotBorrow 0; locked coins are part of walletBalance
  // Spot borrows are deducted: the position is the net holding (equity = walletBalance -
  // spotBorrow).
  r = decode(p, "bybit/private_wallet_borrow.json", s);
  CHECK(s.as<PositionUpdateMsg>().qty == qty("0.801"));

  r = decode(p, "bybit/private_auth_ok.json", s);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.control == ControlOp::Auth);
  CHECK(r.control_success);
  r = decode(p, "bybit/auth_error.json", s);
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.control == ControlOp::Auth);
  CHECK(r.ret_msg == "Invalid sign");
  const venues::PaddedJson bad(R"({"topic":"order","data":[{"category":"spot"}]})");
  CHECK(p.decode(bad.view(), Timestamp{}, Cycles{}, s.span()).status == ParseStatus::Malformed);
}
