// Coinbase Exchange user channel: received, match, done and change in the documented shapes
// (https://docs.cdp.coinbase.com/exchange/websocket-feed/channels, read 2026-09-30) with FastMM
// client_oids; orders of other software on the profile are ignored.
#include "fastmm/venues/coinbase/coinbase_private_parser.hpp"

#include "venue_test_util.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::coinbase;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kVenue{1};
constexpr const char* kOrder = "d50ec984-77a8-460a-b958-66f114b0de9b";
constexpr const char* kOther = "132fb6ae-456b-4654-b4e0-d681ac05cea1";
const std::string kOid = std::string(encode_client_oid(make_cl_ord_id(1, 7)).view());

struct Fixture {
  InstrumentTable instruments;
  SymbolTable symbols;
  std::unique_ptr<CoinbasePrivateParser> p;
  Scratch s;
  Fixture() {
    REQUIRE(instruments.add(venues::test::make_instrument("BTC-USD", 1, "BTC", "USD")));
    REQUIRE(symbols.build(instruments));
    p = std::make_unique<CoinbasePrivateParser>(symbols, instruments, kVenue);
  }
  PrivateDecodeResult decode(const std::string& frame) {
    const PaddedJson j(frame);
    return p->decode(j.view(), Timestamp{1}, Cycles{2}, s.span());
  }
  template <class M>
  const M& at(std::size_t i) const {
    std::size_t off = 0;
    for (std::size_t k = 0; k < i; ++k)
      off += reinterpret_cast<const EventHeader*>(s.buf + off)->len;
    return *reinterpret_cast<const M*>(s.buf + off);
  }
};

std::string received(const std::string& oid, const char* order = kOrder) {
  return R"({"type":"received","time":"2014-11-07T08:19:27.028459Z","product_id":"BTC-USD","sequence":10,"order_id":")" +
         std::string(order) +
         R"(","size":"1.34","price":"502.1","side":"buy","order_type":"limit","client_oid":")" +
         oid + R"(","user_id":"u","profile_id":"p"})";
}
std::string match(const char* maker, const char* taker, const char* size, const char* rate_key) {
  return R"({"type":"match","trade_id":10,"sequence":50,"maker_order_id":")" + std::string(maker) +
         R"(","taker_order_id":")" + taker +
         R"(","time":"2014-11-07T08:19:27.028459Z","product_id":"BTC-USD","size":")" + size +
         R"(","price":"400.23","side":"buy","user_id":"u","profile_id":"p",")" + rate_key +
         R"(":"0.005"})";
}
std::string done(const char* reason, const std::string& cancel_reason = {}) {
  return R"({"type":"done","time":"2014-11-07T08:19:27.028459Z","product_id":"BTC-USD","sequence":10,"price":"502.1","order_id":")" +
         std::string(kOrder) + R"(","reason":")" + reason +
         R"(","side":"buy","remaining_size":"0.34")" +
         (cancel_reason.empty() ? "" : R"(,"cancel_reason":)" + cancel_reason) + "}";
}

}  // namespace

TEST_CASE("coinbase.private_parser: received acks and tracks, match fills maker and taker") {
  Fixture f;
  PrivateDecodeResult r = f.decode(received(kOid));
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  const auto& ack = f.at<OrderAckMsg>(0);
  CHECK(ack.hdr.type == EventType::OrderAck);
  CHECK(ack.cl_ord_id == make_cl_ord_id(1, 7));
  CHECK(ack.venue_order_id.view() == kOrder);
  CHECK(ack.hdr.exch_ts == Timestamp{1415348367028459000});
  REQUIRE(f.p->find(kOrder) != nullptr);
  CHECK(f.p->find(kOrder)->size == Qty::from_decimal("1.34").value());

  // Maker: the match's side is the maker's; the fee is price x size x maker_fee_rate.
  r = f.decode(match(kOrder, kOther, "1", "maker_fee_rate"));
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  const auto& fill = f.at<OrderFillMsg>(0);
  CHECK(fill.cl_ord_id == make_cl_ord_id(1, 7));
  CHECK(fill.exec_id.view() == "10");
  CHECK(fill.side == Side::Buy);
  CHECK(fill.liquidity == Liquidity::Maker);
  CHECK(fill.qty == Qty::from_int(1));
  CHECK(fill.cum_qty == Qty::from_int(1));
  CHECK(fill.leaves_qty == Qty::from_decimal("0.34").value());
  CHECK(fill.fee == Notional::from_decimal("2.00115").value());
  CHECK(fill.fee_asset == FeeAsset::Quote);
  CHECK(fill.venue_order_id.view() == kOrder);

  // Taker: the other side, the taker rate.
  Fixture t;
  REQUIRE(t.decode(received(kOid, kOther)).status == ParseStatus::Ok);
  r = t.decode(match(kOrder, kOther, "0.5", "taker_fee_rate"));
  REQUIRE(r.count == 1);
  CHECK(t.at<OrderFillMsg>(0).side == Side::Sell);
  CHECK(t.at<OrderFillMsg>(0).liquidity == Liquidity::Taker);
  CHECK(t.at<OrderFillMsg>(0).fee == Notional::from_decimal("1.000575").value());

  // Both legs this profile's: two fills.
  Fixture both;
  REQUIRE(both.decode(received(kOid)).status == ParseStatus::Ok);
  REQUIRE(both.decode(received(std::string(encode_client_oid(make_cl_ord_id(1, 8)).view()), kOther))
              .status == ParseStatus::Ok);
  r = both.decode(match(kOrder, kOther, "0.1", "maker_fee_rate"));
  CHECK(r.count == 2);
}

