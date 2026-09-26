// Xmm against a fake strategy context (pricing, basis, rounding, hedge sizing across contract
// multipliers, in-flight blocking, partial fills, rejects, duplicates, uncertain outcomes and the
// guards), then through the real engine and simulated venue (StrategyHarness) with two
// instruments on two venues.
#include "fastmm/strategies/xmm.hpp"

#include "test_support.hpp"

#include "fastmm/testing/strategy_harness.hpp"

#include <array>
#include <vector>

using namespace fastmm;

namespace {

constexpr std::int64_t kT0 = 1'789'344'931'096LL * 1'000'000;

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

struct FakeBook {
  Price bid{};
  Price ask{};
  Qty bid_qty = Qty::from_int(1);
  Qty ask_qty = Qty::from_int(1);
  bool valid = false;
  Timestamp updated{};
  [[nodiscard]] bool is_valid() const { return valid; }
  [[nodiscard]] Price mid() const { return Price::from_raw((bid.raw + ask.raw) / 2); }
  [[nodiscard]] Level best_bid() const { return Level{bid, bid_qty}; }
  [[nodiscard]] Level best_ask() const { return Level{ask, ask_qty}; }
  [[nodiscard]] Timestamp last_update() const { return updated; }
};
struct FakePosition {
  Qty qty{};
};

Instrument linear(const char* sym, std::uint8_t venue, const char* mult, const char* lot) {
  Instrument i{};
  i.symbol = sym;
  i.venue = VenueId{venue};
  i.asset_class = AssetClass::Perpetual;
  i.flags = Instrument::kEnabled;
  i.tick = px("0.1");
  i.lot = qt(lot);
  i.min_qty = i.lot;
  i.contract_multiplier = qt(mult);
  return i;
}

// Id 0: Binance USD-M BTCUSDT (quantity in BTC) on venue 0, the quote instrument. Id 1: the hedge
// on venue 1, by default an OKX-style swap of 0.01 BTC contracts in lots of 0.01 contracts.
struct Ctx {
  struct Sent {
    NewOrderRequest req;
    ClientOrderId id;
    bool open = true;
  };

  InstrumentTable table;
  std::array<FakeBook, 2> books{};
  std::array<Qty, 2> pos{};
  Timestamp t{kT0};
  std::vector<DesiredQuotes> sets;
  int pulls = 0;
  std::vector<Sent> sent;
  bool refuse = false;
  bool quoting = true;

  explicit Ctx(const char* hedge_mult = "0.01", const char* hedge_lot = "0.01") {
    REQUIRE(table.add(linear("BTCUSDT", 0, "1", "0.001")));
    REQUIRE(table.add(linear("BTC-USDT-SWAP", 1, hedge_mult, hedge_lot)));
    quote_book("99990", "100010");
    hedge_book("100000.0", "100000.2");
  }
  void quote_book(const char* b, const char* a) {
    books[0] = FakeBook{px(b), px(a), Qty::from_int(1), Qty::from_int(1), true, t};
  }
  void hedge_book(const char* b, const char* a) {
    books[1] = FakeBook{px(b), px(a), Qty::from_int(1), Qty::from_int(1), true, t};
  }

