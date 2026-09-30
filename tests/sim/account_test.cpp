// The simulated venue's account (sim/sim_account.hpp): holds, fills, fees, refusals and the
// BalanceMsg reports, alone and through SimTransport.
#include "test_support.hpp"

#include "fastmm/sim/sim_account.hpp"
#include "fastmm/sim/sim_transport.hpp"

#include <cstring>
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

constexpr std::int64_t kT0 = 1'700'000'000'000'000'000;
Timestamp at(std::int64_t us) {
  return Timestamp{kT0 + us * 1000};
}

// BTCUSDT spot and BTCUSDT-PERP (linear, settles in USDT) on venue 0, ETHUSDT on venue 1.
InstrumentTable table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.min_qty = qt("0.001");
  i.base = "BTC";
  i.quote = "USDT";
  REQUIRE(t.add(i));
  Instrument p = i;
  p.symbol = "BTCUSDT-PERP";
  p.asset_class = AssetClass::Perpetual;
  REQUIRE(t.add(p));
  Instrument e = i;
  e.symbol = "ETHUSDT";
  e.venue = VenueId{1};
  e.base = "ETH";
  REQUIRE(t.add(e));
  return t;
}

constexpr InstrumentId kSpot{0};
constexpr InstrumentId kPerp{1};
constexpr InstrumentId kEth{2};

std::vector<sim::SimAccountConfig> one_account(const char* btc, const char* usdt) {
  return {sim::SimAccountConfig{VenueId{0},
                                {sim::SimBalance{FixedString<8>("BTC"), nt(btc)},
                                 sim::SimBalance{FixedString<8>("USDT"), nt(usdt)}}}};
}

std::vector<BalanceMsg> published(sim::SimAccounts& a, VenueId v = VenueId{0}) {
  std::vector<BalanceMsg> out;
  a.publish(v, at(1), [&](BalanceMsg& m) { out.push_back(m); });
  return out;
}

}  // namespace

TEST_CASE("sim.account: a spot buy holds its notional and a fill moves both assets") {
  const InstrumentTable t = table();
  const auto cfg = one_account("1", "1000");
  sim::SimAccounts a(t, cfg, {});
  CHECK(a.enabled(kSpot));
  CHECK_FALSE(a.enabled(kEth));
  REQUIRE(a.admit(ClientOrderId{1}, kSpot, Side::Buy, px("100"), qt("0.5"), false));
  CHECK(a.locked(VenueId{0}, "USDT") == nt("50"));
  CHECK(a.free(VenueId{0}, "USDT") == nt("950"));
  // 0.2 filled at 99 (better than the order's 100), fee 0.01 USDT.
  a.fill(ClientOrderId{1}, px("99"), qt("0.2"), qt("0.3"), nt("0.01"));
  CHECK(a.locked(VenueId{0}, "USDT") == nt("30"));
  CHECK(a.total(VenueId{0}, "USDT") == nt("980.19"));
  CHECK(a.total(VenueId{0}, "BTC") == nt("1.2"));
  CHECK(a.free(VenueId{0}, "BTC") == nt("1.2"));
  a.close(ClientOrderId{1});
  CHECK(a.locked(VenueId{0}, "USDT").is_zero());
  CHECK(a.free(VenueId{0}, "USDT") == nt("980.19"));
  CHECK(a.open_orders() == 0);
  // The last fill ends the order: nothing stays held.
  REQUIRE(a.admit(ClientOrderId{2}, kSpot, Side::Sell, px("101"), qt("0.2"), false));
  CHECK(a.locked(VenueId{0}, "BTC") == nt("0.2"));
  a.fill(ClientOrderId{2}, px("101"), qt("0.2"), Qty{}, nt("0.02"));
  CHECK(a.locked(VenueId{0}, "BTC").is_zero());
  CHECK(a.total(VenueId{0}, "BTC") == nt("1"));
  CHECK(a.total(VenueId{0}, "USDT") == nt("1000.37"));
  CHECK(a.open_orders() == 0);
}

