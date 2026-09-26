// AccountBook: fastmm-gateway's positions of an account on one venue, and its exposure check.
#include "fastmm/core/account_book.hpp"

#include "test_support.hpp"

#include <string>
#include <vector>

using namespace fastmm;

namespace {

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}
Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

constexpr InstrumentId kBtc{0};  // venue 0
constexpr InstrumentId kEth{1};  // venue 0
constexpr InstrumentId kSol{2};  // venue 1

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  REQUIRE(t.add(i));
  i.symbol = "ETHUSDT";
  REQUIRE(t.add(i));
  i.symbol = "SOLUSDT";
  i.venue = VenueId{1};
  REQUIRE(t.add(i));
  return t;
}

OrderFillMsg fill(
    InstrumentId id, Side side, const char* price, const char* qty, const char* exec) {
  OrderFillMsg m{};
  init_header(m, EventType::OrderFill, id, VenueId{0});
  m.side = side;
  m.price = px(price);
  m.qty = qt(qty);
  m.fee = nt("0.1");
  m.fee_asset = FeeAsset::Quote;
  m.exec_id.assign(exec);
  return m;
}

void snapshot(AccountBook& b, InstrumentId id, const char* bid, const char* ask) {
  alignas(64) std::byte bytes[BookDeltaMsg::size_for(1, 1)]{};
  auto* d = reinterpret_cast<BookDeltaMsg*>(bytes);
  init_header(*d, EventType::BookSnapshot, id, VenueId{0}, BookDeltaMsg::size_for(1, 1));
  d->hdr.flags |= EventHeader::kSnapshot;
  d->bid_count = d->ask_count = 1;
  d->last_update_id = 1;
  d->levels()[0] = Level{px(bid), qt("5")};
  d->levels()[1] = Level{px(ask), qt("5")};
  CHECK(b.on_book(*d));
}

// first_time() then book(), as the gateway does.
void take(AccountBook& b, const OrderFillMsg& m) {
  if (b.first_time(m)) b.book(m);
}

}  // namespace

TEST_CASE("core.account_book: every execution is booked once, keyed by id, instrument and side") {
  const InstrumentTable t = make_table();
  AccountBook b(t, VenueId{0});
  take(b, fill(kBtc, Side::Buy, "100", "1", "t1"));
  take(b, fill(kBtc, Side::Buy, "100", "1", "t1"));  // the stream and a replay
  CHECK(b.positions()[kBtc].qty == qt("1"));
  CHECK(b.positions()[kBtc].fees == nt("0.1"));
  // The same venue id on the other side (a self-trade's other half) or another instrument is
  // another execution.
  take(b, fill(kBtc, Side::Sell, "110", "1", "t1"));
  take(b, fill(kEth, Side::Sell, "50", "2", "t1"));
  CHECK(b.positions()[kBtc].qty.is_zero());
  CHECK(b.positions()[kBtc].realized == nt("10"));
  CHECK(b.positions()[kEth].qty == qt("-2"));
  CHECK(b.positions().total_fees() == nt("0.3"));
}

TEST_CASE(
    "core.account_book: fees in the base asset change the position, as the engine books them") {
  const InstrumentTable t = make_table();
  AccountBook b(t, VenueId{0});
  OrderFillMsg m = fill(kBtc, Side::Buy, "100", "1", "t1");
  m.fee = Notional::from_raw(qt("0.001").raw);  // 0.001 BTC
  m.fee_asset = FeeAsset::Base;
  take(b, m);
  CHECK(b.positions()[kBtc].qty == qt("0.999"));
  CHECK(b.positions()[kBtc].fees == nt("0.1"));
  OrderFillMsg other = fill(kEth, Side::Buy, "100", "1", "t2");
  other.fee_asset = FeeAsset::Other;  // BNB: not valued
  take(b, other);
  CHECK(b.positions()[kEth].fees.is_zero());
}