  [[nodiscard]] const InstrumentTable& instruments() const { return table; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const { return table.get(id); }
  [[nodiscard]] const FakeBook& book(InstrumentId id) const { return books[id.value]; }
  [[nodiscard]] FakePosition position(InstrumentId id) const { return FakePosition{pos[id.value]}; }
  [[nodiscard]] Timestamp now() const { return t; }
  bool set_quotes(InstrumentId id, const DesiredQuotes& q) {
    CHECK(id == InstrumentId{0});
    if (!quoting) return false;
    sets.push_back(q);
    return true;
  }
  void pull_quotes(InstrumentId id) {
    CHECK(id == InstrumentId{0});
    ++pulls;
  }
  TimerId every(Duration, std::uint64_t) { return TimerId{1}; }
  Result<ClientOrderId, RejectReason> send(const NewOrderRequest& r) {
    if (refuse) return fail(RejectReason::MaxPosition);
    const ClientOrderId id{0x0001'0000'0000ULL + sent.size() + 1};
    sent.push_back(Sent{r, id, true});
    return id;
  }
  [[nodiscard]] Qty open_qty(InstrumentId id, Side side) const {
    Qty q{};
    for (const Sent& s : sent) {
      if (s.open && s.req.instrument == id && s.req.side == side) q += s.req.qty;
    }
    return q;
  }
  [[nodiscard]] const DesiredQuotes& last() const {
    REQUIRE_FALSE(sets.empty());
    return sets.back();
  }
  void advance(Duration d) {
    t = t + d;
    books[0].updated = t;
    books[1].updated = t;
  }
};

Xmm make(const ParamMap& extra = {}) {
  ParamMap p{{"quote_qty", "0.01"},
             {"edge_bps", "2"},
             {"quote_fee_bps", "0"},
             {"hedge_fee_bps", "4"},
             {"slippage_bps", "1"},
             {"hedge_tolerance_bps", "5"},
             {"basis_halflife_s", "0"},
             {"max_unhedged", "0.05"},
             {"requote_threshold_ticks", "0"}};
  for (const auto& [k, v] : extra) p[k] = v;
  Xmm s;
  const auto err = s.configure(p);
  REQUIRE_MESSAGE(!err, err.value_or(""));
  return s;
}

void book(Xmm& s, Ctx& c, InstrumentId id) {
  s.on_book(c, id, c.books[id.value]);
}

// The maker quote on instrument 0 fills `qty` on `side`.
void maker_fill(Xmm& s, Ctx& c, Side side, const char* qty) {
  c.pos[0] += side == Side::Buy ? qt(qty) : -qt(qty);
  Fill f;
  f.instrument = InstrumentId{0};
  f.side = side;
  f.qty = qt(qty);
  s.on_fill(c, f);
}

// Hedge order `i` fills `contracts` (a fill message; the order stays open unless `done`).
void hedge_fill(Xmm& s, Ctx& c, std::size_t i, const char* contracts, bool done) {
  Ctx::Sent& o = c.sent.at(i);
  const Qty q = qt(contracts);
  c.pos[1] += o.req.side == Side::Buy ? q : -q;
  if (done) o.open = false;
  Fill f;
  f.instrument = InstrumentId{1};
  f.side = o.req.side;
  f.qty = q;
  f.order_done = done;
  s.on_fill(c, f);
}

// Hedge order `i` ends in `state` having filled `cum` in total.
void hedge_end(Xmm& s,
               Ctx& c,
               std::size_t i,
               OrderState state,
               const char* cum,
               bool acked = true,
               const char* unresolved = "0",
               RejectReason reason = RejectReason::None) {
  Ctx::Sent& o = c.sent.at(i);
  o.open = false;
  OmsUpdate u;
  u.known = true;
  u.changed = true;
  u.terminal = true;
  u.order.cl_ord_id = o.id;
  u.order.instrument = o.req.instrument;
  u.order.side = o.req.side;
  u.order.qty = o.req.qty;
  u.order.cum_qty = qt(cum);
  u.order.state = state;
  u.order.reject_reason = reason;
  if (acked) u.order.venue_order_id.assign("123");
  u.unresolved_qty = qt(unresolved);
  s.on_order_update(c, u);
}

ConnectionStateMsg connection(std::uint8_t venue, std::uint8_t channel, ConnState st) {
  ConnectionStateMsg m{};
  init_header(m, EventType::ConnectionState, InstrumentId{}, VenueId{venue});
  m.state = st;
  m.channel = channel;
  return m;
}

// Starts the strategy and delivers both books.
void start(Xmm& s, Ctx& c) {
  s.on_start(c);
  REQUIRE(s.ready());
  book(s, c, InstrumentId{0});
  book(s, c, InstrumentId{1});
}

}  // namespace

TEST_CASE("strategies.xmm: parameters are validated") {
  Xmm s;
  CHECK(s.configure({{"quote_instrument", "1"}, {"hedge_instrument", "1"}}).has_value());
  CHECK(s.configure({{"quote_qty", "0.01"}, {"max_unhedged", "0.005"}}).has_value());
  CHECK_FALSE(s.configure({{"quote_qty", "0.01"}, {"max_unhedged", "0"}}).has_value());
  CHECK_FALSE(s.configure({{"quote_fee_bps", "-0.5"}}).has_value());
}

TEST_CASE("strategies.xmm: quotes around the hedge mid, rounded away from it") {
  Xmm s = make();
  Ctx c;
  start(s, c);
  // fair = 100000.1; half = fair * 7 bps = 70.00007.
  const DesiredQuotes& q = c.last();
  REQUIRE(q.bids.size() == 1);
  REQUIRE(q.asks.size() == 1);
  CHECK(q.bids[0].price == px("99930.0"));   // 99930.09993 rounded down
  CHECK(q.asks[0].price == px("100070.2"));  // 100070.17007 rounded up
  CHECK(q.bids[0].qty == qt("0.01"));
  CHECK(s.fair_value(c) == px("100000.1"));

  // A quote fee is priced in as well; a rebate narrows the quotes.
  Xmm r = make({{"quote_fee_bps", "-1"}});
  Ctx c2;
  start(r, c2);
  CHECK(c2.last().bids[0].price == px("99940.0"));  // half 60.00006
  CHECK(c2.last().asks[0].price == px("100060.2"));

  // The microprice leans towards the thin side.
  Xmm m = make({{"use_microprice", "true"}});
  Ctx c3;
  c3.books[1].bid_qty = qt("3");  // heavy bid: microprice near the ask
  start(m, c3);
  CHECK(m.fair_value(c3) == px("100000.15"));
}

TEST_CASE("strategies.xmm: the basis is an EWMA of quote mid minus hedge mid") {
  Xmm s = make({{"basis_halflife_s", "10"}});
  Ctx c;
  c.quote_book("100040", "100060.2");  // mid 100050.1: basis 50
  s.on_start(c);
  CHECK(c.sets.empty());
  book(s, c, InstrumentId{0});
  REQUIRE(s.have_basis());
  CHECK(s.basis() == px("50"));
  CHECK(s.fair_value(c) == px("100050.1"));
  // One half-life later the sample is 150: halfway.
  c.advance(seconds(10));
  c.quote_book("100140", "100160.2");
  book(s, c, InstrumentId{0});
  CHECK(s.basis() == px("100"));
  CHECK(s.fair_value(c) == px("100100.1"));
  // Quotes never cross the quote venue's touch (post-only).
  c.quote_book("100300", "100300.2");
  book(s, c, InstrumentId{0});
  REQUIRE(c.last().bids.size() == 1);
  CHECK(c.last().bids[0].price < px("100300.2"));
  CHECK(c.last().asks[0].price > px("100300"));
}

TEST_CASE("strategies.xmm: hedge size and price across contract multipliers") {
  const Ratio tol = Ratio::from_raw(5 * kRatioPerBp);
  const Price bid = px("100000.0");
  const Price ask = px("100000.2");
  SUBCASE("Binance USD-M: quantity in BTC, lot 0.001") {
    const Instrument h = linear("BTCUSDT", 1, "1", "0.001");
    const auto o = Xmm::hedge_order(h, qt("0.0123"), bid, ask, tol);
    REQUIRE(o);
    CHECK(o->side == Side::Sell);
    CHECK(o->qty == qt("0.012"));
    CHECK(o->tif == TimeInForce::Ioc);
    CHECK_FALSE(o->post_only);
    CHECK(o->price == px("99950.0"));  // 99950.0 exactly (bid - 5 bps), rounded up
    CHECK(o->user_tag == Xmm::kHedgeTag);
    const auto b = Xmm::hedge_order(h, qt("-0.0123"), bid, ask, tol);
    REQUIRE(b);
    CHECK(b->side == Side::Buy);
    CHECK(b->qty == qt("0.012"));
    CHECK(b->price == px("100050.2"));  // 100050.2001 rounded down
  }
  SUBCASE("OKX swap: 0.01 BTC per contract, lot 0.01 contracts") {
    const Instrument h = linear("BTC-USDT-SWAP", 1, "0.01", "0.01");
    const auto o = Xmm::hedge_order(h, qt("0.0123"), bid, ask, tol);
    REQUIRE(o);
    CHECK(o->qty == qt("1.23"));
    CHECK(Xmm::to_base(h, o->qty) == qt("0.0123"));
  }
  SUBCASE("whole contracts: 0.01 BTC each, lot 1") {
    const Instrument h = linear("BTC-USDT-SWAP", 1, "0.01", "1");
    const auto o = Xmm::hedge_order(h, qt("0.0123"), bid, ask, tol);
    REQUIRE(o);
    CHECK(o->qty == qt("1"));
    CHECK_FALSE(Xmm::hedge_order(h, qt("0.0099"), bid, ask, tol));  // under one contract
  }
  SUBCASE("Bybit: min_qty above the lot, max_qty caps") {
    Instrument h = linear("BTCUSDT", 1, "1", "0.001");
    h.min_qty = qt("0.005");
    h.max_qty = qt("0.01");
    CHECK_FALSE(Xmm::hedge_order(h, qt("0.004"), bid, ask, tol));
    const auto o = Xmm::hedge_order(h, qt("0.05"), bid, ask, tol);
    REQUIRE(o);
    CHECK(o->qty == qt("0.01"));
  }
  SUBCASE("no hedge without the touch it needs or with nothing to hedge") {
    const Instrument h = linear("BTCUSDT", 1, "1", "0.001");
    CHECK_FALSE(Xmm::hedge_order(h, qt("0.01"), Price{}, ask, tol));
    CHECK_FALSE(Xmm::hedge_order(h, qt("-0.01"), bid, Price{}, tol));
    CHECK_FALSE(Xmm::hedge_order(h, Qty{}, bid, ask, tol));
  }
}

TEST_CASE("strategies.xmm: a maker fill sends one hedge, which blocks the next until it ends") {
  Xmm s = make();
  Ctx c;
  start(s, c);
  maker_fill(s, c, Side::Buy, "0.01");
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == InstrumentId{1});
  CHECK(c.sent[0].req.side == Side::Sell);
  CHECK(c.sent[0].req.qty == qt("1"));  // 0.01 BTC in 0.01 BTC contracts
  CHECK(c.sent[0].req.tif == TimeInForce::Ioc);
  CHECK(c.sent[0].req.price == px("99950.0"));
  CHECK(s.unhedged(c) == qt("0.01"));