TEST_CASE("sim.account: an order the free balance does not cover is refused") {
  const InstrumentTable t = table();
  const auto cfg = one_account("0.00024", "20");
  sim::SimAccounts a(t, cfg, {});
  CHECK_FALSE(a.admit(ClientOrderId{1}, kSpot, Side::Sell, px("100"), qt("0.00025"), false));
  REQUIRE(a.admit(ClientOrderId{2}, kSpot, Side::Sell, px("100"), qt("0.0002"), false));
  // What is held is not free: a second sell for the rest of the balance plus one lot is refused.
  CHECK_FALSE(a.admit(ClientOrderId{3}, kSpot, Side::Sell, px("100"), qt("0.00005"), false));
  REQUIRE(a.admit(ClientOrderId{4}, kSpot, Side::Sell, px("100"), qt("0.00004"), false));
  CHECK(a.free(VenueId{0}, "BTC").is_zero());
  // A buy for more than the free quote.
  CHECK_FALSE(a.admit(ClientOrderId{5}, kSpot, Side::Buy, px("100"), qt("0.201"), false));
  REQUIRE(a.admit(ClientOrderId{6}, kSpot, Side::Buy, px("100"), qt("0.2"), false));
  // A replace is checked against what it frees: the same buy one tick higher does not fit,
  // one tick lower does, and the old hold goes.
  CHECK_FALSE(a.admit_replace(ClientOrderId{6}, ClientOrderId{7}, px("100.01"), qt("0.2")));
  REQUIRE(a.admit_replace(ClientOrderId{6}, ClientOrderId{8}, px("99.99"), qt("0.2")));
  CHECK(a.locked(VenueId{0}, "USDT") == nt("19.998"));
  CHECK(a.open_orders() == 3);
  // An unknown original is the venue's to answer.
  CHECK(a.admit_replace(ClientOrderId{99}, ClientOrderId{9}, px("1000"), qt("1")));
  CHECK(a.open_orders() == 3);
}

TEST_CASE("sim.account: a derivative holds initial margin and realises PnL into the wallet") {
  const InstrumentTable t = table();
  const auto cfg = one_account("0", "1000");
  std::vector<Ratio> im(t.size());
  im[kPerp.value] = Ratio::from_decimal("0.1").value();
  sim::SimAccounts a(t, cfg, im);
  // Buys and sells of one instrument: the larger side holds.
  REQUIRE(a.admit(ClientOrderId{1}, kPerp, Side::Buy, px("100"), qt("20"), false));
  REQUIRE(a.admit(ClientOrderId{2}, kPerp, Side::Sell, px("110"), qt("10"), false));
  CHECK(a.locked(VenueId{0}, "USDT") == nt("200"));
  // 81 more at 100 would add 810 of margin to the buy side; 800 is free.
  CHECK_FALSE(a.admit(ClientOrderId{3}, kPerp, Side::Buy, px("100"), qt("81"), false));
  a.fill(ClientOrderId{1}, px("100"), qt("20"), Qty{}, nt("0.2"));
  // Long 20 at 100: 200 of position margin, the sell order's 110 is under it.
  CHECK(a.position(kPerp) == qt("20"));
  CHECK(a.total(VenueId{0}, "USDT") == nt("999.8"));
  CHECK(a.locked(VenueId{0}, "USDT") == nt("310"));
  // A sell that only reduces the long needs no margin, even with nothing free.
  REQUIRE(a.admit(ClientOrderId{4}, kPerp, Side::Buy, px("100"), qt("79.98"), false));
  CHECK(a.free(VenueId{0}, "USDT").is_zero());
  CHECK_FALSE(a.admit(ClientOrderId{6}, kPerp, Side::Buy, px("100"), qt("0.001"), false));
  REQUIRE(a.admit(ClientOrderId{5}, kPerp, Side::Sell, px("100"), qt("5"), false));
  CHECK(a.free(VenueId{0}, "USDT").is_zero());
  a.close(ClientOrderId{4});
  // Closing 10 at 110: +100 realised, less the fee.
  a.fill(ClientOrderId{2}, px("110"), qt("10"), Qty{}, nt("0.11"));
  CHECK(a.position(kPerp) == qt("10"));
  CHECK(a.total(VenueId{0}, "USDT") == nt("1099.69"));
  CHECK(a.locked(VenueId{0}, "USDT") == nt("100"));
}

