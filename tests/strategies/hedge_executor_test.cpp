// HedgeExecutor against a fake context with one source and two hedge instruments on three venues
// (sizing, splitting, minimums, the in-flight rule, the uncertain hold, failover on each trigger,
// de-risking and recovery), then xmm with a fallback through the real engine: quote on A, hedge on
// B, fail over to C while B's kill switch is on, and back.
#include "fastmm/strategies/hedge_executor.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/xmm.hpp"
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
Ratio bps(std::int64_t n) {
  return Ratio::from_raw(n * kRatioPerBp);
}

struct FakeBook {
  Price bid{};
  Price ask{};
  bool valid = false;
  Timestamp updated{};
  [[nodiscard]] bool is_valid() const { return valid; }
  [[nodiscard]] Price mid() const { return Price::from_raw((bid.raw + ask.raw) / 2); }
  [[nodiscard]] Level best_bid() const { return Level{bid, Qty::from_int(1)}; }
  [[nodiscard]] Level best_ask() const { return Level{ask, Qty::from_int(1)}; }
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

constexpr InstrumentId kSrc{0};
constexpr InstrumentId kB{1};
constexpr InstrumentId kC{2};

// Id 0: the source on venue 0 (quantity in BTC). Id 1: hedge B on venue 1, 0.01 BTC contracts in
// lots of 0.01. Id 2: hedge C on venue 2, quantity in BTC, lot 0.001.
struct Ctx {
  struct Sent {
    NewOrderRequest req;
    ClientOrderId id;
    bool open = true;
  };

  InstrumentTable table;
  std::array<FakeBook, 3> books{};
  std::array<Qty, 3> pos{};
  Timestamp t{kT0};
  std::vector<Sent> sent;
  std::array<bool, 3> refuse{};  // per instrument
  std::array<bool, 3> killed{};  // per venue
  std::array<bool, 3> gated{};   // per venue
  bool reconciling_now = false;
  std::array<std::array<Qty, 2>, 3> room{};
  std::array<RiskHeadroom, 3> risk{};  // per instrument: what [risk] admits (unlimited)
  std::vector<NewOrderRequest> risk_refused;

  Ctx() {
    REQUIRE(table.add(linear("BTCUSDT", 0, "1", "0.001")));
    REQUIRE(table.add(linear("BTC-USDT-SWAP", 1, "0.01", "0.01")));
    REQUIRE(table.add(linear("BTCUSDT", 2, "1", "0.001")));
    for (auto& r : room) r = {Qty::max(), Qty::max()};
    for (std::uint32_t i = 0; i < 3; ++i) set_book(InstrumentId{i}, "100000.0", "100000.2");
  }
  void set_book(InstrumentId id, const char* b, const char* a) {
    books[id.value] = FakeBook{px(b), px(a), true, t};
  }
  void advance(Duration d) {
    t = t + d;
    for (FakeBook& b : books) b.updated = t;
  }