  // The same fill replayed, a timer, a hedge book update: the hedge is still open.
  maker_fill(s, c, Side::Buy, "0");
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  book(s, c, InstrumentId{1});
  CHECK(c.sent.size() == 1);

  // It fills and ends; the positions are flat and nothing more goes out.
  hedge_fill(s, c, 0, "1", true);
  hedge_end(s, c, 0, OrderState::Filled, "1");
  CHECK(s.unhedged(c).is_zero());
  CHECK(c.sent.size() == 1);
  // A duplicate of the maker fill (the engine drops it by exec id, but the hook may still run):
  // the positions say there is nothing to do.
  Fill dup;
  dup.instrument = InstrumentId{0};
  dup.side = Side::Buy;
  dup.qty = qt("0.01");
  s.on_fill(c, dup);
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  CHECK(c.sent.size() == 1);
  CHECK(s.stats().hedges_sent == 1);
  CHECK(s.stats().hedge_failures == 0);
}

TEST_CASE("strategies.xmm: a partial hedge is followed by one for the remainder") {
  Xmm s = make();
  Ctx c("1", "0.001");  // Bybit-style hedge: quantity in BTC
  start(s, c);
  maker_fill(s, c, Side::Sell, "0.01");
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.side == Side::Buy);
  CHECK(c.sent[0].req.qty == qt("0.01"));
  hedge_fill(s, c, 0, "0.004", false);
  CHECK(c.sent.size() == 1);  // still open
  hedge_end(s, c, 0, OrderState::Expired, "0.004");
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.side == Side::Buy);
  CHECK(c.sent[1].req.qty == qt("0.006"));
  CHECK(s.stats().hedge_failures == 0);  // it filled something
  hedge_fill(s, c, 1, "0.006", true);
  hedge_end(s, c, 1, OrderState::Filled, "0.006");
  CHECK(s.unhedged(c).is_zero());
  CHECK(c.sent.size() == 2);
}