TEST_CASE("sim.account: a derivative's unrealised PnL counts in its free margin and equity") {
  const InstrumentTable t = table();
  const auto cfg = one_account("0", "1000");
  std::vector<Ratio> im(t.size());
  im[kPerp.value] = Ratio::from_decimal("0.1").value();
  sim::SimAccounts a(t, cfg, im);
  REQUIRE(a.admit(ClientOrderId{1}, kPerp, Side::Buy, px("100"), qt("50"), false));
  a.fill(ClientOrderId{1}, px("100"), qt("50"), Qty{}, Notional{});
  CHECK(a.free(VenueId{0}, "USDT") == nt("500"));  // 1000 - 500 of position margin
  // No mark yet: nothing unrealised. The book's mid at 90: long 50 is 500 down.
  a.mark(kPerp, px("90"), false);
  CHECK(a.unrealized(VenueId{0}, "USDT") == nt("-500"));
  CHECK(a.free(VenueId{0}, "USDT").is_zero());
  CHECK_FALSE(a.admit(ClientOrderId{2}, kPerp, Side::Buy, px("90"), qt("0.1"), false));
  // The venue's mark takes over from the mid, and a later mid does not replace it.
  a.mark(kPerp, px("104"), true);
  a.mark(kPerp, px("80"), false);
  CHECK(a.unrealized(VenueId{0}, "USDT") == nt("200"));
  CHECK(a.free(VenueId{0}, "USDT") == nt("700"));
  REQUIRE(a.admit(ClientOrderId{3}, kPerp, Side::Buy, px("104"), qt("60"), false));  // 624
  std::vector<BalanceMsg> r = published(a);
  REQUIRE(r.size() == 1);
  CHECK(r[0].total == nt("1000"));
  CHECK(r[0].equity == nt("1200"));
  CHECK(r[0].locked == nt("1124"));
  CHECK(r[0].free == nt("76"));
  // Spot rows have none.
  CHECK(a.unrealized(VenueId{0}, "BTC").is_zero());
}

TEST_CASE("sim.account: reports: a snapshot of every row, then only the rows that moved") {
  const InstrumentTable t = table();
  std::vector<sim::SimAccountConfig> cfg = one_account("1", "1000");
  cfg.push_back(
      sim::SimAccountConfig{VenueId{1}, {sim::SimBalance{FixedString<8>("USDT"), nt("5")}}});
  sim::SimAccounts a(t, cfg, {});
  std::vector<BalanceMsg> snap;
  a.snapshot(VenueId{0}, at(0), [&](BalanceMsg& m) { snap.push_back(m); });
  REQUIRE(snap.size() == 2);  // BTC and USDT; the perpetual settles in USDT too
  for (std::size_t i = 0; i < snap.size(); ++i) {
    CHECK(snap[i].hdr.type == EventType::Balance);
    CHECK(snap[i].hdr.venue == VenueId{0});
    CHECK_FALSE(snap[i].hdr.instrument.valid());
    CHECK(snap[i].hdr.exch_ts == at(0));
    CHECK((snap[i].flags & BalanceMsg::kSnapshot) != 0);
    CHECK(((snap[i].flags & BalanceMsg::kSnapshotEnd) != 0) == (i + 1 == snap.size()));
  }
  CHECK(snap[1].asset.view() == "USDT");
  CHECK(snap[1].free == nt("1000"));
  CHECK(snap[1].total == nt("1000"));
  CHECK(published(a).empty());
  REQUIRE(a.admit(ClientOrderId{1}, kSpot, Side::Buy, px("100"), qt("1"), false));
  std::vector<BalanceMsg> up = published(a);
  REQUIRE(up.size() == 1);
  CHECK(up[0].asset.view() == "USDT");
  CHECK(up[0].flags == 0);
  CHECK(up[0].free == nt("900"));
  CHECK(up[0].locked == nt("100"));
  CHECK(up[0].total == nt("1000"));
  CHECK(published(a).empty());
  // An order refused changes nothing; one placed and ended at once changes nothing either.
  CHECK_FALSE(a.admit(ClientOrderId{2}, kSpot, Side::Buy, px("100"), qt("10"), false));
  REQUIRE(a.admit(ClientOrderId{3}, kSpot, Side::Buy, px("100"), qt("1"), false));
  a.close(ClientOrderId{3});
  CHECK(published(a).empty());
  a.fill(ClientOrderId{1}, px("100"), qt("1"), Qty{}, Notional{});
  up = published(a);
  CHECK(up.size() == 2);
  // The other venue's account is its own.
  CHECK(published(a, VenueId{1}).empty());
  CHECK(a.total(VenueId{1}, "USDT") == nt("5"));
  CHECK(a.total(VenueId{1}, "ETH").is_zero());
}