  [[nodiscard]] const InstrumentTable& instruments() const { return table; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const { return table.get(id); }
  [[nodiscard]] const FakeBook& book(InstrumentId id) const { return books[id.value]; }
  [[nodiscard]] FakePosition position(InstrumentId id) const { return FakePosition{pos[id.value]}; }
  [[nodiscard]] Timestamp now() const { return t; }
  [[nodiscard]] bool reconciling() const { return reconciling_now; }
  [[nodiscard]] bool venue_killed(VenueId v) const { return killed[v.value]; }
  [[nodiscard]] VenueHealthView venue_health(VenueId v) const {
    VenueHealthView h;
    h.gated = gated[v.value];
    return h;
  }
  [[nodiscard]] RiskHeadroom risk_headroom(InstrumentId id) const { return risk[id.value]; }
  Result<ClientOrderId, RejectReason> send(const NewOrderRequest& r) {
    if (refuse[r.instrument.value]) return fail(RejectReason::MaxPosition);
    // The engine's risk check: an order over what the limits admit is refused whole.
    const RiskHeadroom& h = risk[r.instrument.value];
    const Instrument& inst = table.get(r.instrument);
    if (r.qty > HedgeExecutor::risk_room(h, inst, r.side, r.price)) {
      risk_refused.push_back(r);
      const Notional n = inst.notional(r.price, r.qty);
      return fail(n > h.max_order_notional ? RejectReason::MaxOrderNotional
                  : n > (r.side == Side::Buy ? h.exposure_buy_notional : h.exposure_sell_notional)
                      ? RejectReason::MaxGrossNotional
                      : RejectReason::MaxPosition);
    }
    const ClientOrderId id{0x0001'0000'0000ULL + sent.size() + 1};
    sent.push_back(Sent{r, id, true});
    return id;
  }
  [[nodiscard]] Qty balance_room(InstrumentId id, Side side, Price) const {
    return room[id.value][static_cast<std::size_t>(side)];
  }
  [[nodiscard]] Qty open_qty(InstrumentId id, Side side) const {
    Qty q{};
    for (const Sent& s : sent) {
      if (s.open && s.req.instrument == id && s.req.side == side) q += s.req.qty;
    }
    return q;
  }
};

HedgeExecutor::Config config() {
  HedgeExecutor::Config c;
  c.name = "test";
  c.tag = 0x4845'4447;
  c.derisk_tag = 0x4445'5253;
  c.retry = milliseconds(100);
  c.max_failures = 3;
  c.failure_window = milliseconds(10000);
  c.uncertain_hold = milliseconds(1000);
  c.stale = milliseconds(2000);
  c.bench = milliseconds(5000);
  return c;
}

// Source 0; hedges B then C (C only when `fallback`), each 5 bps.
HedgeExecutor make(Ctx& c, bool fallback = true, HedgeExecutor::Config cfg = config()) {
  HedgeExecutor h;
  h.reset();
  REQUIRE(h.add_source(kSrc));
  REQUIRE(h.add_hedge(kB, bps(5)));
  if (fallback) REQUIRE(h.add_hedge(kC, bps(5)));
  REQUIRE(h.start(c, cfg));
  return h;
}

// The source fills: its position moves by `qty` (signed), then the executor hears of it.
void source_fill(HedgeExecutor& h, Ctx& c, const char* qty) {
  c.pos[0] += qt(qty);
  Fill f;
  f.instrument = kSrc;
  f.qty = qt(qty).abs();
  static_cast<void>(h.on_fill(c, f));
}

// Order `i` ends in `state` having filled `cum` (booked into the position here).
void end(HedgeExecutor& h,
         Ctx& c,
         std::size_t i,
         OrderState state,
         const char* cum,
         bool acked = true,
         const char* unresolved = "0",
         RejectReason reason = RejectReason::None) {
  Ctx::Sent& o = c.sent.at(i);
  o.open = false;
  const Qty q = qt(cum);
  c.pos[o.req.instrument.value] += o.req.side == Side::Buy ? q : -q;
  OmsUpdate u;
  u.known = true;
  u.changed = true;
  u.terminal = true;
  u.order.cl_ord_id = o.id;
  u.order.instrument = o.req.instrument;
  u.order.side = o.req.side;
  u.order.qty = o.req.qty;
  u.order.cum_qty = q;
  u.order.state = state;
  u.order.reject_reason = reason;
  if (acked) u.order.venue_order_id.assign("1");
  u.unresolved_qty = qt(unresolved);
  CHECK(h.on_order_update(c, u));
}

void filled(HedgeExecutor& h, Ctx& c, std::size_t i) {
  const Qty q = c.sent.at(i).req.qty;
  Ctx::Sent& o = c.sent.at(i);
  o.open = false;
  c.pos[o.req.instrument.value] += o.req.side == Side::Buy ? q : -q;
  OmsUpdate u;
  u.known = true;
  u.changed = true;
  u.terminal = true;
  u.order.cl_ord_id = o.id;
  u.order.instrument = o.req.instrument;
  u.order.side = o.req.side;
  u.order.qty = q;
  u.order.cum_qty = q;
  u.order.state = OrderState::Filled;
  u.order.venue_order_id.assign("1");
  CHECK(h.on_order_update(c, u));
}

ConnectionStateMsg connection(std::uint8_t venue, std::uint8_t channel, ConnState st) {
  ConnectionStateMsg m{};
  init_header(m, EventType::ConnectionState, InstrumentId{}, VenueId{venue});
  m.state = st;
  m.channel = channel;
  return m;
}

}  // namespace

TEST_CASE("strategies.hedge_executor: start refuses a bad set of legs") {
  Ctx c;
  HedgeExecutor none;
  none.reset();
  REQUIRE(none.add_source(kSrc));
  CHECK_FALSE(none.start(c, config()));  // no hedge
  HedgeExecutor twice;
  twice.reset();
  REQUIRE(twice.add_source(kSrc));
  REQUIRE(twice.add_hedge(kSrc, bps(5)));
  CHECK_FALSE(twice.start(c, config()));
  HedgeExecutor missing;
  missing.reset();
  REQUIRE(missing.add_source(kSrc));
  REQUIRE(missing.add_hedge(InstrumentId{7}, bps(5)));
  CHECK_FALSE(missing.start(c, config()));
  Ctx inv;
  inv.table.get(kC).flags |= Instrument::kInverse;
  HedgeExecutor inverse;
  inverse.reset();
  REQUIRE(inverse.add_source(kSrc));
  REQUIRE(inverse.add_hedge(kC, bps(5)));
  CHECK_FALSE(inverse.start(inv, config()));
  source_fill(inverse, inv, "0.01");
  CHECK(inv.sent.empty());
}

TEST_CASE("strategies.hedge_executor: the residual in base units across multipliers and a target") {
  Ctx c;
  HedgeExecutor h = make(c);
  c.pos[0] = qt("0.0123");
  c.pos[1] = qt("-0.5");  // 0.005 BTC
  c.pos[2] = qt("-0.002");
  CHECK(h.residual(c) == qt("0.0053"));
  h.set_target(qt("0.003"));
  CHECK(h.residual(c) == qt("0.0023"));
  h.set_target(Qty{});
  c.pos[1] = Qty{};
  c.pos[2] = Qty{};
  // 0.0123 BTC on B is 1.23 contracts of 0.01, priced 5 bps through the bid.
  h.update(c);
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == kB);
  CHECK(c.sent[0].req.side == Side::Sell);
  CHECK(c.sent[0].req.qty == qt("1.23"));
  CHECK(c.sent[0].req.tif == TimeInForce::Ioc);
  CHECK(c.sent[0].req.price == px("99950.0"));
  CHECK(c.sent[0].req.user_tag == config().tag);
  filled(h, c, 0);
  CHECK(h.residual(c).is_zero());
  CHECK(h.state() == HedgeExecutor::State::Flat);
  CHECK(c.sent.size() == 1);
}