TEST_CASE("strategies.xmm: a rejected hedge is retried after the backoff, sized from positions") {
  Xmm s = make({{"hedge_retry_ms", "200"}});
  Ctx c("1", "0.001");
  start(s, c);
  maker_fill(s, c, Side::Buy, "0.01");
  REQUIRE(c.sent.size() == 1);
  hedge_end(s, c, 0, OrderState::Rejected, "0");  // a definite reject (reason None here)
  CHECK(s.stats().hedge_failures == 1);
  CHECK(s.stats().uncertain_ends == 0);
  CHECK(c.sent.size() == 1);  // backing off
  // Meanwhile the other quote fills too.
  c.advance(milliseconds(100));
  maker_fill(s, c, Side::Buy, "0.01");
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  CHECK(c.sent.size() == 1);
  c.advance(milliseconds(100));
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.qty == qt("0.02"));
  // An IOC that expires unfilled is a failure too.
  hedge_end(s, c, 1, OrderState::Expired, "0");
  CHECK(s.stats().hedge_failures == 2);
  // The engine refusing the order (a risk limit) counts the same way.
  c.refuse = true;
  c.advance(milliseconds(200));
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  CHECK(s.stats().hedge_failures == 3);
  CHECK(c.sent.size() == 2);
}

TEST_CASE("strategies.xmm: repeated failures halt quoting and hedging until restart changes") {
  Xmm s = make({{"hedge_retry_ms", "0"}, {"max_hedge_failures", "3"}});
  Ctx c("1", "0.001");
  start(s, c);
  maker_fill(s, c, Side::Buy, "0.01");
  for (std::size_t i = 0; i < 3; ++i) {
    REQUIRE(c.sent.size() == i + 1);
    c.advance(milliseconds(10));
    hedge_end(s, c, i, OrderState::Expired, "0");
  }
  CHECK(s.halted());
  CHECK(s.stats().halts == 1);
  CHECK(c.sent.size() == 3);  // no loop
  const int pulls = c.pulls;
  CHECK(pulls >= 1);
  const std::size_t sets = c.sets.size();
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  book(s, c, InstrumentId{1});
  CHECK(c.sent.size() == 3);
  CHECK(c.sets.size() == sets);
  // Quoting paused and resumed (a reconciliation does that) does not clear it.
  s.on_quoting(c, false);
  s.on_quoting(c, true);
  CHECK(s.halted());
  CHECK(c.sent.size() == 3);
  // Neither do repeated, unchanged parameters (a publisher refreshing them).
  s.on_params(c);
  CHECK(s.halted());
  // A new value of restart does: it starts again from the positions.
  REQUIRE_FALSE(s.configure({{"restart", "1"}}));
  s.on_params(c);
  CHECK_FALSE(s.halted());
  CHECK(c.sent.size() == 4);
  CHECK(c.sets.size() > sets);
}