TEST_CASE("coinbase.private_parser: done is a cancel ack or, for time in force, an expiry") {
  Fixture f;
  REQUIRE(f.decode(received(kOid)).status == ParseStatus::Ok);
  REQUIRE(f.decode(match(kOrder, kOther, "1", "maker_fee_rate")).status == ParseStatus::Ok);
  PrivateDecodeResult r = f.decode(done("canceled"));
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& c = f.at<OrderCancelAckMsg>(0);
  CHECK(c.hdr.type == EventType::OrderCancelAck);
  CHECK(c.cl_ord_id == make_cl_ord_id(1, 7));
  CHECK(c.cum_qty == Qty::from_int(1));
  CHECK(f.p->find(kOrder) == nullptr);  // no longer tracked
  CHECK(f.decode(done("canceled")).status == ParseStatus::Ignored);

  // cancel_reason 101 (time in force: an IOC or FOK remainder, a post-only that would take).
  for (const std::string& reason : {std::string(R"("101:Time In Force")"), std::string("101")}) {
    CAPTURE(reason);
    Fixture e;
    REQUIRE(e.decode(received(kOid)).status == ParseStatus::Ok);
    r = e.decode(done("canceled", reason));
    REQUIRE(r.status == ParseStatus::Ok);
    CHECK(e.at<OrderExpiredMsg>(0).hdr.type == EventType::OrderExpired);
    CHECK(e.at<OrderExpiredMsg>(0).cum_qty.is_zero());
  }
  // Filled: the matches carried it.
  Fixture fl;
  REQUIRE(fl.decode(received(kOid)).status == ParseStatus::Ok);
  CHECK(fl.decode(done("filled")).status == ParseStatus::Ignored);
  CHECK(fl.p->tracked() == 0);
}

TEST_CASE("coinbase.private_parser: change, learn, sweep, foreign orders and control frames") {
  Fixture f;
  // Learned from a REST reply: the later events name only the order id.
  REQUIRE(f.p->learn(kOrder, make_cl_ord_id(1, 7), InstrumentId{0}, Side::Buy, Qty::from_int(2)));
  CHECK_FALSE(
      f.p->learn("312", make_cl_ord_id(1, 7), InstrumentId{0}, Side::Buy, Qty::from_int(2)));
  REQUIRE(f.decode(match(kOrder, kOther, "0.5", "maker_fee_rate")).count == 1);
  // STP decreased what is left to 0.25: 0.75 in all.
  CHECK(
      f.decode(
           R"({"type":"change","reason":"STP","time":"2014-11-07T08:19:27.028459Z","sequence":80,"order_id":"d50ec984-77a8-460a-b958-66f114b0de9b","side":"buy","product_id":"BTC-USD","old_size":"1.5","new_size":"0.25","price":"400.23"})")
          .status == ParseStatus::Ignored);
  REQUIRE(f.decode(match(kOrder, kOther, "0.25", "maker_fee_rate")).count == 1);
  CHECK(f.at<OrderFillMsg>(0).leaves_qty.is_zero());
  CHECK(f.at<OrderFillMsg>(0).cum_qty == Qty::from_decimal("0.75").value());
  // Learning again keeps the filled quantity.
  REQUIRE(f.p->learn(kOrder, make_cl_ord_id(1, 7), InstrumentId{0}, Side::Buy, Qty::from_int(2)));
  CHECK(f.p->find(kOrder)->filled == Qty::from_decimal("0.75").value());
  CHECK(f.p->sweep([](ClientOrderId) { return false; }) == 1);
  CHECK(f.p->tracked() == 0);

  // Another client's order on the same profile: nothing.
  CHECK(f.decode(received("d50ec974-76a2-454b-66f1-35b1ea8c0000", kOther)).status ==
        ParseStatus::Ignored);
  CHECK(f.decode(match(kOther, kOrder, "1", "maker_fee_rate")).status == ParseStatus::Ignored);
  CHECK(f.p->stats().foreign >= 2);

  const PrivateDecodeResult err = f.decode(
      R"({"type":"error","message":"Authentication Failed","reason":"invalid signature"})");
  CHECK(err.status == ParseStatus::Error);
  CHECK(err.control == PrivateControl::Error);
  CHECK(err.reason == "invalid signature");
  CHECK(
      f.decode(
           R"({"type":"heartbeat","last_trade_id":1,"product_id":"BTC-USD","sequence":1,"time":"2014-11-07T08:19:27Z"})")
          .control == PrivateControl::Heartbeat);
  CHECK(
      f.decode(R"({"type":"subscriptions","channels":[{"name":"user","product_ids":["BTC-USD"]}]})")
          .control == PrivateControl::Subscriptions);
  CHECK(f.decode(R"({"type":"open","order_id":"x","product_id":"BTC-USD"})").status ==
        ParseStatus::Ignored);
  CHECK(f.decode("{").status == ParseStatus::Malformed);
  CHECK(f.decode(R"({"type":"match","trade_id":0,"side":"buy","size":"1","price":"1"})").status ==
        ParseStatus::Malformed);
}