TEST_CASE("sim.account: configuration errors") {
  const InstrumentTable t = table();
  const std::vector<sim::SimAccountConfig> none_traded = {sim::SimAccountConfig{VenueId{3}, {}}};
  CHECK_THROWS_AS(sim::SimAccounts(t, none_traded, {}), std::invalid_argument);
  const std::vector<sim::SimAccountConfig> twice = {
      sim::SimAccountConfig{VenueId{0},
                            {sim::SimBalance{FixedString<8>("BTC"), nt("1")},
                             sim::SimBalance{FixedString<8>("btc"), nt("2")}}}};
  CHECK_THROWS_AS(sim::SimAccounts(t, twice, {}), std::invalid_argument);
}

namespace {

struct Wire {
  std::vector<std::vector<std::byte>> msgs;
  template <class M>
  [[nodiscard]] const M& as(std::size_t i) const {
    return *reinterpret_cast<const M*>(msgs[i].data());
  }
  [[nodiscard]] EventType type(std::size_t i) const {
    return reinterpret_cast<const EventHeader*>(msgs[i].data())->type;
  }
};

// Every order-wire message the venue sends up to `until`, venue events processed in time order.
Wire drain(sim::SimTransport& v, SimClock& clock, InlineFeed& feed, Timestamp until) {
  Wire w;
  for (;;) {
    const Timestamp a = v.next_order_arrival();
    const Timestamp b = v.next_inbound_ts();
    const Timestamp t = a < b ? a : b;
    if (t > until) break;
    if (t > clock.now()) clock.set(t);
    if (t == a) {
      v.process_order_arrival();
      continue;
    }
    static_cast<void>(v.deliver_next_inbound(feed));
    const EventHeader* h = feed.next();
    REQUIRE(h != nullptr);
    if (h->type != EventType::BookSnapshot && h->type != EventType::BookTicker &&
        h->type != EventType::Trade && h->type != EventType::BookDelta) {
      const auto* p = reinterpret_cast<const std::byte*>(h);
      w.msgs.emplace_back(p, p + h->len);
    }
    feed.release();
  }
  return w;
}

OutNewOrderMsg new_order(
    std::uint64_t id, Side side, const char* p, const char* q, InstrumentId inst = kSpot) {
  OutNewOrderMsg m{};
  init_header(m, EventType::OutNewOrder, inst, VenueId{0});
  m.cl_ord_id = ClientOrderId{id};
  m.side = side;
  m.type = OrderType::PostOnly;
  m.tif = TimeInForce::Gtc;
  m.price = px(p);
  m.qty = qt(q);
  return m;
}

void book(sim::SimTransport& v,
          Timestamp ts,
          const char* bid,
          const char* ask,
          InstrumentId inst = kSpot) {
  alignas(64) std::byte buf[BookDeltaMsg::size_for(1, 1)] = {};
  auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
  init_header(*d, EventType::BookSnapshot, inst, VenueId{0}, sizeof buf);
  d->hdr.flags |= EventHeader::kSnapshot;
  d->hdr.exch_ts = ts;
  d->hdr.recv_ts = ts;
  d->bid_count = d->ask_count = 1;
  d->levels()[0] = Level{px(bid), qt("1")};
  d->levels()[1] = Level{px(ask), qt("1")};
  v.on_source_event(d->hdr);
}

void trade(sim::SimTransport& v,
           Timestamp ts,
           const char* p,
           const char* q,
           Side aggressor,
           InstrumentId inst = kSpot) {
  TradeMsg m{};
  init_header(m, EventType::Trade, inst, VenueId{0});
  m.hdr.exch_ts = ts;
  m.hdr.recv_ts = ts;
  m.price = px(p);
  m.qty = qt(q);
  m.aggressor = aggressor;
  v.on_source_event(m.hdr);
}

}  // namespace