TEST_CASE("strategies.hedge_executor: a residual over max_qty goes out in pieces, one at a time") {
  Ctx c;
  c.table.get(kB).max_qty = qt("0.5");  // 0.005 BTC
  HedgeExecutor h = make(c, false);
  source_fill(h, c, "-0.012");
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.side == Side::Buy);
  CHECK(c.sent[0].req.qty == qt("0.5"));
  CHECK(c.sent[0].req.price == px("100050.2"));
  // Open: nothing more, whatever happens.
  h.on_timer(c);
  h.on_book(c, kB);
  CHECK(c.sent.size() == 1);
  CHECK(h.state() == HedgeExecutor::State::InFlight);
  filled(h, c, 0);
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.qty == qt("0.5"));
  filled(h, c, 1);
  REQUIRE(c.sent.size() == 3);
  CHECK(c.sent[2].req.qty == qt("0.2"));
  filled(h, c, 2);
  CHECK(h.residual(c).is_zero());
  CHECK(h.stats().hedges_sent == 3);
}

// The live case: [risk] max_order_notional = 60, and 0.001 BTC to hedge on 0.01 BTC contracts is
// 0.1 contracts, 99.95 USDT at the order's price. Whole, the engine refuses it every time.
TEST_CASE("strategies.hedge_executor: a hedge over max_order_notional goes out in pieces") {
  Ctx c;
  c.risk[1].max_order_notional = Notional::from_int(60);
  HedgeExecutor h = make(c, false);
  source_fill(h, c, "0.001");
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == kB);
  CHECK(c.sent[0].req.side == Side::Sell);
  CHECK(c.sent[0].req.price == px("99950.0"));
  CHECK(c.sent[0].req.qty == qt("0.06"));  // 59.97 USDT; 0.07 would be 69.965
  CHECK(h.state() == HedgeExecutor::State::InFlight);
  h.on_timer(c);
  CHECK(c.sent.size() == 1);  // one at a time
  filled(h, c, 0);
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.qty == qt("0.04"));
  filled(h, c, 1);
  CHECK(h.residual(c).is_zero());
  CHECK(h.state() == HedgeExecutor::State::Flat);
  CHECK(c.risk_refused.empty());
  CHECK(h.stats().hedges_sent == 2);
  CHECK(h.stats().hedge_failures == 0);
  // A buy is priced through the ask: 100050.2 * 0.01 a contract, so 0.05 contracts (50.03 USDT).
  source_fill(h, c, "-0.001");
  REQUIRE(c.sent.size() == 3);
  CHECK(c.sent[2].req.side == Side::Buy);
  CHECK(c.sent[2].req.qty == qt("0.05"));
}

// max_gross_notional / max_net_notional: the engine gives what they leave for one order on each
// side, in the instrument's currency.
TEST_CASE("strategies.hedge_executor: a hedge over the exposure caps' room goes out in pieces") {
  Ctx c;
  c.risk[1].exposure_sell_notional = Notional::from_int(60);
  HedgeExecutor h = make(c, false);
  source_fill(h, c, "0.001");
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.side == Side::Sell);
  CHECK(c.sent[0].req.qty == qt("0.06"));
  // The room is the engine's, each time: the first piece used some of it.
  c.risk[1].exposure_sell_notional = Notional::from_int(25);
  filled(h, c, 0);
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.qty == qt("0.02"));
  c.risk[1].exposure_sell_notional = Notional::max();
  filled(h, c, 1);
  REQUIRE(c.sent.size() == 3);
  CHECK(c.sent[2].req.qty == qt("0.02"));
  filled(h, c, 2);
  CHECK(h.residual(c).is_zero());
  // The buy side has room of its own (it reduces the short hedge position: no bound).
  c.risk[1].exposure_sell_notional = Notional{};
  source_fill(h, c, "-0.002");
  REQUIRE(c.sent.size() == 4);
  CHECK(c.sent[3].req.side == Side::Buy);
  CHECK(c.sent[3].req.qty == qt("0.2"));
  CHECK(c.risk_refused.empty());
  CHECK(h.stats().hedge_failures == 0);
  // The tighter of the per-order cap and the exposure room decides.
  RiskHeadroom both;
  both.max_order_notional = Notional::from_int(60);
  both.exposure_sell_notional = Notional::from_int(25);
  CHECK(HedgeExecutor::risk_room(both, c.table.get(kB), Side::Sell, px("99950.0")) == qt("0.02"));
  CHECK(HedgeExecutor::risk_room(both, c.table.get(kB), Side::Buy, px("100050.2")) == qt("0.05"));
}