TEST_CASE("strategies.xmm: failures spread over more than the window do not halt") {
  Xmm s =
      make({{"hedge_retry_ms", "0"}, {"max_hedge_failures", "2"}, {"failure_window_ms", "1000"}});
  Ctx c("1", "0.001");
  start(s, c);
  maker_fill(s, c, Side::Buy, "0.01");
  hedge_end(s, c, 0, OrderState::Expired, "0");
  c.advance(milliseconds(1500));
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  REQUIRE(c.sent.size() == 2);
  hedge_end(s, c, 1, OrderState::Expired, "0");
  CHECK_FALSE(s.halted());
  CHECK(c.sent.size() == 3);
}

TEST_CASE("strategies.xmm: an outcome the venue never reported holds hedging") {
  Xmm s = make({{"uncertain_hold_ms", "5000"}});
  Ctx c("1", "0.001");
  start(s, c);
  maker_fill(s, c, Side::Buy, "0.01");
  REQUIRE(c.sent.size() == 1);
  // The ack timeout cancelled it and the venue said it does not know the order.
  hedge_end(s, c, 0, OrderState::Canceled, "0", /*acked=*/false);
  CHECK(s.stats().uncertain_ends == 1);
  CHECK(s.stats().hedge_failures == 1);
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  CHECK(c.sent.size() == 1);
  // The fill was real: it arrives late (a replayed execution) and nothing is left to hedge.
  c.advance(milliseconds(1000));
  c.pos[1] -= qt("0.01");
  Fill late;
  late.instrument = InstrumentId{1};
  late.side = Side::Sell;
  late.qty = qt("0.01");
  late.late = true;
  s.on_fill(c, late);
  c.advance(milliseconds(5000));
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  CHECK(c.sent.size() == 1);
  CHECK(s.unhedged(c).is_zero());

  // Reconciliation dropping an order with quantity unaccounted for holds as well; once the hold
  // is over the positions decide.
  maker_fill(s, c, Side::Buy, "0.01");
  REQUIRE(c.sent.size() == 2);
  hedge_end(s, c, 1, OrderState::Canceled, "0", true, "0.01");
  CHECK(s.stats().uncertain_ends == 2);
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  CHECK(c.sent.size() == 2);
  c.advance(milliseconds(5001));
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  REQUIRE(c.sent.size() == 3);
  CHECK(c.sent[2].req.qty == qt("0.01"));
}

