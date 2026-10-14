// Lighter decoders on frames recorded from production (tests/fixtures/lighter,
// fixtures.meta.json): order_book snapshot and updates through the book sync (a gap included),
// ticker and trade; account_all_trades of a public account, account_all_orders in the
// reference's Order shape; the refused sendtx reply; the REST answers; the error map.
#include "venue_test_util.hpp"

#include "fastmm/venues/lighter/lighter_md_feed.hpp"
#include "fastmm/venues/lighter/lighter_md_parser.hpp"
#include "fastmm/venues/lighter/lighter_private.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::lighter;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::RecordingSink;

namespace {

constexpr VenueId kLighter{9};
constexpr std::int64_t kAccount = 702384;

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  InstrumentId nvda;
  InstrumentId btc;
  MarketMap markets;
  Universe() {
    Instrument n = make_instrument("NVDA", 9, "NVDA", "USDC");
    n.asset_class = AssetClass::Perpetual;
    nvda = instruments.add(n).value();
    Instrument b = make_instrument("BTC", 9, "BTC", "USDC");
    b.asset_class = AssetClass::Perpetual;
    btc = instruments.add(b).value();
    REQUIRE(symbols.build(instruments));
    markets.add(110, nvda);
    markets.add(1, btc);
  }
};

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}

}  // namespace