TEST_CASE("core.account_book: marks at the mid of a valid book, and seeds a position") {
  const InstrumentTable t = make_table();
  AccountBook b(t, VenueId{0});
  b.set_position(kBtc, qt("2"), px("100"));
  snapshot(b, kBtc, "109", "111");
  CHECK(b.positions()[kBtc].unrealized == nt("20"));
  CHECK(b.positions().gross_exposure() == nt("220"));
  // Another venue's instrument has no book here.
  alignas(64) std::byte bytes[BookDeltaMsg::size_for(0, 0)]{};
  auto* d = reinterpret_cast<BookDeltaMsg*>(bytes);
  init_header(*d, EventType::BookDelta, kSol, VenueId{1}, BookDeltaMsg::size_for(0, 0));
  CHECK_FALSE(b.on_book(*d));
  // A market-data outage clears the books; the next mark waits for a snapshot.
  ConnectionStateMsg cs{};
  init_header(cs, EventType::ConnectionState, InstrumentId::invalid(), VenueId{0});
  cs.state = ConnState::Resyncing;
  cs.channel = 0;
  b.on_connection_state(cs);
  init_header(*d, EventType::BookDelta, kBtc, VenueId{0}, BookDeltaMsg::size_for(0, 0));
  CHECK_FALSE(b.on_book(*d));
}

TEST_CASE("core.account_book: the exposure check lets an order that reduces its position through") {
  Position flat{};
  Position longp{};
  longp.qty = qt("1");
  const Notional n = nt("60");
  // gross 50 + 60 > 100
  CHECK(check_exposure(flat, Side::Buy, n, nt("50"), nt("50"), nt("100"), Notional{}) ==
        RejectReason::GatewayGrossNotional);
  CHECK(check_exposure(longp, Side::Buy, n, nt("50"), nt("50"), nt("100"), Notional{}) ==
        RejectReason::GatewayGrossNotional);
  CHECK(check_exposure(longp, Side::Sell, n, nt("50"), nt("50"), nt("100"), Notional{}) ==
        RejectReason::None);
  CHECK(check_exposure(flat, Side::Buy, n, nt("30"), nt("30"), nt("100"), Notional{}) ==
        RejectReason::None);
  // net 50 + 60 > 100; a sell takes the net towards zero and passes even when flat here
  CHECK(check_exposure(flat, Side::Buy, n, nt("50"), nt("50"), Notional{}, nt("100")) ==
        RejectReason::GatewayNetNotional);
  CHECK(check_exposure(flat, Side::Sell, n, nt("50"), nt("50"), Notional{}, nt("100")) ==
        RejectReason::None);
  // already over the net cap: an order that brings it towards zero is how you get back under
  CHECK(check_exposure(flat, Side::Sell, n, nt("150"), nt("150"), Notional{}, nt("100")) ==
        RejectReason::None);
  CHECK(check_exposure(flat, Side::Buy, n, nt("0"), nt("0"), Notional{}, Notional{}) ==
        RejectReason::None);
}

TEST_CASE("core.account_book: with [accounting] the account keeps its totals per currency") {
  InstrumentTable t = make_table();
  // ETHUSDT becomes ETHBTC: it settles in BTC, and BTCUSDT prices BTC.
  Instrument& eth = t.get(kEth);
  eth.symbol = "ETHBTC";
  eth.base = "ETH";
  eth.quote = "BTC";
  t.get(kBtc).base = "BTC";
  t.get(kBtc).quote = "USDT";
  t.get(kSol).base = "SOL";
  t.get(kSol).quote = "USDT";
  AccountingSpec spec;
  spec.reporting_currency = "USDT";
  spec.fx["BTC"] = "a:BTCUSDT";
  const std::vector<std::string> venues{"a", "b"};
  const auto plan = build_fx_plan(t, spec, venues, /*all_instruments=*/true);
  REQUIRE(plan.has_value());
  AccountBook b(t, VenueId{0}, *plan);
  take(b, fill(kBtc, Side::Buy, "100", "1", "t1"));   // 0.1 USDT fee
  take(b, fill(kEth, Side::Buy, "0.05", "2", "t2"));  // 0.1 BTC fee
  snapshot(b, kEth, "0.049", "0.051");
  const PositionTracker& p = b.positions();
  REQUIRE(p.converting());
  CHECK(p.native(0).fees == nt("0.1"));
  CHECK(p.native(1).fees == nt("0.1"));
  CHECK(p.native(1).gross == nt("0.1"));  // 2 ETH at 0.05 BTC
  CHECK(p.native(0).gross == nt("100"));  // BTCUSDT at its fill price
}