TEST_CASE("strategies.xmm: a generic venue reject may hide a fill and holds hedging") {
  Xmm s = make({{"uncertain_hold_ms", "3000"}, {"hedge_retry_ms", "200"}});
  Ctx c("1", "0.001");
  start(s, c);
  maker_fill(s, c, Side::Buy, "0.01");
  // A REST timeout: the connector reports VenueReject and asks for a reconciliation.
  hedge_end(s, c, 0, OrderState::Rejected, "0", false, "0", RejectReason::VenueReject);
  CHECK(s.stats().uncertain_ends == 1);
  c.advance(milliseconds(1000));
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  CHECK(c.sent.size() == 1);
  c.advance(milliseconds(2000));
  s.on_timer(c, TimerId{1}, Xmm::kTimer);
  CHECK(c.sent.size() == 2);
}

TEST_CASE("strategies.xmm: max_unhedged drops the side that would grow the gap") {
  Xmm s = make({{"max_unhedged", "0.015"}});
  Ctx c("1", "0.001");
  start(s, c);
  CHECK(c.last().bids.size() == 1);
  CHECK(c.last().asks.size() == 1);
  maker_fill(s, c, Side::Buy, "0.01");  // unhedged +0.01: another buy would make it 0.02
  CHECK(c.last().bids.empty());
  CHECK(c.last().asks.size() == 1);
  hedge_fill(s, c, 0, "0.01", true);
  hedge_end(s, c, 0, OrderState::Filled, "0.01");
  CHECK(c.last().bids.size() == 1);
  CHECK(c.last().asks.size() == 1);
}