TEST_CASE("sim.account: the venue refuses what the account cannot cover and reports its balances") {
  const InstrumentTable t = table();
  SimClock clock{at(0)};
  sim::SimTransportConfig tc;
  tc.fill_model = sim::FillModel::L2Queue;
  tc.order_out = sim::LatencyParams{microseconds(100), Duration{}};
  tc.ack_in = sim::LatencyParams{microseconds(200), Duration{}};
  tc.accounts = one_account("0.5", "100");
  sim::SimTransport v(clock, t, tc);
  InlineFeed feed(1 << 20);
  v.publish_balances(clock.now());
  book(v, at(0), "99.00", "101.00");

  Wire w = drain(v, clock, feed, at(10));
  REQUIRE(w.msgs.size() == 2);  // the start snapshot: BTC, USDT
  CHECK(w.as<BalanceMsg>(0).asset.view() == "BTC");
  CHECK(w.as<BalanceMsg>(1).asset.view() == "USDT");
  CHECK((w.as<BalanceMsg>(1).flags & BalanceMsg::kSnapshotEnd) != 0);
  CHECK(w.as<BalanceMsg>(1).hdr.recv_ts == at(0));

  // A sell of 0.6 BTC with 0.5: refused as Binance refuses it, nothing else sent.
  REQUIRE(v.send(new_order(1, Side::Sell, "101.00", "0.6").hdr));
  w = drain(v, clock, feed, at(1000));
  REQUIRE(w.msgs.size() == 1);
  REQUIRE(w.type(0) == EventType::OrderReject);
  CHECK(w.as<OrderRejectMsg>(0).reason == RejectReason::InsufficientBalance);
  CHECK(w.as<OrderRejectMsg>(0).venue_code == -2010);
  CHECK(w.as<OrderRejectMsg>(0).hdr.exch_ts == at(100));
  CHECK(v.stats().rejects_balance == 1);
  CHECK(v.accounts()->stats().refused == 1);

  // A buy of 0.5 at 99: acknowledged, then the USDT row with 49.5 locked, behind the ack.
  REQUIRE(v.send(new_order(2, Side::Buy, "99.00", "0.5").hdr));
  w = drain(v, clock, feed, at(2000));
  REQUIRE(w.msgs.size() == 2);
  CHECK(w.type(0) == EventType::OrderAck);
  REQUIRE(w.type(1) == EventType::Balance);
  CHECK(w.as<BalanceMsg>(1).asset.view() == "USDT");
  CHECK(w.as<BalanceMsg>(1).locked == nt("49.5"));
  CHECK(w.as<BalanceMsg>(1).free == nt("50.5"));
  CHECK(w.as<BalanceMsg>(1).hdr.exch_ts == w.as<OrderAckMsg>(0).hdr.exch_ts);
  CHECK(w.as<BalanceMsg>(1).hdr.recv_ts == w.as<OrderAckMsg>(0).hdr.recv_ts);

  // A sell trade through 99 fills it: the fill, then BTC and USDT.
  trade(v, at(3000), "99.00", "2", Side::Sell);
  w = drain(v, clock, feed, at(4000));
  REQUIRE(w.msgs.size() == 3);
  CHECK(w.type(0) == EventType::OrderFill);
  CHECK(w.as<BalanceMsg>(1).asset.view() == "BTC");
  CHECK(w.as<BalanceMsg>(1).total == nt("1"));
  CHECK(w.as<BalanceMsg>(2).asset.view() == "USDT");
  CHECK(w.as<BalanceMsg>(2).total == nt("50.5"));
  CHECK(w.as<BalanceMsg>(2).locked.is_zero());

  // Now 1 BTC: the sell of 0.6 goes through.
  REQUIRE(v.send(new_order(3, Side::Sell, "101.00", "0.6").hdr));
  w = drain(v, clock, feed, at(5000));
  REQUIRE(w.msgs.size() == 2);
  CHECK(w.type(0) == EventType::OrderAck);
  CHECK(w.as<BalanceMsg>(1).locked == nt("0.6"));
}