TEST_CASE("lighter.md: the order_book snapshot and its updates") {
  Universe u;
  LighterMdParser p(u.markets, kLighter);
  alignas(64) std::byte scratch[kDecoderScratchBytes];

  const auto snap = padded_fixture("lighter/md_order_book_snapshot.json");
  MdDecodeResult r = p.decode(snap.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 1);
  {
    const auto& b = *reinterpret_cast<const BookDeltaMsg*>(scratch);
    CHECK(b.hdr.type == EventType::BookSnapshot);
    CHECK(b.is_snapshot());
    CHECK(b.hdr.instrument == u.nvda);
    REQUIRE(b.bid_count == 5);
    REQUIRE(b.ask_count == 5);
    CHECK(b.bids()[0].price == px("226.338"));
    CHECK(b.bids()[0].qty == qty("0.350"));
    CHECK(b.asks()[0].price == px("226.346"));
    CHECK(b.asks()[4].qty == qty("4.418"));
    CHECK(b.last_update_id == 24476893873ULL);
    CHECK(b.hdr.exch_ts.ns == 1791835627214899LL * 1000);
  }
  const auto up = padded_fixture("lighter/md_order_book_update.json");
  r = p.decode(up.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  {
    const auto& b = *reinterpret_cast<const BookDeltaMsg*>(scratch);
    CHECK(b.hdr.type == EventType::BookDelta);
    CHECK_FALSE(b.is_snapshot());
    CHECK(b.bid_count == 0);
    REQUIRE(b.ask_count == 1);
    CHECK(b.asks()[0].price == px("226.348"));
    CHECK(b.asks()[0].qty.is_zero());  // removed
    CHECK(b.prev_update_id == 24476893873ULL);
    CHECK(b.last_update_id == 24476893939ULL);
  }
}

TEST_CASE("lighter.md: the book sync applies the chain and resubscribes on a gap") {
  Universe u;
  RecordingSink sink(4U << 20);
  std::vector<InstrumentId> asked;
  auto requester = [](void* ctx, InstrumentId id) noexcept {
    static_cast<std::vector<InstrumentId>*>(ctx)->push_back(id);
  };
  LighterMdFeed feed(u.symbols, kLighter, sink.sink, InstrumentCallback{requester, &asked}, {}, 0);
  REQUIRE(feed.add_instrument(u.nvda, 110, "NVDA"));
  REQUIRE(feed.subscription_payloads().size() == 3);
  CHECK(feed.subscription_payloads()[0] == R"({"type":"subscribe","channel":"order_book/110"})");
  CHECK(feed.subscription_payloads()[2] == R"({"type":"subscribe","channel":"trade/110"})");
  feed.on_connected();
  for (const char* f : {"lighter/md_order_book_snapshot.json",
                        "lighter/md_order_book_update.json",
                        "lighter/md_order_book_update2.json"}) {
    const auto j = padded_fixture(f);
    CHECK(feed.on_message(j.view(), 1) == ParseStatus::Ok);
  }
  CHECK(feed.synced_count() == 1);
  CHECK(asked.empty());
  std::size_t books = 0;
  for (const auto& m : sink.drain()) {
    const EventType t = RecordingSink::type_of(m);
    books += t == EventType::BookSnapshot || t == EventType::BookDelta ? 1 : 0;
  }
  CHECK(books == 3);
  // A trade reaches the ring whole, its parties included.
  const auto tr = padded_fixture("lighter/md_trade.json");
  CHECK(feed.on_message(tr.view(), 1) == ParseStatus::Ok);
  const auto out = sink.drain();
  REQUIRE(out.size() == 2);
  CHECK(out[0].size() == TradeMsg::kSizeWithParties);
  const auto& t0 = RecordingSink::as<TradeMsg>(out[0]);
  REQUIRE(t0.parties() != nullptr);
  CHECK(t0.parties()->buyer.account() == 702386);
  CHECK(RecordingSink::as<TradeMsg>(out[1]).parties()->buyer.account() == 270812);
  // A fresh feed that misses the second update: the third does not chain.
  LighterMdFeed gap(u.symbols, kLighter, sink.sink, InstrumentCallback{requester, &asked}, {}, 0);
  REQUIRE(gap.add_instrument(u.nvda, 110, "NVDA"));
  gap.on_connected();
  for (const char* f : {"lighter/md_order_book_snapshot.json",
                        "lighter/md_order_book_update.json",
                        "lighter/md_order_book_update_gap.json"}) {
    const auto j = padded_fixture(f);
    static_cast<void>(gap.on_message(j.view(), 1));
  }
  CHECK(gap.synced_count() == 0);
  REQUIRE(asked.size() == 1);
  CHECK(asked[0] == u.nvda);
  const auto again = gap.resubscribe_payloads(u.nvda);
  REQUIRE(again.size() == 2);
  CHECK(again[0] == R"({"type":"unsubscribe","channel":"order_book/110"})");
  bool resyncing = false;
  for (const auto& m : sink.drain()) {
    if (RecordingSink::type_of(m) == EventType::ConnectionState)
      resyncing =
          resyncing || RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Resyncing;
  }
  CHECK(resyncing);
}

TEST_CASE("lighter.md: ticker, trades and the control frames") {
  Universe u;
  LighterMdParser p(u.markets, kLighter);
  alignas(64) std::byte scratch[kDecoderScratchBytes];

  const auto t = padded_fixture("lighter/md_ticker.json");
  MdDecodeResult r = p.decode(t.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  const auto& bbo = *reinterpret_cast<const BookTickerMsg*>(scratch);
  CHECK(bbo.hdr.instrument == u.nvda);
  CHECK(bbo.bid_px == px("226.336"));
  CHECK(bbo.bid_qty == qty("4.422"));
  CHECK(bbo.ask_px == px("226.346"));
  CHECK(bbo.ask_qty == qty("11.941"));

  const auto tr = padded_fixture("lighter/md_trade.json");
  r = p.decode(tr.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 2);
  const auto& t0 = *reinterpret_cast<const TradeMsg*>(scratch);
  CHECK(t0.hdr.instrument == u.nvda);
  CHECK(t0.price == px("226.336"));
  CHECK(t0.qty == qty("0.063"));
  CHECK(t0.trade_id == 34287672686ULL);
  CHECK(t0.aggressor == Side::Sell);  // the maker was the bid
  CHECK(t0.hdr.exch_ts.ns == 1791835628453LL * 1'000'000);
  // bid_account_id is the buyer, ask_account_id the seller.
  CHECK(t0.hdr.len == TradeMsg::kSizeWithParties);
  REQUIRE(t0.parties() != nullptr);
  CHECK(t0.parties()->buyer.kind == PartyIdKind::Account);
  CHECK(t0.parties()->buyer.account() == 702386);
  CHECK(t0.parties()->seller.account() == 751368);
  CHECK(r.len == 2 * TradeMsg::kSizeWithParties);
  const auto& t1 = *reinterpret_cast<const TradeMsg*>(scratch + TradeMsg::kSizeWithParties);
  CHECK(t1.trade_id == 34287672685ULL);
  CHECK(t1.qty == qty("0.350"));
  REQUIRE(t1.parties() != nullptr);
  CHECK(t1.parties()->buyer == PartyId::of_account(270812));
  CHECK(t1.parties()->seller == PartyId::of_account(751368));

  // A liquidation names the liquidated account like any other; a trade without the account ids
  // is a 128-byte trade with none.
  const PaddedJson liq(
      R"({"channel":"trade:110","liquidation_trades":[{"trade_id":34247094304,"type":"liquidation","market_id":110,"size":"0.182","price":"225.146","ask_account_id":281474976549565,"bid_account_id":702386,"is_maker_ask":false,"timestamp":1791814431015}],"nonce":1,"trades":[{"trade_id":7,"market_id":110,"size":"1","price":"2","is_maker_ask":true,"timestamp":1}],"type":"update/trade"})");
  r = p.decode(liq.view(), Timestamp{1}, Cycles{2}, scratch);
  REQUIRE(r.status == ParseStatus::Ok);
  REQUIRE(r.count == 2);
  CHECK(r.len == TradeMsg::kSizeWithParties + sizeof(TradeMsg));
  const auto& l0 = *reinterpret_cast<const TradeMsg*>(scratch);
  REQUIRE(l0.parties() != nullptr);
  CHECK(l0.parties()->seller.account() == 281474976549565ULL);
  CHECK(l0.parties()->buyer.account() == 702386);
  const auto& l1 = *reinterpret_cast<const TradeMsg*>(scratch + l0.hdr.len);
  CHECK(l1.trade_id == 7);
  CHECK(l1.hdr.len == sizeof(TradeMsg));
  CHECK(l1.parties() == nullptr);

  // The history sent on subscribing is not market data.
  const auto hist = padded_fixture("lighter/md_trade_snapshot.json");
  CHECK(p.decode(hist.view(), Timestamp{1}, Cycles{2}, scratch).status == ParseStatus::Ignored);

  const auto pong = padded_fixture("lighter/ws_pong.json");
  r = p.decode(pong.view(), Timestamp{1}, Cycles{2}, scratch);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.control == ControlOp::Pong);
  const auto conn = padded_fixture("lighter/ws_connected.json");
  CHECK(p.decode(conn.view(), Timestamp{1}, Cycles{2}, scratch).control == ControlOp::Connected);
  const auto err = padded_fixture("lighter/ws_error_auth.json");
  r = p.decode(err.view(), Timestamp{1}, Cycles{2}, scratch);
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.error_code == 20013);
  CHECK(r.error.find("invalid auth") != std::string_view::npos);

  // A market nobody subscribed.
  const PaddedJson other(
      R"({"channel":"ticker:7","ticker":{"s":"X","a":{"price":"1.0","size":"1"},"b":{"price":"0.9","size":"1"}},"type":"update/ticker"})");
  CHECK(p.decode(other.view(), Timestamp{1}, Cycles{2}, scratch).status ==
        ParseStatus::UnknownSymbol);
  CHECK(market_of_channel("order_book:110") == 110);
  CHECK(market_of_channel("height") == -1);
}