TEST_CASE("strategies.hedge_executor: max_order_qty and the position room cut a hedge too") {
  Ctx c;
  c.risk[1].max_order_qty = qt("0.4");
  HedgeExecutor h = make(c, false);
  source_fill(h, c, "0.01");  // 1 contract
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.qty == qt("0.4"));
  filled(h, c, 0);
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.qty == qt("0.4"));
  // max_position leaves room for 0.15 more contracts short: rounded down to the lot.
  c.risk[1].sell_qty = qt("0.155");
  filled(h, c, 1);
  REQUIRE(c.sent.size() == 3);
  CHECK(c.sent[2].req.qty == qt("0.15"));
  // [risk.underlying] max_net, when it is the tightest.
  c.risk[1].sell_qty = Qty::max();
  c.risk[1].underlying_sell_qty = qt("0.02");
  filled(h, c, 2);
  REQUIRE(c.sent.size() == 4);
  CHECK(c.sent[3].req.qty == qt("0.02"));
  CHECK(c.risk_refused.empty());
  CHECK(h.stats().hedge_failures == 0);
  // The other side's rooms do not enter.
  CHECK(HedgeExecutor::risk_room(c.risk[1], c.table.get(kB), Side::Buy, px("100000")) == qt("0.4"));
}

// No room at all (the hedge instrument is at max_position), or less than the instrument's minimum:
// the order goes out whole, the engine refuses it and the refusal is a failure - the hedge moves
// to the next instrument, or hedging halts.
TEST_CASE("strategies.hedge_executor: a hedge [risk] leaves no room for fails over or halts") {
  SUBCASE("to the next instrument") {
    Ctx c;
    c.risk[1].sell_qty = Qty{};
    HedgeExecutor h = make(c);
    source_fill(h, c, "0.01");
    CHECK(c.sent.empty());
    REQUIRE(c.risk_refused.size() == 1);
    CHECK(c.risk_refused[0].qty == qt("1"));  // whole
    CHECK(h.stats().hedge_failures == 1);
    for (int i = 0; i < 2; ++i) {
      c.advance(milliseconds(100));
      h.on_timer(c);
    }
    CHECK(h.stats().hedge_failures == 3);
    CHECK(h.stats().benches == 1);
    c.advance(milliseconds(100));
    h.on_timer(c);
    REQUIRE(c.sent.size() == 1);
    CHECK(c.sent[0].req.instrument == kC);
    CHECK(c.sent[0].req.qty == qt("0.01"));
    CHECK(h.stats().failovers == 1);
  }
  SUBCASE("halts on the last one") {
    Ctx c;
    c.risk[1].max_order_notional = Notional::from_int(5);  // under one lot of 0.01 (9.995 USDT)
    HedgeExecutor h = make(c, false);
    source_fill(h, c, "0.01");
    for (int i = 0; i < 5; ++i) {
      c.advance(milliseconds(100));
      h.on_timer(c);
    }
    CHECK(c.sent.empty());
    CHECK(c.risk_refused.size() == 3);
    CHECK(h.stats().hedge_failures == 3);
    CHECK(h.halted());
  }
  SUBCASE("a cut under min_notional is not sent as a piece") {
    Ctx c;
    c.table.get(kB).min_notional = Notional::from_int(30);
    c.risk[1].max_order_notional = Notional::from_int(25);  // 0.02 contracts: 19.99 USDT
    HedgeExecutor h = make(c, false);
    source_fill(h, c, "0.001");
    CHECK(c.sent.empty());
    REQUIRE(c.risk_refused.size() == 1);
    CHECK(c.risk_refused[0].qty == qt("0.1"));
    CHECK(h.stats().hedge_failures == 1);
  }
}

TEST_CASE("strategies.hedge_executor: qty_within is the largest lot multiple under the notional") {
  Ctx c;
  const Instrument& b = c.table.get(kB);   // 0.01 BTC contracts, lot 0.01
  const Instrument& sp = c.table.get(kC);  // BTC, lot 0.001
  CHECK(HedgeExecutor::qty_within(b, px("83914.2"), Notional::from_int(60)) == qt("0.07"));
  CHECK(b.notional(px("83914.2"), qt("0.07")) <= Notional::from_int(60));
  CHECK(b.notional(px("83914.2"), qt("0.08")) > Notional::from_int(60));
  CHECK(HedgeExecutor::qty_within(sp, px("100000"), Notional::from_int(1000)) == qt("0.01"));
  CHECK(HedgeExecutor::qty_within(sp, px("100000"), Notional::from_int(99)).is_zero());
  CHECK(HedgeExecutor::qty_within(sp, Price{}, Notional::from_int(99)).is_zero());
  CHECK(HedgeExecutor::qty_within(sp, px("100000"), Notional{}).is_zero());
}

TEST_CASE("strategies.hedge_executor: a de-risk order is cut to the per-order limits as well") {
  Ctx c;
  HedgeExecutor::Config cfg = config();
  cfg.derisk_after = milliseconds(500);
  cfg.derisk_interval = milliseconds(200);
  cfg.derisk_tolerance = bps(10);
  c.risk[0].max_order_notional = Notional::from_int(300);  // 0.003 BTC at 99900
  HedgeExecutor h = make(c, false, cfg);
  c.killed[1] = true;
  source_fill(h, c, "0.01");
  c.advance(milliseconds(600));
  h.on_timer(c);
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == kSrc);
  CHECK(c.sent[0].req.reduce_only);
  CHECK(c.sent[0].req.qty == qt("0.003"));
  CHECK(c.risk_refused.empty());
}

