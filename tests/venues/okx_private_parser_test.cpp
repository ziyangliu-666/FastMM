// OKX v5 private stream: the orders, positions and balance_and_position channels into order
// events. Payloads follow the channel pages (https://www.okx.com/docs-v5/en/#order-book-trading-
// trade-ws-order-channel, #trading-account-websocket-positions-channel, read 2026-09-26) with
// FastMM client ids.
#include "fastmm/venues/okx/okx_private_parser.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/padded_json.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::okx;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kOkx{1};
const InstrumentId kBtc{0};

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    Instrument i = make_instrument("BTC-USDT-SWAP", 1, "BTC", "USDT");
    i.contract_multiplier = Qty::from_decimal("0.01").value();
    REQUIRE(instruments.add(i));
    REQUIRE(symbols.build(instruments));
  }
};

// One orders-channel item; `extra` is spliced in before the closing brace.
std::string order(const char* state,
                  const char* cl,
                  const char* acc,
                  const char* fill_sz,
                  const char* trade_id,
                  const std::string& extra = {}) {
  return std::string(
             R"({"arg":{"channel":"orders","instType":"SWAP","uid":"77"},"data":[{"instType":"SWAP","instId":"BTC-USDT-SWAP","tgtCcy":"","ccy":"","ordId":"680800019749904384","clOrdId":")") +
         cl +
         R"(","tag":"","px":"60000.1","pxUsd":"","pxVol":"","pxType":"","sz":"3","notionalUsd":"1800","ordType":"post_only","side":"sell","posSide":"net","tdMode":"cross","accFillSz":")" +
         acc + R"(","fillNotionalUsd":"","avgPx":"60000.1","state":")" + state +
         R"(","lever":"10","pnl":"0","feeCcy":"USDT","fee":"-0.018","rebateCcy":"USDT","rebate":"0","category":"normal","uTime":"1789299703470","cTime":"1789299700444","source":"","reduceOnly":"false","cancelSource":"","quickMgnType":"","stpId":"","stpMode":"cancel_maker","attachAlgoClOrdId":"","lastPx":"60000.1","isTpLimit":"false","slTriggerPx":"","slTriggerPxType":"","tpOrdPx":"","tpTriggerPx":"","tpTriggerPxType":"","slOrdPx":"","fillPx":"60000.1","tradeId":")" +
         trade_id + R"(","fillSz":")" + fill_sz +
         R"(","fillTime":"1789299703453","fillPnl":"0","fillFee":"-0.018","fillFeeCcy":"USDT","execType":"T","fillPxVol":"","fillPxUsd":"","fillMarkVol":"","fillFwdPx":"","fillMarkPx":"","amendSource":"","reqId":"","amendResult":"","code":"0","msg":"","algoId":"","algoClOrdId":"")" +
         extra + "}]}";
}

template <class M>
const M& nth(const Scratch& s, std::uint32_t i) {
  std::uint32_t off = 0;
  for (std::uint32_t k = 0; k < i; ++k)
    off += reinterpret_cast<const EventHeader*>(s.buf + off)->len;
  return *reinterpret_cast<const M*>(s.buf + off);
}

PrivateDecodeResult decode(OkxPrivateParser& p, const std::string& frame, Scratch& s) {
  const PaddedJson j(frame);
  return p.decode(j.view(), Timestamp{1}, Cycles{}, s.span());
}

// Replaces the first `"key":"..."` value in a frame.
std::string with(std::string f, const std::string& key, const std::string& value) {
  const std::string needle = "\"" + key + "\":\"";
  const std::size_t p = f.find(needle);
  REQUIRE(p != std::string::npos);
  const std::size_t s = p + needle.size();
  f.replace(s, f.find('"', s) - s, value);
  return f;
}

}  // namespace

