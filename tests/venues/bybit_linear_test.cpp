// Bybit v5 linear perpetuals: the category-dependent parts of the encoder, the REST decoders and
// the private parser. Wire formats from the create-order, open-order, position, execution and
// websocket/private/{position,execution} pages (read 2026-09-26).
#include "venue_test_util.hpp"

#include "fastmm/venues/bybit/bybit_order_encoder.hpp"
#include "fastmm/venues/bybit/bybit_private_parser.hpp"
#include "fastmm/venues/bybit/bybit_rest_decoder.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <string>
#include <string_view>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::bybit;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {

constexpr std::int64_t kTs = 1789299700000;
constexpr VenueId kBybit{1};
const InstrumentId kBtc{2};

Signer test_signer() {
  Credentials c;
  c.api_key = "test-key";
  c.secret.value = "test-secret";
  return Signer(c);
}

OutNewOrderMsg new_order(OrderType type, TimeInForce tif) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, kBtc, kBybit);
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Sell;
  n.type = type;
  n.tif = tif;
  n.price = Price::from_decimal("60000.1").value();
  n.qty = Qty::from_decimal("0.015").value();
  return n;
}

Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}

// The i-th message the parser wrote into `s`.
template <class M>
const M& nth(const Scratch& s, std::uint32_t i) {
  std::uint32_t off = 0;
  for (std::uint32_t k = 0; k < i; ++k)
    off += reinterpret_cast<const EventHeader*>(s.buf + off)->len;
  return *reinterpret_cast<const M*>(s.buf + off);
}

}  // namespace

TEST_CASE("bybit_linear.category: parsed from the config value, spot by default") {
  CHECK(parse_category("") == BybitCategory::Spot);
  CHECK(parse_category("spot") == BybitCategory::Spot);
  CHECK(parse_category("linear") == BybitCategory::Linear);
  CHECK_FALSE(parse_category("inverse").has_value());
  CHECK(dcp_product(BybitCategory::Linear) == "DERIVATIVES");
  CHECK(dcp_topic(BybitCategory::Linear) == "dcp.future");
  CHECK(dcp_product(BybitCategory::Spot) == "SPOT");
  CHECK(dcp_topic(BybitCategory::Spot) == "dcp.spot");
}

TEST_CASE("bybit_linear.encoder: order.create carries category, positionIdx and reduceOnly") {
  TestUniverse u;
  const Signer s = test_signer();
  BybitOrderEncoder enc(s, u.symbols, 5000, BybitCategory::Linear);
  char buf[kMaxRequestBytes];

  OutNewOrderMsg n = new_order(OrderType::PostOnly, TimeInForce::Gtc);
  std::size_t len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  REQUIRE(len > 0);
  CHECK(
      std::string_view(buf, len) ==
      R"({"reqId":"nfm000100000001","header":{"X-BAPI-TIMESTAMP":"1789299700000","X-BAPI-RECV-WINDOW":"5000"},"op":"order.create","args":[{"category":"linear","symbol":"BTCUSDT","side":"Sell","orderType":"Limit","qty":"0.015","price":"60000.1","timeInForce":"PostOnly","orderLinkId":"fm000100000001","positionIdx":0}]})");

  n.reduce_only = 1;
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  CHECK(std::string_view(buf, len).find(R"("positionIdx":0,"reduceOnly":true})") !=
        std::string_view::npos);

  // A perp market order is always by qty: no marketUnit.
  n = new_order(OrderType::Market, TimeInForce::Gtc);
  len = enc.encode_ws(*OrderCommand::from(n.hdr), nullptr, kTs, buf);
  const std::string_view market(buf, len);
  CHECK(market.find(R"("orderType":"Market","qty":"0.015","orderLinkId")") !=
        std::string_view::npos);
  CHECK(market.find("marketUnit") == std::string_view::npos);

  // Amend and cancel take the category too, and nothing position-related.
  OutReplaceMsg rp{};
  init_header(rp, EventType::OutReplace, kBtc, kBybit);
  rp.cl_ord_id = decode_cl_ord_id("fm000100000002").value();
  rp.orig_cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  rp.price = Price::from_decimal("60000.2").value();
  rp.qty = Qty::from_decimal("0.02").value();
  OrderShadow sh{kBtc, Side::Sell, OrderType::PostOnly, TimeInForce::Gtc, rp.orig_cl_ord_id, {}};
  len = enc.encode_ws(*OrderCommand::from(rp.hdr), &sh, kTs, buf);
  CHECK(
      std::string_view(buf, len).find(
          R"("op":"order.amend","args":[{"category":"linear","symbol":"BTCUSDT","orderLinkId":"fm000100000001","qty":"0.02","price":"60000.2"}])") !=
      std::string_view::npos);
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, kBtc, kBybit);
  c.cl_ord_id = rp.cl_ord_id;
  c.venue_order_id.assign("9aac161b-8ed6-450d-9cab-c5cc67c21784");
  len = enc.encode_ws(*OrderCommand::from(c.hdr), &sh, kTs, buf);
  CHECK(
      std::string_view(buf, len).find(
          R"("args":[{"category":"linear","symbol":"BTCUSDT","orderId":"9aac161b-8ed6-450d-9cab-c5cc67c21784"}])") !=
      std::string_view::npos);
}