TEST_CASE("strategies.hedge_executor: a remainder under the minimum waits without failing") {
  Ctx c;
  c.table.get(kC).min_notional = Notional::from_int(50);
  c.table.get(kC).lot = qt("0.0001");
  c.table.get(kC).min_qty = qt("0.0001");
  HedgeExecutor h = make(c);
  c.killed[1] = true;           // B's kill switch: C decides
  source_fill(h, c, "0.0004");  // 0.0004 BTC sells at 99950: 39.98 USDT
  CHECK(c.sent.empty());
  CHECK(h.state() == HedgeExecutor::State::Waiting);
  for (int i = 0; i < 5; ++i) {
    c.advance(milliseconds(100));
    h.on_timer(c);
  }
  CHECK(c.sent.empty());
  CHECK(h.stats().hedge_failures == 0);
  source_fill(h, c, "0.0002");
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == kC);
  CHECK(c.sent[0].req.qty == qt("0.0006"));
}

TEST_CASE("strategies.hedge_executor: one order in flight across every hedge instrument") {
  Ctx c;
  HedgeExecutor h = make(c);
  source_fill(h, c, "0.01");
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == kB);
  // B's order channel drops with the hedge out: C is usable, but the open order on B may still
  // fill, so nothing goes to C.
  h.on_connection(c, connection(1, 1, ConnState::Disconnected));
  source_fill(h, c, "0.01");
  h.on_timer(c);
  CHECK(c.sent.size() == 1);
  CHECK(h.status(c).in_flight);
  // It ends with half filled: the rest goes to C, sized from the positions.
  end(h, c, 0, OrderState::Expired, "0.5");
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.instrument == kC);
  CHECK(c.sent[1].req.qty == qt("0.015"));
  CHECK(h.stats().failovers == 1);
}

TEST_CASE("strategies.hedge_executor: an unreported outcome holds every hedge instrument") {
  Ctx c;
  HedgeExecutor h = make(c);
  source_fill(h, c, "0.01");
  REQUIRE(c.sent.size() == 1);
  // The ack timeout cancelled B's hedge; B goes down too. C is usable but waits out the hold.
  end(h, c, 0, OrderState::Canceled, "0", /*acked=*/false);
  CHECK(h.stats().uncertain_ends == 1);
  CHECK(h.stats().hedge_failures == 1);
  h.on_connection(c, connection(1, 1, ConnState::Disconnected));
  c.advance(milliseconds(500));
  h.on_timer(c);
  CHECK(c.sent.size() == 1);
  CHECK(h.state() == HedgeExecutor::State::Holding);
  CHECK(h.status(c).hold_until == Timestamp{kT0 + milliseconds(1000).ns});
  // The fill was real and arrives late: nothing is left once the hold is over.
  c.pos[1] -= qt("1");
  c.advance(milliseconds(600));
  h.on_timer(c);
  CHECK(c.sent.size() == 1);
  CHECK(h.state() == HedgeExecutor::State::Flat);
  // A generic venue reject and a reconciliation drop hold the same way.
  source_fill(h, c, "0.01");
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.instrument == kC);
  end(h, c, 1, OrderState::Rejected, "0", false, "0", RejectReason::VenueReject);
  c.advance(milliseconds(900));
  h.on_timer(c);
  CHECK(c.sent.size() == 2);
  c.advance(milliseconds(200));
  h.on_timer(c);
  REQUIRE(c.sent.size() == 3);
  end(h, c, 2, OrderState::Canceled, "0", true, "0.01");
  CHECK(h.stats().uncertain_ends == 3);
  c.advance(milliseconds(500));
  h.on_timer(c);
  CHECK(c.sent.size() == 3);
}

TEST_CASE("strategies.hedge_executor: no order while a venue reconciles") {
  Ctx c;
  HedgeExecutor h = make(c);
  c.reconciling_now = true;
  source_fill(h, c, "0.01");
  CHECK(c.sent.empty());
  CHECK(h.state() == HedgeExecutor::State::Reconciling);
  c.reconciling_now = false;
  h.update(c);
  CHECK(c.sent.size() == 1);
}