TEST_CASE("okx.private_parser: order states map to order events") {
  Universe u;
  OkxPrivateParser p(u.symbols, u.instruments, kOkx);
  Scratch s;
  const ClientOrderId cl = decode_cl_ord_id("fm000100000001").value();

  PrivateDecodeResult r = decode(p, order("live", "fm000100000001", "0", "0", ""), s);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  const auto& ack = s.as<OrderAckMsg>();
  CHECK(ack.hdr.type == EventType::OrderAck);
  CHECK(ack.cl_ord_id == cl);
  CHECK(ack.venue_order_id.view() == "680800019749904384");
  CHECK(ack.flags == 0);
  CHECK(ack.hdr.exch_ts == Timestamp{1789299703470LL * 1'000'000});

  // A fill: contracts, the tradeId as execution id, the fee sign turned round.
  r = decode(p, order("partially_filled", "fm000100000001", "1", "1", "4463701411"), s);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  const auto& f = s.as<OrderFillMsg>();
  CHECK(f.hdr.type == EventType::OrderFill);
  CHECK(f.cl_ord_id == cl);
  CHECK(f.exec_id.view() == "4463701411");
  CHECK(f.side == Side::Sell);
  CHECK(f.price == Price::from_decimal("60000.1").value());
  CHECK(f.qty == Qty::from_int(1));
  CHECK(f.cum_qty == Qty::from_int(1));
  CHECK(f.leaves_qty == Qty::from_int(2));
  CHECK(f.fee == Notional::from_decimal("0.018").value());  // fillFee -0.018: charged
  CHECK(f.fee_asset == FeeAsset::Quote);
  CHECK(f.liquidity == Liquidity::Taker);
  CHECK(f.hdr.exch_ts == Timestamp{1789299703453LL * 1'000'000});
  // A maker rebate (positive fillFee) is a negative fee.
  std::string maker =
      with(order("filled", "fm000100000001", "3", "2", "4463701412"), "execType", "M");
  maker = with(maker, "fillFee", "0.004");
  r = decode(p, maker, s);
  REQUIRE(r.count == 1);
  CHECK(s.as<OrderFillMsg>().fee == Notional::from_decimal("-0.004").value());
  CHECK(s.as<OrderFillMsg>().liquidity == Liquidity::Maker);
  CHECK(s.as<OrderFillMsg>().leaves_qty.is_zero());

  // Cancelled by the user: a cancel ack with the filled quantity.
  r = decode(p, with(order("canceled", "fm000100000001", "1", "0", ""), "cancelSource", "1"), s);
  REQUIRE(r.count == 1);
  const auto& ca = s.as<OrderCancelAckMsg>();
  CHECK(ca.hdr.type == EventType::OrderCancelAck);
  CHECK(ca.cum_qty == Qty::from_int(1));
  // A post-only order that would have taken (cancelSource 31, pushed as canceled only since
  // 2026-08-20), and an IOC remainder, expired rather than cancelled.
  r = decode(p, with(order("canceled", "fm000100000001", "0", "0", ""), "cancelSource", "31"), s);
  REQUIRE(r.count == 1);
  CHECK(s.as<OrderExpiredMsg>().hdr.type == EventType::OrderExpired);
  r = decode(p, with(order("canceled", "fm000100000001", "0", "0", ""), "cancelSource", "14"), s);
  CHECK(s.as<OrderExpiredMsg>().hdr.type == EventType::OrderExpired);
  r = decode(
      p, with(order("mmp_canceled", "fm000100000001", "0", "0", ""), "cancelSource", "38"), s);
  CHECK(s.as<OrderCancelAckMsg>().hdr.type == EventType::OrderCancelAck);

  // A fill that finishes the order and the cancel in one push: the fill first.
  r = decode(
      p, with(order("canceled", "fm000100000001", "3", "1", "4463701413"), "cancelSource", "1"), s);
  REQUIRE(r.count == 2);
  CHECK(nth<OrderFillMsg>(s, 0).hdr.type == EventType::OrderFill);
  CHECK(nth<OrderCancelAckMsg>(s, 1).hdr.type == EventType::OrderCancelAck);

  // Foreign client ids reach the OMS as unknown.
  r = decode(p, order("live", "manual123", "0", "0", ""), s);
  REQUIRE(r.count == 1);
  CHECK_FALSE(s.as<OrderAckMsg>().cl_ord_id.valid());
  CHECK(p.stats().foreign_ids == 1);
}