TEST_CASE("bybit_linear.encoder: REST reconciliation, positions, cancel-all, executions, dcp") {
  TestUniverse u;
  const Signer s = test_signer();
  BybitOrderEncoder enc(s, u.symbols, 5000, BybitCategory::Linear);
  RestRequest rr;
  // Linear needs one of symbol, baseCoin, settleCoin.
  CHECK_FALSE(enc.encode_rest_open_orders({}, {}, rr));
  REQUIRE(enc.encode_rest_open_orders({}, "USDT", {}, rr));
  CHECK(rr.target() == "/v5/order/realtime?category=linear&settleCoin=USDT&limit=50");
  REQUIRE(enc.encode_rest_open_orders({}, "USDT", "c%3D2", rr));
  CHECK(rr.target() == "/v5/order/realtime?category=linear&settleCoin=USDT&limit=50&cursor=c%3D2");
  REQUIRE(enc.encode_rest_open_orders("BTCUSDT", "USDT", {}, rr));
  CHECK(rr.target() == "/v5/order/realtime?category=linear&symbol=BTCUSDT&limit=50");

  REQUIRE(BybitOrderEncoder::encode_rest_positions(BybitCategory::Linear, "BTCUSDT", {}, {}, rr));
  CHECK(rr.method == "GET");
  CHECK(rr.target() == "/v5/position/list?category=linear&symbol=BTCUSDT&limit=200");
  REQUIRE(BybitOrderEncoder::encode_rest_positions(BybitCategory::Linear, {}, "USDC", "x", rr));
  CHECK(rr.target() == "/v5/position/list?category=linear&settleCoin=USDC&limit=200&cursor=x");
  CHECK_FALSE(BybitOrderEncoder::encode_rest_positions(BybitCategory::Linear, {}, {}, {}, rr));
  CHECK_FALSE(BybitOrderEncoder::encode_rest_positions(BybitCategory::Spot, "BTCUSDT", {}, {}, rr));

  REQUIRE(enc.encode_rest_cancel_all("BTCUSDT", rr));
  CHECK(rr.body == R"({"category":"linear","symbol":"BTCUSDT"})");
  REQUIRE(enc.encode_rest_executions(kTs, 0, 100, {}, rr));
  CHECK(rr.target() == "/v5/execution/list?category=linear&startTime=1789299700000&limit=100");
  REQUIRE(enc.encode_rest_set_dcp(dcp_product(BybitCategory::Linear), 10, rr));
  CHECK(rr.body == R"({"product":"DERIVATIVES","timeWindow":10})");

  // The spot encoder is unchanged: no settleCoin, no position list.
  BybitOrderEncoder spot(s, u.symbols, 5000);
  REQUIRE(spot.encode_rest_open_orders({}, "USDT", {}, rr));
  CHECK(rr.target() == "/v5/order/realtime?category=spot&settleCoin=USDT&limit=50");
  REQUIRE(spot.encode_rest_open_orders({}, {}, rr));
  CHECK(rr.target() == "/v5/order/realtime?category=spot&limit=50");
}

