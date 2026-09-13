// MatchingEngine semantics: price-time priority, partial fills, IOC/FOK/post-only, STP,
// replace priority rules, update_id monotonicity, quantity conservation.
#include "test_support.hpp"

#include "fastmm/sim/matching_engine.hpp"

#include <vector>

using namespace fastmm;
using namespace fastmm::sim;

namespace {
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

struct Event {
  enum Kind { Ack, Reject, Cancel, CancelReject, Fill, Book, Trade } kind;
  std::uint64_t a = 0;  // order id / maker id / update id
  std::uint64_t b = 0;  // taker id / trade id
  Price price{};
  Qty qty{};
  std::uint8_t code = 0;  // reason / side
};

struct Recorder final : MatchingSink {
  std::vector<Event> ev;
  std::uint64_t last_update_id = 0;
  bool monotonic = true;
  void on_ack(const SimOrder& o, Timestamp) override {
    ev.push_back({Event::Ack, o.order_id, o.cl_ord_id.value, o.price, o.qty, 0});
  }
  void on_reject(const NewOrder& o, RejectReason r, Timestamp) override {
    ev.push_back(
        {Event::Reject, 0, o.cl_ord_id.value, o.price, o.qty, static_cast<std::uint8_t>(r)});
  }
  void on_cancel(const SimOrder& o, CancelReason r, Timestamp) override {
    ev.push_back({Event::Cancel,
                  o.order_id,
                  o.cl_ord_id.value,
                  o.price,
                  o.leaves(),
                  static_cast<std::uint8_t>(r)});
  }
  void on_cancel_reject(AccountId, ClientOrderId id, InstrumentId, Timestamp) override {
    ev.push_back({Event::CancelReject, 0, id.value, {}, {}, 0});
  }
  void on_fill(const SimOrder& m, const SimOrder& t, Price p, Qty q, Timestamp) override {
    ev.push_back({Event::Fill, m.order_id, t.order_id, p, q, 0});
  }
  void on_book_change(InstrumentId, Side s, Price p, Qty q, std::uint64_t u) override {
    if (u <= last_update_id) monotonic = false;
    last_update_id = u;
    ev.push_back({Event::Book, u, 0, p, q, static_cast<std::uint8_t>(s)});
  }
  void on_trade(InstrumentId, Price p, Qty q, Side s, std::uint64_t id, Timestamp) override {
    ev.push_back({Event::Trade, id, 0, p, q, static_cast<std::uint8_t>(s)});
  }
  std::size_t count(Event::Kind k) const {
    std::size_t n = 0;
    for (const Event& e : ev) n += e.kind == k ? 1U : 0U;
    return n;
  }
  const Event* find(Event::Kind k, std::size_t nth = 0) const {
    for (const Event& e : ev) {
      if (e.kind == k && nth-- == 0) return &e;
    }
    return nullptr;
  }
  Qty filled_total() const {
    Qty q{};
    for (const Event& e : ev) {
      if (e.kind == Event::Fill) q += e.qty;
    }
    return q;
  }
};

struct Fixture {
  Recorder rec;
  MatchingEngine me{1, &rec};
  std::uint64_t next_id = 1;
  Timestamp now{1'000};

  NewOrder mk(AccountId acct,
              Side s,
              const char* p,
              const char* q,
              OrderType t = OrderType::Limit,
              TimeInForce tif = TimeInForce::Gtc) {
    NewOrder o;
    o.account = acct;
    o.cl_ord_id = ClientOrderId{next_id++};
    o.instrument = InstrumentId{0};
    o.side = s;
    o.type = t;
    o.tif = tif;
    o.price = px(p);
    o.qty = qt(q);
    return o;
  }
  SubmitResult submit(AccountId acct,
                      Side s,
                      const char* p,
                      const char* q,
                      OrderType t = OrderType::Limit,
                      TimeInForce tif = TimeInForce::Gtc) {
    now.ns += 1;
    return me.submit(mk(acct, s, p, q, t, tif), now);
  }
  ClientOrderId last_id() const { return ClientOrderId{next_id - 1}; }
};
}  // namespace

TEST_CASE("sim.matching: resting limit orders build the book, best at top, update ids monotonic") {
  Fixture f;
  auto r1 = f.submit(0, Side::Buy, "100.00", "1");
  auto r2 = f.submit(0, Side::Buy, "100.10", "2");
  auto r3 = f.submit(0, Side::Sell, "100.50", "3");
  auto r4 = f.submit(0, Side::Sell, "100.40", "4");
  CHECK(r1.accepted());
  CHECK(r1.resting == qt("1"));
  CHECK(r2.order_id == 2);
  CHECK(r3.order_id == 3);
  CHECK(r4.order_id == 4);
  const auto top = f.me.top_of_book(InstrumentId{0});
  CHECK(top.bid == Level{px("100.10"), qt("2")});
  CHECK(top.ask == Level{px("100.40"), qt("4")});
  Level lv[4];
  REQUIRE(f.me.l2_snapshot(InstrumentId{0}, Side::Buy, lv, 4) == 2);
  CHECK(lv[0].price == px("100.10"));
  CHECK(lv[1].price == px("100.00"));
  REQUIRE(f.me.l2_snapshot(InstrumentId{0}, Side::Sell, lv, 4) == 2);
  CHECK(lv[0].price == px("100.40"));
  CHECK(f.me.update_id(InstrumentId{0}) == 4);
  CHECK(f.rec.monotonic);
  CHECK(f.me.open_orders() == 4);
  CHECK(f.rec.count(Event::Ack) == 4);
  CHECK(f.rec.count(Event::Book) == 4);
}

TEST_CASE("sim.matching: price-time priority and partial fills at one level") {
  Fixture f;
  f.submit(0, Side::Sell, "101", "2");  // id 1, first in queue
  f.submit(0, Side::Sell, "101", "3");  // id 2
  f.submit(0, Side::Sell, "100", "1");  // id 3, better price
  auto r = f.submit(1, Side::Buy, "101", "4");
  CHECK(r.filled == qt("4"));
  CHECK(r.resting.is_zero());
  REQUIRE(f.rec.count(Event::Fill) == 3);
  const Event* f0 = f.rec.find(Event::Fill, 0);
  const Event* f1 = f.rec.find(Event::Fill, 1);
  const Event* f2 = f.rec.find(Event::Fill, 2);
  CHECK(f0->a == 3);  // best price first
  CHECK(f0->price == px("100"));
  CHECK(f0->qty == qt("1"));
  CHECK(f1->a == 1);  // then time priority at 101
  CHECK(f1->qty == qt("2"));
  CHECK(f2->a == 2);  // partial of the last maker
  CHECK(f2->qty == qt("1"));
  const SimOrder* rest = f.me.find(0, ClientOrderId{2});
  REQUIRE(rest != nullptr);
  CHECK(rest->leaves() == qt("2"));
  CHECK(f.me.top_of_book(InstrumentId{0}).ask == Level{px("101"), qt("2")});
  CHECK(f.me.ledger(1).bought == qt("4"));
  CHECK(f.me.ledger(0).sold == qt("4"));
  CHECK(f.me.ledger(1).buy_notional == Notional::from_decimal("403").value());
  CHECK(f.rec.count(Event::Trade) == 3);
  CHECK(f.rec.monotonic);
}

TEST_CASE("sim.matching: limit crossing the book rests the remainder at its own price") {
  Fixture f;
  f.submit(0, Side::Sell, "100", "1");
  auto r = f.submit(1, Side::Buy, "100.5", "3");
  CHECK(r.filled == qt("1"));
  CHECK(r.resting == qt("2"));
  CHECK(f.me.top_of_book(InstrumentId{0}).bid == Level{px("100.5"), qt("2")});
  CHECK(f.me.top_of_book(InstrumentId{0}).ask == Level{});
  CHECK(f.rec.find(Event::Fill)->price == px("100"));  // taker gets the maker's price
}

TEST_CASE("sim.matching: IOC / FOK / market / post-only semantics") {
  Fixture f;
  f.submit(0, Side::Sell, "100", "1");
  f.submit(0, Side::Sell, "101", "1");

  SUBCASE("IOC fills what it can and expires the rest") {
    auto r = f.submit(1, Side::Buy, "100", "5", OrderType::Limit, TimeInForce::Ioc);
    CHECK(r.filled == qt("1"));
    CHECK(r.resting.is_zero());
    const Event* c = f.rec.find(Event::Cancel);
    REQUIRE(c != nullptr);
    CHECK(c->code == static_cast<std::uint8_t>(CancelReason::Ioc));
    CHECK(c->qty == qt("4"));
    CHECK(f.me.open_orders() == 1);
    CHECK(f.me.stats().expired == 1);
  }
  SUBCASE("FOK is all or nothing") {
    auto r = f.submit(1, Side::Buy, "101", "3", OrderType::Limit, TimeInForce::Fok);
    CHECK(r.filled.is_zero());
    CHECK(f.rec.count(Event::Fill) == 0);
    CHECK(f.rec.find(Event::Cancel)->code == static_cast<std::uint8_t>(CancelReason::Fok));
    CHECK(f.me.open_orders() == 2);  // untouched
    r = f.submit(1, Side::Buy, "101", "2", OrderType::Limit, TimeInForce::Fok);
    CHECK(r.filled == qt("2"));
    CHECK(f.me.open_orders() == 0);
  }
  SUBCASE("market walks the book and cancels the remainder") {
    auto r = f.submit(1, Side::Buy, "0", "5", OrderType::Market);
    CHECK(r.accepted());
    CHECK(r.filled == qt("2"));
    REQUIRE(f.rec.count(Event::Fill) == 2);
    CHECK(f.rec.find(Event::Fill, 0)->price == px("100"));
    CHECK(f.rec.find(Event::Fill, 1)->price == px("101"));
    CHECK(f.rec.find(Event::Cancel)->code == static_cast<std::uint8_t>(CancelReason::NoLiquidity));
    CHECK(f.me.open_orders() == 0);
  }
  SUBCASE("post-only rests when passive and is rejected when it would cross") {
    auto ok = f.submit(1, Side::Buy, "99.99", "1", OrderType::PostOnly);
    CHECK(ok.accepted());
    CHECK(ok.resting == qt("1"));
    auto bad = f.submit(1, Side::Buy, "100", "1", OrderType::PostOnly);
    CHECK(bad.reason == RejectReason::PostOnlyWouldCross);
    CHECK(bad.order_id == 0);
    CHECK(f.rec.count(Event::Reject) == 1);
    CHECK(f.rec.count(Event::Fill) == 0);
    CHECK(f.me.stats().rejects == 1);
  }
  SUBCASE("validation rejects") {
    CHECK(f.submit(1, Side::Buy, "0", "1").reason == RejectReason::InvalidTick);
    CHECK(f.submit(1, Side::Buy, "1", "0").reason == RejectReason::InvalidLot);
    NewOrder dup = f.mk(0, Side::Buy, "1", "1");
    dup.cl_ord_id = ClientOrderId{1};  // already resting for account 0
    CHECK(f.me.submit(dup, f.now).reason == RejectReason::DuplicateId);
    dup.instrument = InstrumentId{7};
    CHECK(f.me.submit(dup, f.now).reason == RejectReason::InstrumentDisabled);
  }
}

TEST_CASE("sim.matching: cancel semantics and cancel reject") {
  Fixture f;
  f.submit(0, Side::Buy, "100", "1");
  f.submit(0, Side::Buy, "100", "2");
  CHECK(f.me.cancel(0, ClientOrderId{1}, f.now));
  CHECK(f.me.top_of_book(InstrumentId{0}).bid == Level{px("100"), qt("2")});
  CHECK_FALSE(f.me.cancel(0, ClientOrderId{1}, f.now));  // gone
  CHECK_FALSE(f.me.cancel(1, ClientOrderId{2}, f.now));  // wrong account
  CHECK(f.rec.count(Event::CancelReject) == 2);
  CHECK(f.me.cancel(0, ClientOrderId{2}, f.now));
  CHECK(f.me.top_of_book(InstrumentId{0}).bid == Level{});
  CHECK(f.me.book(InstrumentId{0}).depth(Side::Buy) == 0);
  CHECK(f.me.update_id(InstrumentId{0}) == 4);
  CHECK(f.rec.monotonic);
  CHECK(f.me.open_orders() == 0);
}

TEST_CASE("sim.matching: replace keeps priority only for same price and qty <= leaves") {
  Fixture f;
  f.submit(0, Side::Buy, "100", "5");  // id 1 (front)
  f.submit(0, Side::Buy, "100", "5");  // id 2

  SUBCASE("qty decrease at the same price keeps the queue position") {
    auto r = f.me.replace(0, ClientOrderId{2}, ClientOrderId{10}, px("100"), qt("3"), f.now);
    CHECK(r.accepted());
    CHECK(r.order_id == 3);
    CHECK(f.me.find(0, ClientOrderId{2}) == nullptr);
    CHECK(f.me.find(0, ClientOrderId{10})->qty == qt("3"));
    CHECK(f.me.top_of_book(InstrumentId{0}).bid.qty == qt("8"));
    // front order 1 still first: a seller of 6 fills 5 from id 1 then 1 from id 3 (the replaced)
    auto s = f.submit(1, Side::Sell, "100", "6");
    CHECK(s.filled == qt("6"));
    CHECK(f.rec.find(Event::Fill, 0)->a == 1);
    CHECK(f.rec.find(Event::Fill, 1)->a == 3);
  }
  SUBCASE("qty decrease of the FRONT order keeps it in front") {
    auto r = f.me.replace(0, ClientOrderId{1}, ClientOrderId{10}, px("100"), qt("1"), f.now);
    CHECK(r.accepted());
    auto s = f.submit(1, Side::Sell, "100", "1");
    CHECK(s.filled == qt("1"));
    CHECK(f.rec.find(Event::Fill, 0)->a == r.order_id);
  }
  SUBCASE("qty increase loses priority") {
    auto r = f.me.replace(0, ClientOrderId{1}, ClientOrderId{10}, px("100"), qt("6"), f.now);
    CHECK(r.accepted());
    CHECK(r.resting == qt("6"));
    auto s = f.submit(1, Side::Sell, "100", "1");
    CHECK(f.rec.find(Event::Fill, 0)->a == 2);  // id 2 is now first
    CHECK(f.rec.find(Event::Cancel)->code == static_cast<std::uint8_t>(CancelReason::Replaced));
    static_cast<void>(s);
  }
  SUBCASE("price change loses priority and can execute immediately") {
    f.submit(0, Side::Sell, "101", "1");
    auto r = f.me.replace(0, ClientOrderId{1}, ClientOrderId{10}, px("101"), qt("5"), f.now);
    CHECK(r.filled == qt("1"));
    CHECK(r.resting == qt("4"));
    CHECK(f.me.top_of_book(InstrumentId{0}).bid == Level{px("101"), qt("4")});
  }
  SUBCASE("unknown orig -> cancel reject + reject of the new leg") {
    auto r = f.me.replace(0, ClientOrderId{99}, ClientOrderId{10}, px("100"), qt("1"), f.now);
    CHECK(r.reason == RejectReason::VenueUnknownOrder);
    CHECK(f.rec.count(Event::CancelReject) == 1);
    CHECK(f.rec.count(Event::Reject) == 1);
  }
}

TEST_CASE("sim.matching: self-trade prevention modes") {
  for (StpMode mode :
       {StpMode::None, StpMode::CancelTaker, StpMode::CancelMaker, StpMode::CancelBoth}) {
    Fixture f;
    f.me.set_stp(1, mode);
    f.submit(0, Side::Sell, "100", "1");          // id 1, other account, first in queue
    f.submit(1, Side::Sell, "100", "1");          // id 2, own resting
    f.submit(0, Side::Sell, "100", "1");          // id 3
    auto r = f.submit(1, Side::Buy, "100", "3");  // own taker
    CAPTURE(static_cast<int>(mode));
    switch (mode) {
      case StpMode::None:
        CHECK(r.filled == qt("3"));
        CHECK(f.rec.count(Event::Fill) == 3);
        break;
      case StpMode::CancelTaker:
        CHECK(r.filled == qt("1"));  // filled against id 1, then cancelled
        CHECK(f.me.find(1, ClientOrderId{2}) != nullptr);
        CHECK(f.me.find(1, ClientOrderId{4}) == nullptr);
        CHECK(f.rec.count(Event::Cancel) == 1);
        break;
      case StpMode::CancelMaker:
        CHECK(r.filled == qt("2"));  // ids 1 and 3, own maker cancelled
        CHECK(f.me.find(1, ClientOrderId{2}) == nullptr);
        CHECK(f.me.find(1, ClientOrderId{4}) != nullptr);  // remainder rests
        CHECK(f.me.find(1, ClientOrderId{4})->leaves() == qt("1"));
        break;
      case StpMode::CancelBoth:
        CHECK(r.filled == qt("1"));
        CHECK(f.me.find(1, ClientOrderId{2}) == nullptr);
        CHECK(f.me.find(1, ClientOrderId{4}) == nullptr);
        CHECK(f.rec.count(Event::Cancel) == 2);
        break;
    }
    CHECK(f.rec.monotonic);
    // level qty always equals the sum of resting leaves
    Qty sum{};
    f.me.for_each_open_order([&](const SimOrder& o) {
      if (o.side == Side::Sell) sum += o.leaves();
    });
    CHECK(f.me.top_of_book(InstrumentId{0}).ask.qty == sum);
  }
}

TEST_CASE("sim.matching: fills conserve quantity across accounts") {
  Fixture f;
  for (int i = 0; i < 50; ++i) {
    f.submit(0, i % 2 == 0 ? Side::Buy : Side::Sell, i % 2 == 0 ? "99" : "101", "2");
  }
  for (int i = 0; i < 20; ++i) {
    f.submit(1, i % 2 == 0 ? Side::Sell : Side::Buy, "0", "3", OrderType::Market);
  }
  const AccountLedger& a0 = f.me.ledger(0);
  const AccountLedger& a1 = f.me.ledger(1);
  CHECK(a0.bought == a1.sold);
  CHECK(a0.sold == a1.bought);
  CHECK(a0.buy_notional == a1.sell_notional);
  CHECK(a1.fills == f.rec.count(Event::Fill));
  CHECK(f.rec.filled_total() == a1.bought + a1.sold);
  Qty resting{};
  f.me.for_each_open_order([&](const SimOrder& o) { resting += o.leaves(); });
  CHECK(resting == qt("100") - f.rec.filled_total());
}