// Each trigger moves the next hedge from B to C, and B takes the one after it back.
TEST_CASE("strategies.hedge_executor: failover on each trigger, and back") {
  Ctx c;
  HedgeExecutor h = make(c);
  auto round = [&](InstrumentId expect) {
    source_fill(h, c, "0.01");
    REQUIRE_FALSE(c.sent.empty());
    CHECK(c.sent.back().req.instrument == expect);
    CHECK(c.sent.back().req.price == px("99950.0"));
    filled(h, c, c.sent.size() - 1);
    CHECK(h.residual(c).is_zero());
  };
  round(kB);
  SUBCASE("order channel down") {
    h.on_connection(c, connection(1, 1, ConnState::Disconnected));
    h.on_connection(c, connection(1, 0, ConnState::Disconnected));  // market data: the book decides
    round(kC);
    h.on_connection(c, connection(1, 1, ConnState::Live));
    round(kB);
  }
  SUBCASE("venue killed") {
    c.killed[1] = true;
    CHECK(h.why(c, 0, c.t.ns) == HedgeExecutor::Reason::Killed);
    round(kC);
    c.killed[1] = false;
    round(kB);
  }
  SUBCASE("feed-lag gate") {
    c.gated[1] = true;
    round(kC);
    c.gated[1] = false;
    round(kB);
  }
  SUBCASE("book invalid") {
    c.books[1].valid = false;
    round(kC);
    c.books[1].valid = true;
    round(kB);
  }
  SUBCASE("book stale") {
    c.t = c.t + milliseconds(2500);
    c.books[0].updated = c.t;
    c.books[2].updated = c.t;
    CHECK(h.why(c, 0, c.t.ns) == HedgeExecutor::Reason::Stale);
    round(kC);
    c.books[1].updated = c.t;
    round(kB);
  }
  SUBCASE("balance or margin short") {
    c.room[1][static_cast<std::size_t>(Side::Sell)] = qt("0.5");  // half a hedge on B
    round(kC);
    CHECK_FALSE(h.held());
    CHECK(h.stats().hedges_held == 0);
    c.room[1][static_cast<std::size_t>(Side::Sell)] = Qty::max();
    round(kB);
  }
  SUBCASE("refused max_failures times: benched, then back") {
    c.refuse[1] = true;
    source_fill(h, c, "0.01");
    for (int i = 0; i < 2; ++i) {
      c.advance(milliseconds(100));
      h.on_timer(c);
    }
    CHECK(h.stats().hedge_failures == 3);
    CHECK(h.stats().benches == 1);
    CHECK(h.why(c, 0, c.t.ns) == HedgeExecutor::Reason::Benched);
    CHECK_FALSE(h.halted());
    c.advance(milliseconds(100));
    h.on_timer(c);
    REQUIRE(c.sent.size() == 2);
    CHECK(c.sent[1].req.instrument == kC);
    filled(h, c, 1);
    c.refuse[1] = false;
    round(kC);  // still benched
    c.advance(milliseconds(5000));
    round(kB);
  }
  CHECK(h.stats().failovers == 1);
  CHECK_FALSE(h.halted());
}

TEST_CASE("strategies.hedge_executor: no usable balance anywhere holds the hedge") {
  Ctx c;
  HedgeExecutor h = make(c);
  c.room[1][static_cast<std::size_t>(Side::Sell)] = Qty{};
  c.room[2][static_cast<std::size_t>(Side::Sell)] = qt("0.005");
  source_fill(h, c, "0.01");
  CHECK(c.sent.empty());
  CHECK(h.held());
  CHECK(h.state() == HedgeExecutor::State::Held);
  for (int i = 0; i < 5; ++i) h.on_timer(c);
  CHECK(h.stats().hedges_held == 1);
  CHECK(h.stats().hedge_failures == 0);
  c.room[2][static_cast<std::size_t>(Side::Sell)] = qt("0.01");
  BalanceMsg m{};
  init_header(m, EventType::Balance, InstrumentId{}, VenueId{2});
  CHECK(h.on_balance(c, m));
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == kC);
  CHECK_FALSE(h.held());
  // A balance of a venue no leg trades on is not ours.
  init_header(m, EventType::Balance, InstrumentId{}, VenueId{5});
  CHECK_FALSE(h.on_balance(c, m));
}

TEST_CASE("strategies.hedge_executor: every hedge instrument failing halts until restart") {
  Ctx c;
  HedgeExecutor h = make(c);
  c.refuse[1] = true;
  c.refuse[2] = true;
  source_fill(h, c, "0.01");
  for (int i = 0; i < 10; ++i) {
    c.advance(milliseconds(100));
    h.on_timer(c);
  }
  CHECK(h.stats().benches == 1);
  CHECK(h.halted());
  CHECK(h.stats().halts == 1);
  CHECK(h.stats().hedge_failures == 6);
  CHECK_FALSE(h.can_hedge(c));
  CHECK(h.state() == HedgeExecutor::State::Halted);
  c.refuse = {};
  h.on_timer(c);
  CHECK(c.sent.empty());
  h.restart();
  h.on_timer(c);
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == kB);  // the bench is cleared as well
}

