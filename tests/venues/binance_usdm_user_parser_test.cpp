#include "fastmm/venues/binance_usdm/binance_usdm_user_parser.hpp"

#include "venue_test_util.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance_usdm;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {
const Timestamp kRecv{1'700'000'000'000'000'000LL};
const Cycles kT0{42};
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
ClientOrderId id(const char* s) {
  return decode_cl_ord_id(s).value();
}
}  // namespace

TEST_CASE("binance_usdm.user: ORDER_TRADE_UPDATE NEW, CANCELED and EXPIRED") {
  TestUniverse u;
  BinanceUsdmUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  {
    const auto fx = padded_fixture("binance_usdm/order_update_new.json");
    const UserDecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.count == 1);
    CHECK(r.order_kind == OrderEventKind::Ack);
    const auto& m = s.as<OrderAckMsg>();
    CHECK(m.hdr.type == EventType::OrderAck);
    CHECK(m.hdr.instrument == InstrumentId{0});
    CHECK(m.cl_ord_id == id("fm000100000001"));
    CHECK(m.venue_order_id.view() == "8886774");
    CHECK(m.hdr.exch_ts.ns == 1789467600099LL * 1'000'000);
    CHECK(m.hdr.recv_ts == kRecv);
  }
  {
    const auto fx = padded_fixture("binance_usdm/order_update_canceled.json");
    const UserDecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.order_kind == OrderEventKind::CancelAck);
    const auto& m = s.as<OrderCancelAckMsg>();
    CHECK(m.cl_ord_id == id("fm000100000001"));
    CHECK(m.cum_qty == qty("0.0004"));
  }
  {
    // A GTX order that would take expires ("Order Update" execution type EXPIRED).
    const auto fx = padded_fixture("binance_usdm/order_update_expired.json");
    const UserDecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.order_kind == OrderEventKind::Expired);
    const auto& m = s.as<OrderExpiredMsg>();
    CHECK(m.cl_ord_id == id("fm000100000002"));
    CHECK(m.cum_qty.is_zero());
  }
  CHECK(p.stats().order_updates == 3);
}

TEST_CASE("binance_usdm.user: TRADE -> OrderFillMsg with the USDT commission as quote") {
  TestUniverse u;
  BinanceUsdmUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance_usdm/order_update_trade.json");
  const UserDecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::Fill);
  const auto& f = s.as<OrderFillMsg>();
  CHECK(f.hdr.type == EventType::OrderFill);
  CHECK(f.cl_ord_id == id("fm000100000001"));
  CHECK(f.exec_id.view() == "537853300");
  CHECK(f.price == px("76980.00"));
  CHECK(f.qty == qty("0.0004"));
  CHECK(f.cum_qty == qty("0.0004"));
  CHECK(f.leaves_qty == qty("0.0006"));
  CHECK(f.fee == Notional::from_decimal("0.0061584").value());
  CHECK(f.fee_asset == FeeAsset::Quote);
  CHECK(f.side == Side::Buy);
  CHECK(f.liquidity == Liquidity::Maker);

  // BNB commission cannot be valued by the engine; a liquidation fill has a foreign id but still
  // changes the position, so it is forwarded.
  std::string liq = fastmm::test::fixture("binance_usdm/order_update_trade.json");
  liq.replace(liq.find("fm000100000001"), 14, "autoclose-1789");
  liq.replace(liq.find(R"("N":"USDT")"), 10, R"("N":"BNB")");
  liq.replace(liq.find(R"("x":"TRADE")"), 11, R"("x":"CALCULATED")");
  const PaddedJson liq_json(liq);
  const UserDecodeResult lr = p.decode(liq_json.view(), kRecv, kT0, s.span());
  REQUIRE(lr.status == ParseStatus::Ok);
  const auto& lf = s.as<OrderFillMsg>();
  CHECK_FALSE(lf.cl_ord_id.valid());
  CHECK(lf.fee_asset == FeeAsset::Other);
  CHECK(p.stats().foreign_ids == 1);

  // Without `t` the exec id used to collapse to "0" for every execution, and the OMS dedupe
  // window dropped the second fill of the order.
  std::string base = fastmm::test::fixture("binance_usdm/order_update_trade.json");
  base.replace(base.find(R"("t":537853300,)"), 14, "");
  const PaddedJson no_t1(base);
  REQUIRE(p.decode(no_t1.view(), kRecv, kT0, s.span()).status == ParseStatus::Ok);
  const ExecId first = s.as<OrderFillMsg>().exec_id;
  std::string more = base;
  more.replace(more.find(R"("z":"0.0004")"), 12, R"("z":"0.0007")");
  const PaddedJson no_t2(more);
  REQUIRE(p.decode(no_t2.view(), kRecv, kT0, s.span()).status == ParseStatus::Ok);
  CHECK_FALSE(first.empty());
  CHECK(first.view() != s.as<OrderFillMsg>().exec_id.view());
}