TEST_CASE("lighter.private: account_all_trades is the account's side of each trade") {
  LighterPrivateParser p(kAccount);
  const auto f = padded_fixture("lighter/private_account_all_trades.json");
  const PrivateFrame fr = p.decode(f.view());
  REQUIRE(fr.kind == FrameKind::Trades);
  REQUIRE(p.fills().size() == 2);
  const FillRow& a = p.fills()[0];
  CHECK(a.market == 1);
  CHECK(a.trade_id == 34287671274ULL);
  CHECK(a.side == Side::Sell);
  CHECK(a.maker);
  CHECK(a.fee_rate == 28);
  CHECK(a.order_index == 562953539268523LL);
  CHECK(a.client_order_index == 164615996158818LL);
  CHECK(a.price == px("82976.9"));
  CHECK(a.size == qty("0.00067"));
  CHECK(a.usd == Notional::from_decimal("55.594523").value());
  CHECK(a.time_ms == 1791835627185LL);
  // 28 millionths of 55.594523 USDC.
  CHECK(fee_of(a.usd, a.fee_rate) == Notional::from_decimal("0.00155664").value());
  // The other account's view of the same frame: the buy side, taker, no taker fee field.
  LighterPrivateParser other(242414);
  REQUIRE(other.decode(f.view()).kind == FrameKind::Trades);
  REQUIRE(other.fills().size() == 2);
  CHECK(other.fills()[0].side == Side::Buy);
  CHECK_FALSE(other.fills()[0].maker);
  CHECK(other.fills()[0].fee_rate == 0);
  CHECK(other.fills()[0].client_order_index == 39170101764009LL);
  // The history on subscribing is skipped.
  const auto snap = padded_fixture("lighter/private_account_all_trades_snapshot.json");
  CHECK(p.decode(snap.view()).kind == FrameKind::Ignored);
  CHECK(p.fills().empty());
  // A trade with itself gives both legs.
  const PaddedJson self(
      R"({"channel":"account_all_trades:5","trades":{"1":[{"trade_id":9,"market_id":1,"size":"1","price":"2","usd_amount":"2","ask_id":11,"bid_id":12,"ask_client_id":3,"bid_client_id":4,"ask_account_id":5,"bid_account_id":5,"is_maker_ask":true,"maker_fee":10,"taker_fee":20,"timestamp":1}]},"type":"update/account_all_trades"})");
  LighterPrivateParser five(5);
  REQUIRE(five.decode(self.view()).kind == FrameKind::Trades);
  REQUIRE(five.fills().size() == 2);
  CHECK(five.fills()[0].side == Side::Sell);
  CHECK(five.fills()[0].fee_rate == 10);
  CHECK(five.fills()[1].side == Side::Buy);
  CHECK(five.fills()[1].fee_rate == 20);
  CHECK(five.fills()[1].self_trade_other_leg);
}