TEST_CASE("okx.private_parser: amend results come under the id in reqId") {
  Universe u;
  OkxPrivateParser p(u.symbols, u.instruments, kOkx);
  Scratch s;
  std::string ok = order("live", "fm000100000001", "0", "0", "");
  ok = with(ok, "reqId", "fm000100000002");
  ok = with(ok, "amendResult", "0");
  PrivateDecodeResult r = decode(p, ok, s);
  REQUIRE(r.count == 1);
  const auto& ack = s.as<OrderAckMsg>();
  CHECK(ack.hdr.type == EventType::OrderAck);
  CHECK(ack.cl_ord_id == decode_cl_ord_id("fm000100000002").value());
  CHECK(ack.flags == OrderAckMsg::kAmendedInPlace);  // the fills booked against it stay
  std::string failed = with(ok, "amendResult", "-1");
  failed = with(failed, "code", "51511");
  failed = with(failed, "msg", "Operation failed as the price is invalid for a post-only order");
  r = decode(p, failed, s);
  REQUIRE(r.count == 1);
  const auto& rej = s.as<OrderRejectMsg>();
  CHECK(rej.hdr.type == EventType::OrderReject);
  CHECK(rej.cl_ord_id == decode_cl_ord_id("fm000100000002").value());
  CHECK(rej.reason == RejectReason::PostOnlyWouldCross);
  CHECK(rej.venue_code == 51511);
}

TEST_CASE("okx.private_parser: positions, funding events and control frames") {
  Universe u;
  OkxPrivateParser p(u.symbols, u.instruments, kOkx);
  Scratch s;
  PrivateDecodeResult r = decode(
      p,
      R"({"arg":{"channel":"positions","instType":"SWAP","uid":"77"},"eventType":"snapshot","curPage":1,"lastPage":true,"data":[{"adl":"1","availPos":"","avgPx":"60123.4","cTime":"1619507758793","ccy":"USDT","instId":"BTC-USDT-SWAP","instType":"SWAP","lever":"10","liqPx":"","markPx":"60200","mgnMode":"cross","pos":"-3.5","posCcy":"","posId":"1","posSide":"net","upl":"-0.27","uTime":"1789299703499","pTime":"1789299703500"},{"instId":"ETH-USDT-SWAP","posSide":"net","pos":"1","avgPx":"3000","uTime":"1"}]})",
      s);
  REQUIRE(r.status == ParseStatus::Ok);
  CHECK(r.positions_snapshot);
  REQUIRE(r.count == 1);  // ETH is not in the symbol table
  const auto& pos = s.as<PositionUpdateMsg>();
  CHECK(pos.hdr.instrument == kBtc);
  CHECK(pos.qty == Qty::from_decimal("-3.5").value());
  CHECK(pos.avg_px == Price::from_decimal("60123.4").value());
  CHECK(p.stats().unknown_symbol == 1);

  // Long/short mode positions are counted, not decoded.
  r = decode(
      p,
      R"({"arg":{"channel":"positions","instType":"SWAP","uid":"77"},"eventType":"event_update","data":[{"instId":"BTC-USDT-SWAP","posSide":"long","pos":"1","avgPx":"60000","uTime":"1"}]})",
      s);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK_FALSE(r.positions_snapshot);
  CHECK(p.stats().hedge_positions == 1);

  r = decode(
      p,
      R"({"arg":{"channel":"balance_and_position","uid":"77"},"data":[{"pTime":"1789300000000","eventType":"funding_fee","balData":[{"ccy":"USDT","cashBal":"100","uTime":"1"}],"posData":[],"trades":[]}]})",
      s);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.funding_event);
  r = decode(
      p,
      R"({"arg":{"channel":"balance_and_position","uid":"77"},"data":[{"pTime":"1","eventType":"filled","balData":[],"posData":[],"trades":[]}]})",
      s);
  CHECK_FALSE(r.funding_event);

  r = decode(p, R"({"event":"login","code":"0","msg":"","connId":"a4d3ae55"})", s);
  CHECK(r.control == ControlOp::Login);
  CHECK(r.control_success);
  r = decode(
      p, R"({"event":"error","code":"60024","msg":"Wrong passphrase","connId":"a4d3ae55"})", s);
  CHECK(r.control == ControlOp::Error);
  CHECK(r.code == 60024);
  CHECK(r.status == ParseStatus::Error);
  r = decode(
      p,
      R"({"id":"private","event":"subscribe","arg":{"channel":"orders","instType":"SWAP"},"connId":"a4d3ae55"})",
      s);
  CHECK(r.control == ControlOp::Subscribe);
  CHECK(r.channel == "orders");
  r = decode(
      p,
      R"({"event":"channel-conn-count","channel":"orders","connCount":"2","connId":"abcd1234"})",
      s);
  CHECK(r.control == ControlOp::ChannelConnCount);
  CHECK(r.control_success);
  r = decode(p, "pong", s);
  CHECK(r.control == ControlOp::Pong);
  CHECK(decode(p, R"({"arg":{"channel":"orders"},"data":[{"instId":1}]})", s).status ==
        ParseStatus::Malformed);
}