TEST_CASE("strategies.xmm: a bad or stale hedge book and a hedge venue down pull the quotes") {
  Xmm s = make({{"stale_ms", "1000"}});
  Ctx c("1", "0.001");
  start(s, c);
  REQUIRE(c.sets.size() >= 1);
  SUBCASE("hedge book invalid") {
    c.books[1].valid = false;
    book(s, c, InstrumentId{1});
    CHECK(c.pulls == 1);
    maker_fill(s, c, Side::Buy, "0.01");
    CHECK(c.sent.empty());  // no book to price the hedge from
    c.books[1].valid = true;
    book(s, c, InstrumentId{1});
    CHECK(c.sent.size() == 1);
  }
  SUBCASE("hedge book stale") {
    c.t = c.t + milliseconds(1500);  // books not updated
    s.on_timer(c, TimerId{1}, Xmm::kTimer);
    CHECK(c.pulls == 1);
    const std::size_t sets = c.sets.size();
    c.books[0].updated = c.t;
    book(s, c, InstrumentId{0});  // the quote book is fresh, the hedge book is not
    CHECK(c.sets.size() == sets);
    c.books[1].updated = c.t;
    book(s, c, InstrumentId{1});
    CHECK(c.sets.size() == sets + 1);
  }
  SUBCASE("hedge venue order channel down") {
    s.on_connection(c, connection(1, 1, ConnState::Disconnected));
    CHECK(c.pulls == 1);
    maker_fill(s, c, Side::Buy, "0.01");
    CHECK(c.sent.empty());
    const std::size_t sets = c.sets.size();
    book(s, c, InstrumentId{0});
    CHECK(c.sets.size() == sets);
    s.on_connection(c, connection(1, 1, ConnState::Live));
    CHECK(c.sent.size() == 0);  // the hedge goes out on the next event
    s.on_timer(c, TimerId{1}, Xmm::kTimer);
    CHECK(c.sent.size() == 1);
    CHECK(c.sets.size() > sets);
  }
  SUBCASE("a drop of the quote venue does not stop hedging") {
    s.on_connection(c, connection(0, 1, ConnState::Disconnected));
    maker_fill(s, c, Side::Buy, "0.01");
    CHECK(c.sent.size() == 1);
  }
}

TEST_CASE("strategies.xmm: quoting disabled, then enabled again") {
  Xmm s = make();
  Ctx c;
  start(s, c);
  c.quoting = false;
  s.on_quoting(c, false);
  const std::size_t sets = c.sets.size();
  book(s, c, InstrumentId{1});
  CHECK(c.sets.size() == sets);
  c.quoting = true;
  s.on_quoting(c, true);
  CHECK(c.sets.size() == sets + 1);
}

TEST_CASE("strategies.xmm: an inverse or missing instrument leaves it idle") {
  Xmm s = make({{"hedge_instrument", "2"}});
  Ctx c;
  s.on_start(c);
  CHECK_FALSE(s.ready());
  Ctx inv;
  inv.table.clear();
  REQUIRE(inv.table.add(linear("BTCUSDT", 0, "1", "0.001")));
  Instrument perp = linear("BTC-PERPETUAL", 1, "10", "1");
  perp.flags = Instrument::kEnabled | Instrument::kInverse;
  REQUIRE(inv.table.add(perp));
  Xmm t = make();
  t.on_start(inv);
  CHECK_FALSE(t.ready());
  book(t, inv, InstrumentId{0});
  maker_fill(t, inv, Side::Buy, "0.01");
  CHECK(inv.sets.empty());
  CHECK(inv.sent.empty());
}

// ---- through the real engine
// ---------------------------------------------------------------------

namespace {

using fastmm::sim::HarnessOptions;
using fastmm::sim::StrategyHarness;

// BTCUSDT on venue 0 (quotes) and BTC-USDT-SWAP on venue 1 (0.01 BTC contracts, hedges).
HarnessOptions two_venues() {
  HarnessOptions o;
  o.instruments.clear();
  REQUIRE(o.instruments.add(linear("BTCUSDT", 0, "1", "0.001")));
  REQUIRE(o.instruments.add(linear("BTC-USDT-SWAP", 1, "0.01", "1")));
  return o;
}

// Resting liquidity at the simulated venue on the hedge instrument (the harness's market data
// does not reach the venue's matching engine).
void rest(StrategyHarness<Xmm>& h, Side side, const char* price, const char* qty) {
  static std::uint64_t seq = 1000;
  sim::NewOrder o;
  o.account = 9;
  o.cl_ord_id = ClientOrderId{++seq};
  o.instrument = InstrumentId{1};
  o.side = side;
  o.price = px(price);
  o.qty = qt(qty);
  static_cast<void>(h.venue_transport().matching_engine().submit(o, h.now()));
}

std::size_t hedge_orders(StrategyHarness<Xmm>& h) {
  return h.strategy().stats().hedges_sent;
}

}  // namespace