TEST_CASE("lighter.private: account_all_orders rows and their statuses") {
  LighterPrivateParser p(kAccount);
  const auto f = padded_fixture("lighter/private_account_all_orders.json");
  REQUIRE(p.decode(f.view()).kind == FrameKind::Orders);
  REQUIRE(p.orders().size() == 2);
  const OrderRow& o = p.orders()[0];
  CHECK(o.market == 110);
  CHECK(o.client_order_index == 42);
  CHECK(o.order_index == 31243722509674453LL);
  CHECK(o.price == px("226.3"));
  CHECK(o.initial == qty("0.05"));
  CHECK(o.filled == qty("0.02"));
  CHECK(o.remaining == qty("0.03"));
  CHECK_FALSE(o.is_ask);
  CHECK(classify_status(o.status) == StatusClass::Open);
  CHECK(o.time_us == 1791835627257555LL);
  const OrderRow& c = p.orders()[1];
  CHECK(c.is_ask);
  CHECK(classify_status(c.status) == StatusClass::Ended);
  CHECK(ended_reason(c.status) == RejectReason::PostOnlyWouldCross);
  CHECK(classify_status("canceled") == StatusClass::Canceled);
  CHECK(classify_status("filled") == StatusClass::Filled);
  CHECK(classify_status("in-progress") == StatusClass::Ignore);
  CHECK(ended_reason("canceled-margin-not-allowed") == RejectReason::InsufficientBalance);
}

TEST_CASE("lighter.private: sendtx replies, refused and accepted") {
  LighterPrivateParser p(kAccount);
  const auto refused = padded_fixture("lighter/tx_reply_refused.json");
  PrivateFrame f = p.decode(refused.view());
  REQUIRE(f.kind == FrameKind::TxReply);
  CHECK(f.reply.id == "fm-1");
  CHECK(f.reply.refused);
  CHECK(f.reply.code == 21100);
  CHECK(f.reply.message == "account not found");
  // The REST reply's shape, at the top level and under "data".
  const PaddedJson top(
      R"({"id":"7","code":200,"message":"{\"ratelimit\": \"didn't use volume quota\"}","tx_hash":"ab","predicted_execution_time_ms":1791835627300,"volume_quota_remaining":998})");
  f = p.decode(top.view());
  REQUIRE(f.kind == FrameKind::TxReply);
  CHECK(f.reply.id == "7");
  CHECK_FALSE(f.reply.refused);
  CHECK(f.reply.quota_remaining == 998);
  const PaddedJson nested(
      R"({"type":"jsonapi/sendtx","data":{"id":"8","code":200,"tx_hash":"ab","volume_quota_remaining":12}})");
  f = p.decode(nested.view());
  REQUIRE(f.kind == FrameKind::TxReply);
  CHECK(f.reply.id == "8");
  CHECK(f.reply.quota_remaining == 12);
  const auto err = padded_fixture("lighter/ws_error_auth.json");
  f = p.decode(err.view());
  CHECK(f.kind == FrameKind::Error);
  CHECK(f.reply.code == 20013);
  const auto pong = padded_fixture("lighter/ws_pong.json");
  CHECK(p.decode(pong.view()).kind == FrameKind::Ignored);
}