TEST_CASE("bybit_linear.decoder: instruments-info for a linear perpetual") {
  std::vector<InstrumentInfo> infos;
  REQUIRE(decode_instruments(fastmm::test::fixture("bybit/linear_instruments_info.json"), infos)
              .empty());
  REQUIRE(infos.size() == 1);
  const InstrumentInfo& i = infos[0];
  CHECK(i.symbol == "BTCUSDT");
  CHECK(i.contract_type == "LinearPerpetual");
  CHECK(i.settle_coin == "USDT");
  CHECK(i.base_coin == "BTC");
  CHECK(i.tick == Price::from_decimal("0.1").value());
  CHECK(i.base_precision == qty("0.001"));  // qtyStep
  CHECK(i.min_qty == qty("0.001"));
  CHECK(i.max_qty == qty("1190"));
  CHECK(i.min_amount == Notional::from_int(5));  // minNotionalValue
}

TEST_CASE("bybit_linear.decoder: position/list rows are signed by side") {
  std::vector<PositionRecord> rows;
  std::string cursor = "stale";
  REQUIRE(decode_positions(fastmm::test::fixture("bybit/linear_position_list.json"), rows, cursor)
              .empty());
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].symbol == "BTCUSDT");
  CHECK(rows[0].position_idx == 0);
  CHECK(rows[0].qty == qty("-0.015"));
  CHECK(rows[0].avg_px == Price::from_decimal("60123.45").value());
  CHECK(cursor.empty());
  rows.clear();
  REQUIRE(
      decode_positions(fastmm::test::fixture("bybit/linear_position_list_hedge.json"), rows, cursor)
          .empty());
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].position_idx == 1);
  CHECK(rows[1].position_idx == 2);
  CHECK(rows[0].qty.is_zero());
  CHECK_FALSE(
      decode_positions(fastmm::test::fixture("bybit/rest_error.json"), rows, cursor).empty());
}

TEST_CASE("bybit_linear.private_parser: execution fills with fees in the settle coin") {
  TestUniverse u;
  BybitPrivateParser p(u.symbols, u.instruments, kBybit, 1U << 20, BybitCategory::Linear);
  Scratch s;
  const auto j = padded_fixture("bybit/linear_private_execution.json");
  const MdDecodeResult r = p.decode(j.view(), Timestamp{11}, Cycles{12}, s.span());
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);  // the spot row is another category's
  const auto& taker = nth<OrderFillMsg>(s, 0);
  CHECK(taker.hdr.type == EventType::OrderFill);
  CHECK(taker.hdr.instrument == kBtc);
  CHECK(taker.cl_ord_id == decode_cl_ord_id("fm000100000001").value());
  CHECK(taker.venue_order_id.view() == "9aac161b-8ed6-450d-9cab-c5cc67c21784");
  CHECK(taker.exec_id.view() == "0ab1bdf7-4219-438b-b30a-32ec863018f7");
  CHECK(taker.side == Side::Sell);
  CHECK(taker.qty == qty("0.5"));
  CHECK(taker.cum_qty == qty("0.5"));
  CHECK(taker.fee == Notional::from_decimal("26.3725275").value());
  CHECK(taker.fee_asset == FeeAsset::Quote);  // feeCurrency "": the settle coin
  CHECK(taker.liquidity == Liquidity::Taker);
  const auto& maker = nth<OrderFillMsg>(s, 1);
  CHECK(maker.side == Side::Buy);
  CHECK(maker.fee == Notional::from_decimal("-0.0012").value());  // a rebate
  CHECK(maker.fee_asset == FeeAsset::Quote);  // the spot rule would say Base for a buy
  CHECK(maker.cum_qty == qty("0.01"));
  CHECK(maker.leaves_qty == qty("0.01"));
  CHECK(maker.liquidity == Liquidity::Maker);

  // The spot parser takes only the spot row.
  BybitPrivateParser spot(u.symbols, u.instruments, kBybit);
  const MdDecodeResult rs = spot.decode(j.view(), Timestamp{11}, Cycles{12}, s.span());
  REQUIRE(rs.ok());
  CHECK(rs.count == 1);
  CHECK(s.as<OrderFillMsg>().exec_id.view() == "2100000000000000001");
}