TEST_CASE("strategies.xmm: engine: a maker fill leads to exactly one hedge") {
  const ParamMap p{{"quote_qty", "0.02"}, {"basis_halflife_s", "0"}, {"max_unhedged", "0.1"}};
  StrategyHarness<Xmm> h(p, two_venues());
  const InstrumentId q{0};
  const InstrumentId hid{1};
  h.book(px("99990"), px("100010"), Qty::from_int(1), q);
  h.book(px("100000.0"), px("100000.2"), Qty::from_int(1), hid);
  h.advance(milliseconds(1));
  REQUIRE(h.working_orders(q).size() == 2);
  rest(h, Side::Buy, "100000.0", "5");  // 5 contracts = 0.05 BTC

  REQUIRE(h.fill(Side::Buy, Qty{}, q));  // our bid: +0.02 BTC
  CHECK(h.engine().position(q).qty == qt("0.02"));
  h.advance(milliseconds(5));
  CHECK(hedge_orders(h) == 1);
  CHECK(h.engine().position(hid).qty == qt("-2"));  // 2 contracts of 0.01 BTC
  h.advance(milliseconds(500));
  CHECK(hedge_orders(h) == 1);
  CHECK(h.strategy().unhedged(h.engine().context()).is_zero());
}

TEST_CASE("strategies.xmm: engine: a partial hedge, a miss and a retry converge on flat") {
  const ParamMap p{{"quote_qty", "0.05"},
                   {"basis_halflife_s", "0"},
                   {"max_unhedged", "0.2"},
                   {"hedge_retry_ms", "200"},
                   {"hedge_tolerance_bps", "1"}};
  StrategyHarness<Xmm> h(p, two_venues());
  const InstrumentId q{0};
  const InstrumentId hid{1};
  h.book(px("99990"), px("100010"), Qty::from_int(1), q);
  h.book(px("100000.0"), px("100000.2"), Qty::from_int(1), hid);
  h.advance(milliseconds(1));
  REQUIRE(h.working_orders(q).size() == 2);
  rest(h, Side::Buy, "100000.0", "2");  // 2 of the 5 contracts needed
  rest(h, Side::Buy, "99000.0", "10");  // outside the 1 bp tolerance

  REQUIRE(h.fill(Side::Buy, Qty{}, q));  // +0.05 BTC
  h.advance(milliseconds(5));
  CHECK(h.engine().position(hid).qty == qt("-2"));
  // The remainder went straight out, found nothing within tolerance and expired: a failure.
  CHECK(hedge_orders(h) == 2);
  CHECK(h.strategy().stats().hedge_failures == 1);
  h.advance(milliseconds(100));
  CHECK(hedge_orders(h) == 2);  // backing off
  rest(h, Side::Buy, "100000.0", "10");
  h.advance(milliseconds(300));
  CHECK(hedge_orders(h) == 3);
  CHECK(h.engine().position(hid).qty == qt("-5"));
  CHECK(h.strategy().unhedged(h.engine().context()).is_zero());
  h.advance(milliseconds(500));
  CHECK(hedge_orders(h) == 3);
}

TEST_CASE("strategies.xmm: engine: the hedge venue dropping pulls the quotes") {
  const ParamMap p{{"quote_qty", "0.02"}, {"basis_halflife_s", "0"}, {"max_unhedged", "0.1"}};
  StrategyHarness<Xmm> h(p, two_venues());
  const InstrumentId q{0};
  const InstrumentId hid{1};
  h.book(px("99990"), px("100010"), Qty::from_int(1), q);
  h.book(px("100000.0"), px("100000.2"), Qty::from_int(1), hid);
  h.advance(milliseconds(1));
  REQUIRE(h.working_orders(q).size() == 2);
  h.disconnect(0, VenueId{1});  // the hedge venue's market data
  h.advance(milliseconds(1));
  CHECK(h.working_orders(q).empty());
  h.reconnect(0, VenueId{1});
  h.book(px("100000.0"), px("100000.2"), Qty::from_int(1), hid);
  h.advance(milliseconds(1));
  CHECK(h.working_orders(q).size() == 2);
}