TEST_CASE("lighter.rest: reference data, account, keys, nonce, trades") {
  std::vector<MarketDetail> markets;
  REQUIRE(
      decode_market_details(fastmm::test::fixture("lighter/rest_order_book_details.json"), markets)
          .empty());
  REQUIRE(markets.size() == 1);
  CHECK(markets[0].market_id == 110);
  CHECK(markets[0].symbol == "NVDA");
  CHECK(markets[0].market_type == "perp");
  CHECK(markets[0].active);
  CHECK(markets[0].size_decimals == 3);
  CHECK(markets[0].price_decimals == 3);
  CHECK(markets[0].min_base == qty("0.025"));
  CHECK(markets[0].min_quote == Notional::from_int(10));

  AccountState acct;
  REQUIRE(decode_account(fastmm::test::fixture("lighter/rest_account.json"), acct).empty());
  CHECK(acct.index == kAccount);
  CHECK(acct.collateral == Notional::from_decimal("2449291.968579").value());
  CHECK(acct.available == Notional::from_decimal("1230099.539059").value());
  REQUIRE(acct.positions.size() == 1);
  CHECK(acct.positions[0].market == 1);
  CHECK(acct.positions[0].qty == qty("17.99188"));
  CHECK(acct.positions[0].avg_entry == px("82743.5"));

  std::vector<ApiKeyRow> keys;
  REQUIRE(decode_api_keys(fastmm::test::fixture("lighter/rest_apikeys.json"), keys).empty());
  REQUIRE(keys.size() == 3);
  CHECK(keys[2].api_key_index == 2);
  CHECK(keys[2].public_key.starts_with("bd0d208326"));

  std::int64_t nonce = 0;
  REQUIRE(decode_next_nonce(fastmm::test::fixture("lighter/rest_next_nonce.json"), nonce).empty());
  CHECK(nonce == 9395367);

  std::vector<OrderRow> orders;
  const std::string err =
      decode_active_orders(fastmm::test::fixture("lighter/rest_active_orders_noauth.json"), orders);
  CHECK(err.find("code 20001") != std::string::npos);

  std::vector<FillRow> fills;
  std::string cursor;
  REQUIRE(decode_trades(
              fastmm::test::fixture("lighter/rest_trades_noauth.json"), kAccount, fills, cursor)
              .empty());
  CHECK(cursor == "eyJpbmRleCI6MzQyODg2NDI4MzF9");
  REQUIRE(fills.size() == 3);
  CHECK(fills[0].market == 113);
  CHECK(fills[0].side == Side::Sell);
  CHECK(fills[0].maker);

  TxReply r;
  REQUIRE(decode_send_tx(fastmm::test::fixture("lighter/rest_send_tx_refused.json"), r).empty());
  CHECK(r.refused);
  CHECK(r.code == 21100);
}

TEST_CASE("lighter.rest: what a refused transaction means") {
  CHECK(map_tx_error(21100, "account not found").fatal);
  CHECK(map_tx_error(20013, "invalid auth: expired token").fatal);
  CHECK(map_tx_error(0, "invalid nonce").nonce);
  CHECK(map_tx_error(429, "Too Many Requests").rate_limited);
  CHECK(map_tx_error(0, "not enough volume quota").reason == RejectReason::VenueRateLimit);
  CHECK(map_tx_error(0, "not enough margin").reason == RejectReason::InsufficientBalance);
  CHECK(map_tx_error(0, "order not found").unknown_order);
  CHECK(map_tx_error(0, "something else").reason == RejectReason::VenueReject);
}
