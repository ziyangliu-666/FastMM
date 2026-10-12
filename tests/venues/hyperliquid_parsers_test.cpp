// Hyperliquid decoders on frames recorded from production (tests/fixtures/hyperliquid,
// fixtures.meta.json): l2Book, bbo and trades; orderUpdates, userFills and userFundings of a public
// address rewritten to FastMM cloids; post replies (recorded errors, the docs' success shapes);
// the info answers; the error map.
#include "venue_test_util.hpp"

#include "fastmm/venues/hyperliquid/hl_error_map.hpp"
#include "fastmm/venues/hyperliquid/hl_md_parser.hpp"
#include "fastmm/venues/hyperliquid/hl_private_parser.hpp"
#include "fastmm/venues/hyperliquid/hl_response.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::hyperliquid;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::padded_fixture;

namespace {

constexpr VenueId kHl{7};

struct HlUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  InstrumentId nvda;
  HlUniverse() {
    Instrument n = make_instrument("xyz:NVDA", 7, "NVDA", "USDC");
    n.asset_class = AssetClass::Perpetual;
    nvda = instruments.add(n).value();
    REQUIRE(symbols.build(instruments));
  }
};

// The messages a decoder wrote back to back.
std::vector<const EventHeader*> messages(const std::byte* scratch, std::uint32_t count) {
  std::vector<const EventHeader*> out;
  std::uint32_t off = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto* h = reinterpret_cast<const EventHeader*>(scratch + off);
    out.push_back(h);
    off += h->len;
  }
  return out;
}

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}

constexpr std::uint64_t kCl1 = 0x00010000002a0005ULL;
constexpr std::uint64_t kCl2 = 0x000100000000ffffULL;

}  // namespace

TEST_CASE("hyperliquid.md: l2Book is a whole book, bbo the touch, trades the taker's side") {
  HlUniverse u;
  HlMdParser p(u.symbols, kHl);
  alignas(64) std::byte scratch[kDecoderScratchBytes];

  const auto book = padded_fixture("hyperliquid/md_l2book.json");
  MdDecodeResult r = p.decode(book.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  const auto& b = *reinterpret_cast<const BookDeltaMsg*>(scratch);
  CHECK(b.hdr.type == EventType::BookSnapshot);
  CHECK(b.is_snapshot());
  CHECK(b.hdr.instrument == u.nvda);
  CHECK(b.bid_count == 20);
  CHECK(b.ask_count == 20);
  CHECK(b.bids()[0].price == px("230.32"));
  CHECK(b.bids()[0].qty == qty("10.169"));
  CHECK(b.asks()[0].price == px("230.33"));
  CHECK(b.bids()[19].price < b.bids()[0].price);
  CHECK(b.last_update_id == 1791781700186ULL);
  CHECK(b.hdr.exch_ts.ns == 1791781700186LL * 1'000'000);

  const auto bbo = padded_fixture("hyperliquid/md_bbo.json");
  r = p.decode(bbo.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& t = *reinterpret_cast<const BookTickerMsg*>(scratch);
  CHECK(t.hdr.type == EventType::BookTicker);
  CHECK(t.bid_px == px("230.32"));
  CHECK(t.ask_px == px("230.33"));
  CHECK(t.ask_qty == qty("4.294"));

  const auto trades = padded_fixture("hyperliquid/md_trades.json");
  r = p.decode(trades.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count >= 2);
  const auto msgs = messages(scratch, r.count);
  const auto& t0 = *reinterpret_cast<const TradeMsg*>(msgs[0]);
  CHECK(t0.price == px("230.28"));
  CHECK(t0.qty == qty("8.521"));
  CHECK(t0.aggressor == Side::Sell);  // "A": the taker sold
  CHECK(t0.trade_id == 781762851773916ULL);
  CHECK(reinterpret_cast<const TradeMsg*>(msgs[1])->aggressor == Side::Buy);

  const auto ack = padded_fixture("hyperliquid/md_subscription_response.json");
  r = p.decode(ack.view(), Timestamp{1}, Cycles{2}, scratch);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.control == ControlOp::Subscribe);
  const auto pong = padded_fixture("hyperliquid/md_pong.json");
  CHECK(p.decode(pong.view(), Timestamp{1}, Cycles{2}, scratch).control == ControlOp::Pong);
  const auto err = padded_fixture("hyperliquid/ws_error.json");
  r = p.decode(err.view(), Timestamp{1}, Cycles{2}, scratch);
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.error.starts_with("Error parsing JSON"));
}