TEST_CASE("bybit_linear.private_parser: the position topic becomes a signed PositionUpdate") {
  TestUniverse u;
  BybitPrivateParser p(u.symbols, u.instruments, kBybit, 1U << 20, BybitCategory::Linear);
  Scratch s;
  auto j = padded_fixture("bybit/linear_private_position.json");
  MdDecodeResult r = p.decode(j.view(), Timestamp{11}, Cycles{12}, s.span());
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);  // the inverse row is skipped
  CHECK(r.order_kind == OrderEventKind::Position);
  const auto& pos = s.as<PositionUpdateMsg>();
  CHECK(pos.hdr.type == EventType::PositionUpdate);
  CHECK(pos.hdr.instrument == kBtc);
  CHECK(pos.qty == qty("0.02"));
  CHECK(pos.avg_px == Price::from_decimal("60000.5").value());
  CHECK(pos.hdr.exch_ts.ns == 1697682317038LL * 1'000'000);
  CHECK(p.stats().hedge_positions == 0);

  // A short is negative; a hedge-mode row (positionIdx 2) is counted, not decoded.
  const std::string short_frame =
      R"({"id":"x","topic":"position","creationTime":1697682317044,"data":[{"positionIdx":0,"symbol":"BTCUSDT","side":"Sell","size":"0.3","entryPrice":"61000","category":"linear","updatedTime":"1697682317038"}]})";
  const PaddedJson pj(short_frame);
  r = p.decode(pj.view(), Timestamp{11}, Cycles{12}, s.span());
  REQUIRE(r.ok());
  CHECK(s.as<PositionUpdateMsg>().qty == qty("-0.3"));
  const std::string hedge_frame =
      R"({"id":"x","topic":"position","creationTime":1697682317044,"data":[{"positionIdx":2,"symbol":"BTCUSDT","side":"Sell","size":"0.3","entryPrice":"61000","category":"linear","updatedTime":"1697682317038"}]})";
  const PaddedJson ph(hedge_frame);
  r = p.decode(ph.view(), Timestamp{11}, Cycles{12}, s.span());
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(p.stats().hedge_positions == 1);

  // The spot parser does not decode positions of another category.
  BybitPrivateParser spot(u.symbols, u.instruments, kBybit);
  j = padded_fixture("bybit/linear_private_position.json");
  CHECK(spot.decode(j.view(), Timestamp{11}, Cycles{12}, s.span()).status == ParseStatus::Ignored);
}