TEST_CASE("strategies.hedge_executor: de-risks the source in capped, spaced steps, then recovers") {
  Ctx c;
  HedgeExecutor::Config cfg = config();
  cfg.derisk_after = milliseconds(1000);
  cfg.derisk_step = qt("0.004");
  cfg.derisk_interval = milliseconds(500);
  cfg.derisk_tolerance = bps(10);
  HedgeExecutor h = make(c, true, cfg);
  h.on_connection(c, connection(1, 1, ConnState::Disconnected));
  c.killed[2] = true;
  source_fill(h, c, "0.01");
  CHECK(c.sent.empty());
  CHECK(h.state() == HedgeExecutor::State::Unavailable);
  CHECK_FALSE(h.can_hedge(c));
  c.advance(milliseconds(900));
  h.on_timer(c);
  CHECK(c.sent.empty());
  CHECK_FALSE(h.derisking());
  c.advance(milliseconds(100));
  h.on_timer(c);
  REQUIRE(c.sent.size() == 1);
  CHECK(h.derisking());
  const NewOrderRequest& d = c.sent[0].req;
  CHECK(d.instrument == kSrc);
  CHECK(d.side == Side::Sell);
  CHECK(d.qty == qt("0.004"));      // derisk_step caps it
  CHECK(d.price == px("99900.0"));  // 10 bps through the bid
  CHECK(d.reduce_only);
  CHECK(d.tif == TimeInForce::Ioc);
  CHECK(d.user_tag == cfg.derisk_tag);
  // In flight: nothing more.
  c.advance(milliseconds(600));
  h.on_timer(c);
  CHECK(c.sent.size() == 1);
  // It fills; the next step waits for derisk_interval from the last one's send.
  end(h, c, 0, OrderState::Filled, "0.004");
  REQUIRE(c.sent.size() == 2);  // 600 ms since the first: past the interval
  CHECK(c.sent[1].req.qty == qt("0.004"));
  end(h, c, 1, OrderState::Expired, "0");  // nothing filled: not a hedge failure
  CHECK(h.stats().hedge_failures == 0);
  CHECK(c.sent.size() == 2);
  c.advance(milliseconds(400));
  h.on_timer(c);
  CHECK(c.sent.size() == 2);
  c.advance(milliseconds(100));
  h.on_timer(c);
  REQUIRE(c.sent.size() == 3);
  CHECK(c.sent[2].req.qty == qt("0.004"));
  end(h, c, 2, OrderState::Filled, "0.004");
  CHECK(h.residual(c) == qt("0.002"));
  // B comes back: the rest is hedged there and de-risking stops.
  h.on_connection(c, connection(1, 1, ConnState::Live));
  c.advance(milliseconds(500));
  h.on_timer(c);
  REQUIRE(c.sent.size() == 4);
  CHECK(c.sent[3].req.instrument == kB);
  CHECK(c.sent[3].req.qty == qt("0.2"));
  CHECK_FALSE(h.derisking());
  CHECK(h.stats().derisk_episodes == 1);
  CHECK(h.stats().derisk_orders == 3);
  filled(h, c, 3);
  CHECK(h.residual(c).is_zero());
}

TEST_CASE("strategies.hedge_executor: a de-risk step is capped by the residual and the source") {
  Ctx c;
  HedgeExecutor::Config cfg = config();
  cfg.derisk_after = milliseconds(100);
  cfg.derisk_step = qt("1");
  cfg.derisk_interval = milliseconds(100);
  HedgeExecutor h = make(c, false, cfg);
  c.killed[1] = true;
  c.pos[1] = qt("0.5");  // B holds +0.005 BTC from before
  source_fill(h, c, "-0.002");
  // residual +0.003 (long) but the source is short: nothing on the source reduces it.
  c.advance(milliseconds(200));
  h.on_timer(c);
  CHECK(c.sent.empty());
  CHECK(h.derisking());
  // Short residual, source short 0.003 and B long 0.001: at most the residual.
  c.pos[1] = qt("0.1");
  source_fill(h, c, "-0.001");
  c.advance(milliseconds(200));
  h.on_timer(c);
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.side == Side::Buy);
  CHECK(c.sent[0].req.qty == qt("0.002"));
  // An unreported de-risk end holds as a hedge's does.
  end(h, c, 0, OrderState::Canceled, "0", false);
  CHECK(h.stats().uncertain_ends == 1);
  c.advance(milliseconds(500));
  h.on_timer(c);
  CHECK(c.sent.size() == 1);
  c.advance(milliseconds(600));
  h.on_timer(c);
  CHECK(c.sent.size() == 2);
}

TEST_CASE("strategies.hedge_executor: halted hedging de-risks, restart brings the hedges back") {
  Ctx c;
  HedgeExecutor::Config cfg = config();
  cfg.derisk_after = milliseconds(300);
  cfg.derisk_step = qt("0.01");
  cfg.derisk_interval = milliseconds(1000);
  HedgeExecutor h = make(c, false, cfg);
  c.refuse[1] = true;
  source_fill(h, c, "0.02");
  for (int i = 0; i < 3; ++i) {
    c.advance(milliseconds(100));
    h.on_timer(c);
  }
  REQUIRE(h.halted());
  CHECK(c.sent.empty());
  for (int i = 0; i < 4; ++i) {
    c.advance(milliseconds(100));
    h.on_timer(c);
  }
  REQUIRE(c.sent.size() == 1);
  CHECK(c.sent[0].req.instrument == kSrc);
  filled(h, c, 0);
  CHECK(h.residual(c) == qt("0.01"));
  c.refuse[1] = false;
  h.restart();
  h.on_timer(c);
  REQUIRE(c.sent.size() == 2);
  CHECK(c.sent[1].req.instrument == kB);
  CHECK_FALSE(h.derisking());
}

// ---- xmm with a fallback through the real engine ------------------------------------------------