TEST_CASE("sim.account: without accounts the venue sends no balances") {
  const InstrumentTable t = table();
  SimClock clock{at(0)};
  sim::SimTransportConfig tc;
  tc.fill_model = sim::FillModel::L2Queue;
  sim::SimTransport v(clock, t, tc);
  InlineFeed feed(1 << 20);
  v.publish_balances(clock.now());
  CHECK(v.accounts() == nullptr);
  book(v, at(0), "99.00", "101.00");
  REQUIRE(v.send(new_order(1, Side::Sell, "101.00", "1000").hdr));
  const Wire w = drain(v, clock, feed, at(10'000));
  REQUIRE(w.msgs.size() == 1);
  CHECK(w.type(0) == EventType::OrderAck);
}

TEST_CASE("sim.account: the venue marks a derivative at its mark price, else the book's mid") {
  const InstrumentTable t = table();
  SimClock clock{at(0)};
  sim::SimTransportConfig tc;
  tc.fill_model = sim::FillModel::L2Queue;
  tc.order_out = sim::LatencyParams{microseconds(100), Duration{}};
  tc.ack_in = sim::LatencyParams{microseconds(100), Duration{}};
  tc.accounts = one_account("0", "1000");
  tc.initial_margin.assign(t.size(), Ratio{});
  tc.initial_margin[kPerp.value] = Ratio::from_decimal("0.1").value();
  sim::SimTransport v(clock, t, tc);
  InlineFeed feed(1 << 20);
  book(v, at(0), "99.00", "101.00", kPerp);
  REQUIRE(v.send(new_order(1, Side::Buy, "99.00", "10", kPerp).hdr));
  static_cast<void>(drain(v, clock, feed, at(1000)));
  trade(v, at(1000), "98.00", "1", Side::Sell, kPerp);  // through 99: long 10 at 99
  static_cast<void>(drain(v, clock, feed, at(2000)));
  REQUIRE(v.accounts()->position(kPerp) == qt("10"));
  book(v, at(2000), "89.00", "91.00", kPerp);
  CHECK(v.accounts()->unrealized(VenueId{0}, "USDT") == nt("-90"));
  PerpStateMsg p{};
  init_header(p, EventType::PerpState, kPerp, VenueId{0});
  p.hdr.exch_ts = at(3000);
  p.hdr.recv_ts = at(3000);
  p.mark_price = px("100");
  p.fields = PerpStateMsg::kMark;
  v.on_source_event(p.hdr);
  book(v, at(4000), "79.00", "81.00", kPerp);
  CHECK(v.accounts()->unrealized(VenueId{0}, "USDT") == nt("10"));
  CHECK(v.accounts()->free(VenueId{0}, "USDT") == nt("911"));  // 1000 + 10 - 99 of margin
}