TEST_CASE("binance_usdm.user: ACCOUNT_UPDATE -> one-way positions, hedge and unknown skipped") {
  TestUniverse u;
  BinanceUsdmUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  const auto fx = padded_fixture("binance_usdm/account_update.json");
  const UserDecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.order_kind == OrderEventKind::Position);
  REQUIRE(r.count == 1);  // ETHUSDT is LONG (hedge mode), LTCUSDT is not configured
  CHECK(r.len == sizeof(PositionUpdateMsg));
  const auto& m = s.as<PositionUpdateMsg>();
  CHECK(m.hdr.type == EventType::PositionUpdate);
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.qty == qty("-0.0004"));
  CHECK(m.avg_px == px("76980.0"));
  CHECK(m.hdr.exch_ts.ns == 1789467601199LL * 1'000'000);
  CHECK(p.stats().hedge_positions == 1);
}

TEST_CASE("binance_usdm.user: listenKeyExpired, foreign ids, AMENDMENT, balance-only, malformed") {
  TestUniverse u;
  BinanceUsdmUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  {
    const auto fx = padded_fixture("binance_usdm/listen_key_expired.json");
    const UserDecodeResult r = p.decode(fx.view(), kRecv, kT0, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(r.listen_key_expired);
  }
  std::string base = fastmm::test::fixture("binance_usdm/order_update_new.json");
  {
    std::string manual = base;
    manual.replace(manual.find("fm000100000001"), 14, "web_7Gd1kLp9Qx");
    const PaddedJson j(manual);
    CHECK(p.decode(j.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  }
  {
    std::string amend = base;
    amend.replace(amend.find(R"("x":"NEW")"), 9, R"("x":"AMENDMENT")");
    const PaddedJson j(amend);
    CHECK(p.decode(j.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  }
  {
    std::string other = base;
    other.replace(other.find("BTCUSDT"), 7, "XRPUSDT");
    const PaddedJson j(other);
    CHECK(p.decode(j.view(), kRecv, kT0, s.span()).status == ParseStatus::UnknownSymbol);
  }
  {
    const PaddedJson funding(
        R"({"e":"ACCOUNT_UPDATE","E":1,"T":1,"a":{"m":"FUNDING_FEE","B":[{"a":"USDT","wb":"10","cw":"10","bc":"0"}]}})");
    CHECK(p.decode(funding.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  }
  {
    const PaddedJson truncated(R"({"e":"ORDER_TRADE_UPDATE","E":1,"T":1,"o":{"s":"BTCUSDT"}})");
    CHECK(p.decode(truncated.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
    const PaddedJson garbage("{not json");
    CHECK(p.decode(garbage.view(), kRecv, kT0, s.span()).status == ParseStatus::Malformed);
    const PaddedJson margin_call(R"({"e":"MARGIN_CALL","E":1})");
    CHECK(p.decode(margin_call.view(), kRecv, kT0, s.span()).status == ParseStatus::Ignored);
  }
}

TEST_CASE("binance_usdm.user: an ACCOUNT_UPDATE for funding says so, crossed or isolated") {
  TestUniverse u;
  BinanceUsdmUserParser p(u.symbols, u.instruments, VenueId{0});
  Scratch s;
  {
    // Crossed: the balance and the symbol, no position (the connector books it from the income
    // history, which has an id for it).
    const PaddedJson crossed(
        R"({"e":"ACCOUNT_UPDATE","E":1789500000001,"T":1789500000000,"a":{"m":"FUNDING_FEE","S":"BTCUSDT","B":[{"a":"USDT","wb":"4999.625","cw":"4999.625","bc":"-0.375"}]}})");
    const UserDecodeResult r = p.decode(crossed.view(), kRecv, kT0, s.span());
    CHECK(r.status == ParseStatus::Ignored);
    CHECK(r.funding);
    CHECK(r.count == 0);
  }
  {
    // Isolated: the position too, which is decoded as before.
    const PaddedJson isolated(
        R"({"e":"ACCOUNT_UPDATE","E":1789500000001,"T":1789500000000,"a":{"m":"FUNDING_FEE","S":"BTCUSDT","B":[{"a":"USDT","wb":"4999.625","cw":"99.625","bc":"-0.375"}],"P":[{"s":"BTCUSDT","pa":"0.002","ep":"70000.0","cr":"0","up":"0.1","mt":"isolated","iw":"99.625","ps":"BOTH"}]}})");
    const UserDecodeResult r = p.decode(isolated.view(), kRecv, kT0, s.span());
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(r.funding);
    REQUIRE(r.count == 1);
    CHECK(s.as<PositionUpdateMsg>().qty == qty("0.002"));
  }
  {
    const auto fx = padded_fixture("binance_usdm/account_update.json");
    CHECK_FALSE(p.decode(fx.view(), kRecv, kT0, s.span()).funding);  // reason ORDER
  }
  CHECK(p.stats().funding_events == 2);
}