namespace {

using fastmm::sim::HarnessOptions;
using fastmm::sim::StrategyHarness;

// A: BTCUSDT on venue 0 (quotes). B: BTC-USDT-SWAP on venue 1 (0.01 BTC contracts). C: BTCUSDT on
// venue 2 (BTC).
HarnessOptions three_venues() {
  HarnessOptions o;
  o.instruments.clear();
  REQUIRE(o.instruments.add(linear("BTCUSDT", 0, "1", "0.001")));
  REQUIRE(o.instruments.add(linear("BTC-USDT-SWAP", 1, "0.01", "1")));
  REQUIRE(o.instruments.add(linear("BTCUSDT", 2, "1", "0.001")));
  return o;
}

void rest(StrategyHarness<Xmm>& h, InstrumentId id, const char* price, const char* qty) {
  static std::uint64_t seq = 2000;
  sim::NewOrder o;
  o.account = 9;
  o.cl_ord_id = ClientOrderId{++seq};
  o.instrument = id;
  o.side = Side::Buy;
  o.price = px(price);
  o.qty = qt(qty);
  static_cast<void>(h.venue_transport().matching_engine().submit(o, h.now()));
}

void control(StrategyHarness<Xmm>& h, ControlCommand cmd, VenueId v) {
  ControlMsg m{};
  init_header(m, EventType::Control, InstrumentId{}, v);
  m.command = cmd;
  m.arg = static_cast<std::uint64_t>(KillReason::VenueFatal);
  h.push(m.hdr);
}

}  // namespace

// Through the engine's own risk check. [risk] max_order_qty = 1 admits A's quotes (0.03 BTC) and
// one contract of B an order: the 3 contracts to hedge go out as three orders, and the risk engine
// refuses none.
TEST_CASE(
    "strategies.hedge_executor: engine: a hedge over a [risk] order cap is split, not refused") {
  const ParamMap p{{"quote_qty", "0.03"}, {"basis_halflife_s", "0"}, {"max_unhedged", "0.1"}};
  HarnessOptions o = three_venues();
  o.engine.risk.max_order_qty = qt("1");
  StrategyHarness<Xmm> h(p, o);
  const InstrumentId a{0};
  h.book(px("99990"), px("100010"), Qty::from_int(1), a);
  h.book(px("100000.0"), px("100000.2"), Qty::from_int(1), kB);
  h.advance(milliseconds(1));
  REQUIRE(h.working_orders(a).size() == 2);
  rest(h, kB, "100000.0", "10");
  REQUIRE(h.fill(Side::Buy, Qty{}, a));
  h.advance(milliseconds(5));
  CHECK(h.engine().position(kB).qty == qt("-3"));
  CHECK(h.strategy().stats().hedges_sent == 3);
  CHECK(h.strategy().stats().hedge_failures == 0);
  const RiskStats& rs = h.engine().risk().stats();
  CHECK(rs.rejects[static_cast<std::uint8_t>(RejectReason::MaxOrderQty)] == 0);
  CHECK(rs.rejects[static_cast<std::uint8_t>(RejectReason::MaxOrderNotional)] == 0);
}

TEST_CASE("strategies.hedge_executor: engine: B killed, the hedge goes to C, then back to B") {
  const ParamMap p{{"quote_qty", "0.02"},
                   {"basis_halflife_s", "0"},
                   {"max_unhedged", "0.1"},
                   {"fallback_instrument", "2"}};
  StrategyHarness<Xmm> h(p, three_venues());
  const InstrumentId a{0};
  h.book(px("99990"), px("100010"), Qty::from_int(1), a);
  h.book(px("100000.0"), px("100000.2"), Qty::from_int(1), kB);
  h.book(px("100000.0"), px("100000.2"), Qty::from_int(1), kC);
  h.advance(milliseconds(1));
  REQUIRE(h.working_orders(a).size() == 2);
  rest(h, kB, "100000.0", "10");  // 0.1 BTC
  rest(h, kC, "100000.0", "0.1");
  auto net = [&] {
    auto& e = h.engine();
    return e.position(a).qty + Xmm::to_base(e.instruments().get(kB), e.position(kB).qty) +
           e.position(kC).qty;
  };

  REQUIRE(h.fill(Side::Buy, Qty{}, a));
  h.advance(milliseconds(5));
  CHECK(h.engine().position(kB).qty == qt("-2"));
  CHECK(net().is_zero());

  control(h, ControlCommand::TripVenueKill, VenueId{1});
  REQUIRE(h.engine().risk().venue_killed(VenueId{1}));
  h.advance(milliseconds(1));
  REQUIRE(h.working_orders(a).size() == 2);  // C can hedge: A keeps quoting
  REQUIRE(h.fill(Side::Buy, Qty{}, a));
  h.advance(milliseconds(5));
  CHECK(h.engine().position(kC).qty == qt("-0.02"));
  CHECK(h.engine().position(kB).qty == qt("-2"));
  CHECK(net().is_zero());
  CHECK(h.strategy().stats().failovers == 1);
  CHECK(h.strategy().stats().hedge_failures == 0);

  control(h, ControlCommand::ResetKill, VenueId{1});
  h.advance(milliseconds(1));
  REQUIRE(h.working_orders(a).size() == 2);
  REQUIRE(h.fill(Side::Buy, Qty{}, a));
  h.advance(milliseconds(5));
  CHECK(h.engine().position(kB).qty == qt("-4"));
  CHECK(h.engine().position(kC).qty == qt("-0.02"));
  CHECK(net().is_zero());
  CHECK(h.strategy().stats().hedges_sent == 3);

  // Both hedge venues killed: no fill could be hedged, so A's quotes come off.
  control(h, ControlCommand::TripVenueKill, VenueId{1});
  control(h, ControlCommand::TripVenueKill, VenueId{2});
  h.advance(Xmm::kTimerPeriod + milliseconds(1));
  CHECK(h.working_orders(a).empty());
}