TEST_CASE("hyperliquid.md: coins without the dex prefix when the connector names the dex") {
  InstrumentTable instruments;
  Instrument n = make_instrument("NVDA", 7, "NVDA", "USDC");
  REQUIRE(instruments.add(n));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  alignas(64) std::byte scratch[kDecoderScratchBytes];
  const auto bbo = padded_fixture("hyperliquid/md_bbo.json");
  HlMdParser with_dex(symbols, kHl, "xyz");
  CHECK(with_dex.decode(bbo.view(), Timestamp{1}, Cycles{2}, scratch).status == ParseStatus::Ok);
  HlMdParser without(symbols, kHl);
  CHECK(without.decode(bbo.view(), Timestamp{1}, Cycles{2}, scratch).status ==
        ParseStatus::UnknownSymbol);
}

TEST_CASE("hyperliquid.private: orderUpdates statuses become acks, cancels, rejects, expiries") {
  HlUniverse u;
  HlPrivateParser p(u.symbols, kHl);
  alignas(64) std::byte scratch[kDecoderScratchBytes];
  const auto f = padded_fixture("hyperliquid/private_order_updates.json");
  const PrivateDecodeResult r = p.decode(f.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  // open, canceled, badAloPxRejected, (filled: nothing), iocCancelRejected, marginCanceled
  REQUIRE(r.count == 5);
  const auto msgs = messages(scratch, r.count);
  const auto& ack = *reinterpret_cast<const OrderAckMsg*>(msgs[0]);
  CHECK(ack.hdr.type == EventType::OrderAck);
  CHECK(ack.cl_ord_id == ClientOrderId{kCl1});
  CHECK(ack.venue_order_id.view() == "571712116790");
  CHECK(ack.hdr.exch_ts.ns == 1791781700524LL * 1'000'000);
  const auto& cancel = *reinterpret_cast<const OrderCancelAckMsg*>(msgs[1]);
  CHECK(cancel.hdr.type == EventType::OrderCancelAck);
  CHECK(cancel.cum_qty.is_zero());
  const auto& rej = *reinterpret_cast<const OrderRejectMsg*>(msgs[2]);
  CHECK(rej.hdr.type == EventType::OrderReject);
  CHECK(rej.cl_ord_id == ClientOrderId{kCl2});
  CHECK(rej.reason == RejectReason::PostOnlyWouldCross);
  CHECK(rej.text.view() == "badAloPxRejected");
  const auto& ioc = *reinterpret_cast<const OrderRejectMsg*>(msgs[3]);
  CHECK(ioc.hdr.type == EventType::OrderReject);
  CHECK_FALSE(ioc.cl_ord_id.valid());  // no cloid: not one of ours
  const auto& exp = *reinterpret_cast<const OrderExpiredMsg*>(msgs[4]);
  CHECK(exp.hdr.type == EventType::OrderExpired);
  CHECK(exp.cum_qty == qty("0.02"));
  CHECK(exp.venue_order_id.view() == "571712108399");
  CHECK(p.stats().order_updates == 6);
}

TEST_CASE("hyperliquid.private: userFills, the snapshot skipped, and userFundings") {
  HlUniverse u;
  HlPrivateParser p(u.symbols, kHl);
  alignas(64) std::byte scratch[kDecoderScratchBytes];
  const auto snap = padded_fixture("hyperliquid/private_user_fills_snapshot.json");
  PrivateDecodeResult r = p.decode(snap.view(), Timestamp{1}, Cycles{2}, scratch);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.snapshot);
  CHECK(p.stats().snapshots_skipped == 1);

  const auto live = padded_fixture("hyperliquid/private_user_fills.json");
  r = p.decode(live.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 2);
  const auto msgs = messages(scratch, r.count);
  const auto& f0 = *reinterpret_cast<const OrderFillMsg*>(msgs[0]);
  CHECK(f0.cl_ord_id == ClientOrderId{kCl1});
  CHECK(f0.side == Side::Buy);
  CHECK(f0.price == px("230.31"));
  CHECK(f0.qty == qty("0.05"));
  CHECK(f0.liquidity == Liquidity::Maker);
  CHECK(f0.fee == Notional::from_decimal("0.000345").value());
  CHECK(f0.fee_asset == FeeAsset::Quote);
  CHECK(f0.cum_qty.is_zero());  // the connector completes it from its shadow
  const auto& f1 = *reinterpret_cast<const OrderFillMsg*>(msgs[1]);
  CHECK_FALSE(f1.cl_ord_id.valid());
  CHECK(f1.side == Side::Sell);
  CHECK(f1.liquidity == Liquidity::Taker);
  CHECK(f1.fee == Notional::from_decimal("-0.001").value());
  CHECK(f1.exec_id.view() != f0.exec_id.view());

  const auto fsnap = padded_fixture("hyperliquid/private_user_fundings_snapshot.json");
  CHECK(p.decode(fsnap.view(), Timestamp{1}, Cycles{2}, scratch).snapshot);
  const auto fund = padded_fixture("hyperliquid/private_user_fundings.json");
  r = p.decode(fund.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  const auto& m = *reinterpret_cast<const FundingMsg*>(scratch);
  CHECK(m.hdr.type == EventType::Funding);
  CHECK(m.amount == Notional::from_decimal("-0.012345").value());
  CHECK(m.asset.view() == "USDC");
  CHECK(m.funding_id.view() == "1791781200001:xyz:NVDA");
}

TEST_CASE("hyperliquid.response: post replies per order, whole-action errors, info") {
  HlResponseDecoder d;
  PostReply r;
  const auto orders = padded_fixture("hyperliquid/post_order_statuses.json");
  REQUIRE(d.decode_frame(orders.view(), r) == ParseStatus::Ok);
  CHECK(r.kind == PostReply::Kind::Action);
  CHECK(r.id == 11);
  CHECK(r.ok);
  CHECK(r.action_type == "order");
  REQUIRE(r.count == 3);
  CHECK(r.statuses[0].kind == OrderStatus::Kind::Resting);
  CHECK(r.statuses[0].oid == 77738308);
  CHECK(r.statuses[0].cloid == "0x666d00000000000000010000002a0005");
  CHECK(r.statuses[1].kind == OrderStatus::Kind::Filled);
  CHECK(r.statuses[1].total_sz == qty("0.02"));
  CHECK(r.statuses[1].avg_px == px("1891.4"));
  CHECK(r.statuses[2].kind == OrderStatus::Kind::Error);
  CHECK(map_error_text(r.statuses[2].error).reason == RejectReason::PostOnlyWouldCross);

  const auto cancels = padded_fixture("hyperliquid/post_cancel_statuses.json");
  REQUIRE(d.decode_frame(cancels.view(), r) == ParseStatus::Ok);
  REQUIRE(r.count == 2);
  CHECK(r.statuses[0].kind == OrderStatus::Kind::Success);
  CHECK(r.statuses[1].kind == OrderStatus::Kind::Error);
  CHECK(map_error_text(r.statuses[1].error).reason == RejectReason::VenueUnknownOrder);

  const auto wallet = padded_fixture("hyperliquid/post_err_wallet.json");
  REQUIRE(d.decode_frame(wallet.view(), r) == ParseStatus::Ok);
  CHECK(r.id == 100);
  CHECK_FALSE(r.ok);
  CHECK(r.error.starts_with("User or API Wallet"));
  CHECK(map_error_text(r.error).action == VenueAction::Fatal);

  const auto sched = padded_fixture("hyperliquid/post_schedule_cancel.json");
  REQUIRE(d.decode_frame(sched.view(), r) == ParseStatus::Ok);
  CHECK(r.ok);
  CHECK(r.action_type == "default");
  CHECK(r.count == 0);

  const auto info = padded_fixture("hyperliquid/post_info_rate_limit.json");
  REQUIRE(d.decode_frame(info.view(), r) == ParseStatus::Ok);
  CHECK(r.kind == PostReply::Kind::Info);
  CHECK(r.id == 200);
  CHECK(r.info_type == "userRateLimit");
  RateLimitState rl;
  CHECK(decode_rate_limit(r.info_data, rl).empty());
  CHECK(rl.cap == 10000);

  const auto err = padded_fixture("hyperliquid/ws_error.json");
  REQUIRE(d.decode_frame(err.view(), r) == ParseStatus::Ok);
  CHECK(r.kind == PostReply::Kind::Error);
  CHECK(r.id == 0);

  const auto pong = padded_fixture("hyperliquid/md_pong.json");
  CHECK(d.decode_frame(pong.view(), r) == ParseStatus::Ignored);

  const auto rest = padded_fixture("hyperliquid/exchange_order_resting.json");
  REQUIRE(d.decode_exchange(rest.view(), r) == ParseStatus::Ok);
  CHECK(r.ok);
  REQUIRE(r.count == 1);
  CHECK(r.statuses[0].oid == 77738308);
}

TEST_CASE("hyperliquid.info: meta, positions, balances, open orders, fills, budget, status") {
  std::vector<std::string> dexes;
  CHECK(decode_perp_dexs(fastmm::test::fixture("hyperliquid/rest_perp_dexs.json"), dexes).empty());
  REQUIRE(dexes.size() >= 2);
  CHECK(dexes[0].empty());
  CHECK(dexes[1] == "xyz");

  std::vector<AssetMeta> meta;
  CHECK(decode_meta(fastmm::test::fixture("hyperliquid/rest_meta_xyz.json"), meta).empty());
  REQUIRE(meta.size() == 6);
  CHECK(meta[0].name == "xyz:XYZ100");
  CHECK(meta[0].sz_decimals == 4);
  CHECK(meta[2].name == "xyz:NVDA");
  CHECK(meta[2].sz_decimals == 3);
  std::vector<AssetMeta> with_ctx;
  CHECK(decode_meta(fastmm::test::fixture("hyperliquid/rest_meta_and_ctxs_xyz.json"), with_ctx)
            .empty());
  REQUIRE(with_ctx.size() == 6);
  CHECK(with_ctx[2].mark.is_positive());

  ClearinghouseState ch;
  CHECK(decode_clearinghouse(fastmm::test::fixture("hyperliquid/rest_clearinghouse_xyz.json"), ch)
            .empty());
  REQUIRE(ch.positions.size() == 3);
  CHECK(ch.positions[0].coin == "xyz:XYZ100");
  CHECK(ch.positions[0].szi == qty("-420.8346"));
  CHECK(ch.account_value.is_positive());

  std::vector<SpotBalanceRow> spot;
  CHECK(
      decode_spot_balances(fastmm::test::fixture("hyperliquid/rest_spot_clearinghouse.json"), spot)
          .empty());
  REQUIRE(!spot.empty());
  CHECK(spot[0].coin == "USDC");
  CHECK(spot[0].total.is_positive());

  std::vector<OpenOrderRow> oo;
  CHECK(decode_open_orders(fastmm::test::fixture("hyperliquid/rest_open_orders_xyz.json"), oo)
            .empty());
  REQUIRE(oo.size() == 3);
  CHECK(oo[0].coin == "xyz:GOLD");
  CHECK_FALSE(oo[0].buy);
  CHECK(oo[0].oid == 571712089455ULL);
  CHECK(oo[0].cloid == "0xb2d4936236d646e63bd16a334c6fc6eb");

  std::vector<FillRow> fills;
  CHECK(decode_user_fills(fastmm::test::fixture("hyperliquid/rest_user_fills_by_time.json"), fills)
            .empty());
  REQUIRE(fills.size() == 3);
  CHECK(fills[0].coin == "xyz:SKHX");
  CHECK_FALSE(fills[0].crossed);
  CHECK(fills[0].fee.raw < 0);  // a maker rebate
  CHECK(fills[0].tid != 0);

  RateLimitState rl;
  CHECK(decode_rate_limit(fastmm::test::fixture("hyperliquid/rest_user_rate_limit.json"), rl)
            .empty());
  CHECK(rl.cap > rl.used);
  CHECK(rl.cum_volume > 0);

  OrderStatusRow st;
  CHECK(
      decode_order_status(fastmm::test::fixture("hyperliquid/rest_order_status.json"), st).empty());
  CHECK(st.status == "canceled");
  CHECK(st.status_ms == 1791781699383);
  CHECK(st.placed_ms == 1791781698849);
  OrderStatusRow unknown;
  CHECK(decode_order_status(fastmm::test::fixture("hyperliquid/rest_order_status_unknown.json"),
                            unknown)
            .empty());
  CHECK(unknown.status == "unknownOid");

  CHECK_FALSE(decode_meta("{}", meta).empty());
  CHECK_FALSE(decode_clearinghouse("not json", ch).empty());
}

TEST_CASE("hyperliquid.errors: texts and statuses map to reasons and actions") {
  CHECK(map_error_text("Order must have minimum value of $10.").reason ==
        RejectReason::BelowMinNotional);
  CHECK(map_error_text("Insufficient margin to place order. asset=110002").reason ==
        RejectReason::InsufficientBalance);
  CHECK(map_error_text("Too many cumulative requests sent").action == VenueAction::RateLimit);
  CHECK(map_error_text("Order does not exist.").action != VenueAction::Fatal);
  CHECK(map_error_text("Order does not exist.").reason == RejectReason::VenueUnknownOrder);
  CHECK(map_error_text("Invalid nonce: duplicate").action == VenueAction::Backoff);
  CHECK(map_error_text("Price must be divisible by tick size. asset=110002").reason ==
        RejectReason::InvalidTick);
  CHECK_FALSE(map_error_text("something new").known);
  CHECK(classify_status("open") == StatusClass::Open);
  CHECK(classify_status("canceled") == StatusClass::Canceled);
  CHECK(classify_status("filled") == StatusClass::Ignore);
  CHECK(classify_status("marginCanceled") == StatusClass::Expired);
  CHECK(classify_status("scheduledCancel") == StatusClass::Expired);
  CHECK(classify_status("badAloPxRejected") == StatusClass::Rejected);
  CHECK(classify_status("perpMarginRejected") == StatusClass::Rejected);
  CHECK(rejected_reason("minTradeNtlRejected") == RejectReason::BelowMinNotional);
  CHECK(rejected_reason("iocCancelRejected") == RejectReason::VenueReject);
}