// A funding execution as Bybit writes it (enum page: execType "Funding", orderType "UNKNOWN";
// execFee is the funding fee, positive when paid, the opposite of the transaction log's `funding`
// field, which "Positive fee value means receive funding" and "This is opposite to the execFee from
// Get Trade History"). execQty is the position, execPrice the mark price.
TEST_CASE("bybit_linear.private_parser: a Funding execution becomes a funding payment") {
  TestUniverse u;
  BybitPrivateParser p(u.symbols, u.instruments, kBybit, 1U << 20, BybitCategory::Linear);
  Scratch s;
  const std::string frame =
      R"({"id":"e9","topic":"execution","creationTime":1789315200010,"data":[)"
      R"({"category":"linear","symbol":"BTCUSDT","closedSize":"","execFee":"0.5","execId":"fund-7c1a","execPrice":"60010.2","execQty":"0.2","execType":"Funding","execValue":"12002.04","feeRate":"0.0000416","tradeIv":"","markIv":"","blockTradeId":"","markPrice":"60010.2","indexPrice":"","underlyingPrice":"","leavesQty":"0","orderId":"1b3ffe0e-6e7a-4e8f-9fb0-2d2e31b0a7b2","orderLinkId":"","orderPrice":"0","orderQty":"0","orderType":"UNKNOWN","stopOrderType":"UNKNOWN","side":"Buy","execTime":"1789315200000","isLeverage":"0","isMaker":false,"seq":140612148849391,"feeCurrency":""},)"
      R"({"category":"linear","symbol":"BTCUSDT","execFee":"-0.3","execId":"fund-9d2b","execPrice":"60010.2","execQty":"0.2","execType":"Funding","orderId":"x","orderLinkId":"","side":"Sell","execTime":"1789315200001","isMaker":false,"feeCurrency":"USDT"},)"
      R"({"category":"linear","symbol":"ETHUSDT","execFee":"0.1","execId":"fund-eth","execType":"Funding","orderId":"y","orderLinkId":"","side":"Buy","execTime":"1789315200002","feeCurrency":""},)"
      R"({"category":"linear","symbol":"BTCUSDT","execFee":"0.1","execId":"adl-1","execPrice":"60000","execQty":"0.1","execType":"AdlTrade","orderId":"z","orderLinkId":"","side":"Buy","orderQty":"0.1","leavesQty":"0","execTime":"1789315200003"}]})";
  const PaddedJson j(frame);
  const MdDecodeResult r = p.decode(j.view(), Timestamp{11}, Cycles{12}, s.span());
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);  // ETHUSDT is not traded on this venue; the ADL trade is not funding
  const auto& paid = nth<FundingMsg>(s, 0);
  CHECK(paid.hdr.type == EventType::Funding);
  CHECK(paid.hdr.instrument == kBtc);
  CHECK(paid.hdr.venue == kBybit);
  CHECK(paid.amount == Notional::from_decimal("-0.5").value());
  CHECK(paid.asset.view() == "USDT");  // feeCurrency "": the settle coin
  CHECK(paid.funding_id.view() == "fund-7c1a");
  CHECK(paid.hdr.exch_ts.ns == 1789315200000LL * 1'000'000);
  CHECK((paid.flags & FundingMsg::kReplayed) == 0);
  const auto& received = nth<FundingMsg>(s, 1);
  CHECK(received.amount == Notional::from_decimal("0.3").value());
  CHECK(received.funding_id.view() == "fund-9d2b");
  CHECK(p.stats().funding == 2);

  // The spot parser has no funding: a linear row is another category's.
  BybitPrivateParser spot(u.symbols, u.instruments, kBybit);
  CHECK(spot.decode(j.view(), Timestamp{11}, Cycles{12}, s.span()).status == ParseStatus::Ignored);
}

TEST_CASE("bybit_linear.decoder: an execution/list Funding row needs no order fields") {
  BybitResponseDecoder d;
  const std::string body =
      R"({"retCode":0,"retMsg":"OK","result":{"nextPageCursor":"","category":"linear","list":[)"
      R"({"symbol":"BTCUSDT","execFee":"0.5","execId":"fund-7c1a","execType":"Funding","execTime":"1789315200000","feeCurrency":"USDT"}]},"retExtInfo":{},"time":1789315201000})";
  const PaddedJson j(body);
  std::string cursor;
  std::vector<ExecutionRecord> rows;
  std::vector<std::string> ids;
  REQUIRE(d.decode_executions(j.view(), cursor, [&](const ExecutionRecord& e) {
    rows.push_back(e);
    ids.emplace_back(e.exec_id);
  }) == ParseStatus::Ok);
  REQUIRE(rows.size() == 1);
  CHECK(ids[0] == "fund-7c1a");
  CHECK(rows[0].exec_type == "Funding");
  CHECK(rows[0].exec_fee == "0.5");
  CHECK(rows[0].exec_time_ms == 1789315200000);
  // A Trade row still needs its order fields.
  std::string trade = body;
  trade.replace(trade.find("\"Funding\""), 9, "\"Trade\"");
  const PaddedJson tj(trade);
  CHECK(d.decode_executions(tj.view(), cursor, [](const ExecutionRecord&) {}) != ParseStatus::Ok);
}
