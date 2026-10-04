// Account pools (core/account_pool.hpp) through the simulator: a member is a link of its own with
// its own account over the primary's book; the engine sends an order to the account the request
// names, else to the member whose balance covers it, and the order's events, cancels and fills
// stay on that member.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/quoting.hpp"

#include <map>
#include <memory>
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
Notional nt(const char* s) {
  return Notional::from_decimal(s).value();
}

constexpr InstrumentId kBtc{0};
constexpr VenueId kPrimary{0};
constexpr VenueId kMember{1};

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = kPrimary;
  i.flags = Instrument::kEnabled;
  i.tick = px("0.01");
  i.lot = qt("0.00001");
  i.min_qty = qt("0.00001");
  i.base = "BTC";
  i.quote = "USDT";
  REQUIRE(t.add(i));
  return t;
}

MarketGeneratorParams gen_params() {
  MarketGeneratorParams p;
  p.start_mid = px("60000");
  p.tick = px("0.01");
  p.lot = qt("0.00001");
  p.limit_rate_per_s = 300;
  p.market_rate_per_s = 40;
  p.mid_step_rate_per_s = 5;
  p.market_qty_median_lots = 300;
  return p;
}

SimAccountConfig account(VenueId v, const char* btc, const char* usdt) {
  return SimAccountConfig{
      v,
      {SimBalance{FixedString<8>("BTC"), nt(btc)}, SimBalance{FixedString<8>("USDT"), nt(usdt)}}};
}

struct Seen {
  VenueId venue;
  OrderState state;
};

// Sends one order of each kind once the book and the balances are there, then cancels the first.
struct Probe {
  int step = 0;
  ClientOrderId explicit_member, auto_buy, auto_sell, explicit_primary, taker;
  RejectReason bad_account = RejectReason::None;
  RejectReason unknown_primary = RejectReason::None;
  std::map<std::uint64_t, std::vector<Seen>> updates;  // by client order id
  std::vector<VenueId> fill_venues;
  std::vector<Qty> fill_qtys;
  PoolMembers pool;
  VenueId primary_of_member;
  bool member_usable = false;

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    if (!b.is_valid() || !ctx.balances_live()) return;
    const Instrument& inst = ctx.instrument(id);
    const Price deep_bid = b.best_bid().price - inst.ticks(2000);
    const Price deep_ask = b.best_ask().price + inst.ticks(2000);
    if (step == 0) {
      step = 1;
      pool = ctx.pool(kPrimary);
      primary_of_member = ctx.pool_primary(kMember);
      member_usable = ctx.account_usable(kMember);
      explicit_member =
          *ctx.send(NewOrderRequest::limit(id, Side::Buy, deep_bid, qt("0.01")).account(kMember));
      auto_buy = *ctx.send(NewOrderRequest::limit(id, Side::Buy, deep_bid, qt("0.01")));
      auto_sell = *ctx.send(NewOrderRequest::limit(id, Side::Sell, deep_ask, qt("0.01")));
      explicit_primary =
          *ctx.send(NewOrderRequest::limit(id, Side::Buy, deep_bid, qt("0.01")).account(kPrimary));
      bad_account =
          ctx.send(NewOrderRequest::limit(id, Side::Buy, deep_bid, qt("0.01")).account(VenueId{5}))
              .error();
      // A sell at the bid from the member: it holds the base, and the fill lands there.
      taker = *ctx.send(NewOrderRequest::limit(id, Side::Sell, b.best_bid().price, qt("0.01"))
                            .ioc()
                            .account(kMember));
    } else if (step == 1 && ctx.order(explicit_member) != nullptr &&
               ctx.order(explicit_member)->is_working()) {
      step = 2;
      REQUIRE(ctx.cancel(explicit_member));
    }
  }
  template <class Ctx>
  void on_order_update(Ctx&, const OmsUpdate& u) noexcept {
    updates[u.order.cl_ord_id.value].push_back(Seen{u.order.venue, u.order.state});
  }
  template <class Ctx>
  void on_fill(Ctx&, const Fill& f) noexcept {
    fill_venues.push_back(f.msg->hdr.venue);
    fill_qtys.push_back(f.qty);
  }
};

using ProbeEngine = Engine<Probe, SimClock, SimTransport, InlineFeed>;

struct Rig {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1'700'000'000).ns}};
  SimTransportConfig tc;
  std::unique_ptr<SimTransport> transport;
  InlineFeed feed{1 << 22};
  Probe probe;
  std::unique_ptr<ProbeEngine> engine;

  Rig(const char* primary_btc, const char* member_btc, bool check_balance = true) {
    tc.seed = 7;
    tc.fees = FeeModel::from_bps(0.0, 0.0);
    REQUIRE(tc.pools.add(kMember, kPrimary));
    tc.accounts = {account(kPrimary, primary_btc, "100000"),
                   account(kMember, member_btc, "100000")};
    transport = std::make_unique<SimTransport>(clock, table, tc);
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    cfg.risk.max_position = qt("5");
    cfg.risk.max_open_orders = 16;
    cfg.pools = tc.pools;
    cfg.balance.check = check_balance;
    engine = std::make_unique<ProbeEngine>(cfg, table, clock, *transport, feed, probe);
  }
  void run(Duration horizon) {
    MarketGenerator gen(gen_params(), 7, kBtc, clock.now(), clock.now() + horizon);
    SimDriver driver(clock, *transport, feed, EngineHooks::for_engine(*engine));
    driver.set_generator(&gen, 20);
    driver.run_all();
    driver.finish();
  }
};

}  // namespace

TEST_CASE(
    "sim.pool: an order goes to the account the request names, else to the member that "
    "holds the balance, and stays there") {
  Rig rig("0", "1");  // the primary holds the quote only, the member the base too
  rig.run(seconds(2));
  const Probe& p = rig.probe;
  REQUIRE(p.step == 2);
  REQUIRE(p.pool.size() == 2);
  CHECK(p.pool[0] == kPrimary);
  CHECK(p.pool[1] == kMember);
  CHECK(p.primary_of_member == kPrimary);
  CHECK(p.member_usable);
  CHECK(p.bad_account == RejectReason::InvalidAccount);
  CHECK(rig.engine->stats().risk_rejects == 0);

  const auto venue_of = [&](ClientOrderId id) {
    const auto it = p.updates.find(id.value);
    REQUIRE(it != p.updates.end());
    REQUIRE_FALSE(it->second.empty());
    return it->second.front().venue;
  };
  const auto last_state = [&](ClientOrderId id) { return p.updates.at(id.value).back().state; };
  // Explicit accounts are honoured, member and primary alike.
  CHECK(venue_of(p.explicit_member) == kMember);
  CHECK(venue_of(p.explicit_primary) == kPrimary);
  // Both accounts cover the buy: the tie goes to the primary.
  CHECK(venue_of(p.auto_buy) == kPrimary);
  // Only the member holds the base: the sell goes there.
  CHECK(venue_of(p.auto_sell) == kMember);
  // The cancel reached the member that holds the order.
  CHECK(last_state(p.explicit_member) == OrderState::Canceled);
  for (const Seen& s : p.updates.at(p.explicit_member.value)) CHECK(s.venue == kMember);
  // The taker filled on the member, and its account paid: 0.01 BTC less, the proceeds more.
  REQUIRE(p.fill_venues.size() >= 1);
  CHECK(p.fill_venues.front() == kMember);
  CHECK(venue_of(p.taker) == kMember);
  const SimAccounts& acct = *rig.transport->accounts();
  Qty sold{};
  for (const Qty q : p.fill_qtys) sold += q;
  CHECK(acct.total(kMember, "BTC") == Notional::from_raw(nt("1").raw - sold.raw));
  CHECK(acct.total(kPrimary, "BTC").is_zero());
  CHECK(acct.total(kMember, "USDT") > nt("100000"));
  // The engine's estimate of the member's balances followed the fill (ctx.balance per member).
  CHECK(rig.engine->balance(kMember, "BTC").known);
  CHECK(rig.engine->balance(kMember, "BTC").total == acct.total(kMember, "BTC"));
  CHECK(rig.engine->balance(kPrimary, "BTC").total.is_zero());
  // The orders still resting are held by the account they went to.
  CHECK(rig.transport->order_venue(p.auto_sell) == kMember);
  CHECK(rig.transport->order_venue(p.auto_buy) == kPrimary);
  CHECK_FALSE(rig.transport->order_venue(p.explicit_member).valid());  // cancelled
}

TEST_CASE("sim.pool: no member covers the order: the primary refuses it as the venue would") {
  Rig rig("0", "0", /*check_balance=*/false);  // nobody holds the base; the venue decides
  rig.run(seconds(1));
  const Probe& p = rig.probe;
  REQUIRE(p.step >= 1);
  // The automatic sell found no account with the base and went to the primary, which refused it
  // as Binance does; the explicit taker from the member was refused the same way.
  CHECK(rig.engine->stats().risk_rejects == 0);
  CHECK(p.auto_sell.valid());
  CHECK(p.updates.at(p.auto_sell.value).front().venue == kPrimary);
  CHECK(p.updates.at(p.auto_sell.value).back().state == OrderState::Rejected);
  CHECK(rig.engine->stats().venue_rejects_by_reason[RejectReason::InsufficientBalance] >= 1);
}

// ---- order-count windows (SimVenueConfig::orders_10s / orders_1d) ------------------------------

namespace {

// Three orders from the primary by name, then automatic ones; after 11 s one more from the primary.
struct WindowProbe {
  int step = 0;
  std::vector<ClientOrderId> named;  // explicit, on the primary
  std::vector<ClientOrderId> automatic;
  std::vector<RejectReason> refused;  // send() errors
  ClientOrderId later;
  OrderBudget primary_budget, member_budget;
  std::map<std::uint64_t, std::vector<Seen>> updates;

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    if (step != 0 || !b.is_valid() || !ctx.balances_live()) return;
    step = 1;
    const Price deep = b.best_bid().price - ctx.instrument(id).ticks(2000);
    for (int k = 0; k < 3; ++k) {
      const auto r =
          ctx.send(NewOrderRequest::limit(id, Side::Buy, deep, qt("0.01")).account(kPrimary));
      if (r) {
        named.push_back(*r);
      } else {
        refused.push_back(r.error());
      }
    }
    for (int k = 0; k < 4; ++k) {
      const auto r = ctx.send(NewOrderRequest::limit(id, Side::Buy, deep, qt("0.01")));
      if (r) {
        automatic.push_back(*r);
      } else {
        refused.push_back(r.error());
      }
    }
    primary_budget = ctx.order_budget(kPrimary);
    member_budget = ctx.order_budget(kMember);
    static_cast<void>(ctx.once(seconds(11)));
  }
  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t) noexcept {
    const Price deep = ctx.book(kBtc).best_bid().price - ctx.instrument(kBtc).ticks(2000);
    later = *ctx.send(NewOrderRequest::limit(kBtc, Side::Buy, deep, qt("0.01")).account(kPrimary));
  }
  template <class Ctx>
  void on_order_update(Ctx&, const OmsUpdate& u) noexcept {
    updates[u.order.cl_ord_id.value].push_back(Seen{u.order.venue, u.order.state});
  }
};

using WindowEngine = Engine<WindowProbe, SimClock, SimTransport, InlineFeed>;

struct WindowRig {
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1'700'000'000).ns}};
  SimTransportConfig tc;
  std::unique_ptr<SimTransport> transport;
  InlineFeed feed{1 << 22};
  WindowProbe probe;
  std::unique_ptr<WindowEngine> engine;

  WindowRig(std::int64_t primary_10s, std::int64_t member_10s, std::int64_t primary_1d = 0) {
    tc.seed = 7;
    tc.fees = FeeModel::from_bps(0.0, 0.0);
    REQUIRE(tc.pools.add(kMember, kPrimary));
    tc.accounts = {account(kPrimary, "1", "100000"), account(kMember, "1", "100000")};
    SimVenueConfig p = tc.venue_config(kPrimary);
    p.orders_10s = primary_10s;
    p.orders_1d = primary_1d;
    SimVenueConfig m = tc.venue_config(kMember);
    m.orders_10s = member_10s;
    tc.venues = {p, m};
    transport = std::make_unique<SimTransport>(clock, table, tc);
    EngineConfig cfg;
    cfg.risk.max_order_qty = qt("1");
    cfg.risk.max_position = qt("5");
    cfg.risk.max_open_orders = 32;
    cfg.pools = tc.pools;
    engine = std::make_unique<WindowEngine>(cfg, table, clock, *transport, feed, probe);
  }
  void run(Duration horizon) {
    MarketGenerator gen(gen_params(), 7, kBtc, clock.now(), clock.now() + horizon);
    SimDriver driver(clock, *transport, feed, EngineHooks::for_engine(*engine));
    driver.set_generator(&gen, 20);
    driver.run_all();
    driver.finish();
  }
};

}  // namespace

TEST_CASE("sim.pool: the automatic routing takes the member with the most 10 s window left") {
  WindowRig rig(100, 100);
  rig.run(seconds(1));
  const WindowProbe& p = rig.probe;
  REQUIRE(p.step == 1);
  CHECK(p.refused.empty());
  REQUIRE(p.named.size() == 3);
  REQUIRE(p.automatic.size() == 4);
  const auto venue_of = [&](ClientOrderId id) { return p.updates.at(id.value).front().venue; };
  for (const ClientOrderId id : p.named) CHECK(venue_of(id) == kPrimary);
  // The primary used 3 of 100, the member none: the member takes the next three (100, 99, 98
  // against 97), then the tie goes to the primary.
  CHECK(venue_of(p.automatic[0]) == kMember);
  CHECK(venue_of(p.automatic[1]) == kMember);
  CHECK(venue_of(p.automatic[2]) == kMember);
  CHECK(venue_of(p.automatic[3]) == kPrimary);
  // What the strategy read right after: the budgets per account, known in a backtest with limits.
  CHECK(p.primary_budget.venue_known);
  CHECK(p.primary_budget.orders_10s.limit == 100);
  CHECK(p.primary_budget.orders_10s.used == 4);
  CHECK(p.primary_budget.orders_10s.remaining() == 96);
  CHECK(p.member_budget.orders_10s.used == 3);
  CHECK_FALSE(p.primary_budget.orders_1d.known());
}

TEST_CASE("sim.pool: an order past the venue's window is refused, and the window turns over") {
  WindowRig rig(2, 0);  // the primary admits two per 10 s, the member has no limit
  rig.run(seconds(12));
  const WindowProbe& p = rig.probe;
  REQUIRE(p.step == 1);
  REQUIRE(p.named.size() == 3);
  const auto last_state = [&](ClientOrderId id) { return p.updates.at(id.value).back().state; };
  CHECK(last_state(p.named[0]) == OrderState::Live);
  CHECK(last_state(p.named[1]) == OrderState::Live);
  CHECK(last_state(p.named[2]) == OrderState::Rejected);
  CHECK(rig.transport->stats().rejects_rate_limit == 1);
  CHECK(rig.engine->stats().venue_rejects_by_reason[RejectReason::VenueRateLimit] == 1);
  // The automatic orders all went to the member: unlimited counts as the most room.
  for (const ClientOrderId id : p.automatic) CHECK(p.updates.at(id.value).front().venue == kMember);
  CHECK(p.primary_budget.orders_10s.remaining() == 0);
  CHECK_FALSE(p.member_budget.venue_known);
  // Eleven seconds on, the primary's window has turned over.
  REQUIRE(p.later.valid());
  CHECK(last_state(p.later) == OrderState::Live);
  CHECK(p.updates.at(p.later.value).front().venue == kPrimary);
}

TEST_CASE("sim.pool: the daily window refuses too") {
  WindowRig rig(100, 0, /*primary_1d=*/1);
  rig.run(seconds(12));
  const WindowProbe& p = rig.probe;
  REQUIRE(p.named.size() == 3);
  const auto last_state = [&](ClientOrderId id) { return p.updates.at(id.value).back().state; };
  CHECK(last_state(p.named[0]) == OrderState::Live);
  CHECK(last_state(p.named[1]) == OrderState::Rejected);
  CHECK(last_state(p.named[2]) == OrderState::Rejected);
  CHECK(p.primary_budget.orders_1d.limit == 1);
  CHECK(p.primary_budget.orders_1d.remaining() == 0);
  CHECK(p.primary_budget.orders_10s.remaining() == 99);
  CHECK(last_state(p.later) == OrderState::Rejected);  // the day is not over
  CHECK(rig.transport->stats().rejects_rate_limit == 3);
}

// ---- sizing against a pool: balance_room is the largest single holder ---------------------------

namespace {

// The primary holds 0.3 BTC, the member 0.5: one order lands on one account, so a sell can be at
// most 0.5, whatever the two hold together.
struct SizingProbe {
  int step = 0;
  Qty room_before, room_after;
  Qty fitted_before, fitted_after;
  Qty open_after;
  RejectReason too_big = RejectReason::None;
  ClientOrderId biggest;
  std::map<std::uint64_t, std::vector<Seen>> updates;

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& b) noexcept {
    if (step != 0 || !b.is_valid() || !ctx.balances_live()) return;
    step = 1;
    const Instrument& inst = ctx.instrument(id);
    const Price deep = b.best_ask().price + inst.ticks(2000);
    room_before = ctx.balance_room(id, Side::Sell, deep);
    DesiredQuotes q;
    static_cast<void>(q.ask(deep, qt("0.8")));
    fit_to_balance(ctx, id, inst, q);
    fitted_before = q.asks[0].qty;
    too_big = ctx.send(NewOrderRequest::limit(id, Side::Sell, deep, qt("0.6"))).error();
    biggest = *ctx.send(NewOrderRequest::limit(id, Side::Sell, deep, qt("0.5")));
    // With 0.5 held on the member, the primary's 0.3 is the most one order can be; the ladder may
    // still total the room plus what rests (open_qty is over every account).
    room_after = ctx.balance_room(id, Side::Sell, deep);
    open_after = ctx.open_qty(id, Side::Sell);
    DesiredQuotes q2;
    static_cast<void>(q2.ask(deep, qt("0.8")));
    fit_to_balance(ctx, id, inst, q2);
    fitted_after = q2.asks[0].qty;
  }
  template <class Ctx>
  void on_order_update(Ctx&, const OmsUpdate& u) noexcept {
    updates[u.order.cl_ord_id.value].push_back(Seen{u.order.venue, u.order.state});
  }
};

using SizingEngine = Engine<SizingProbe, SimClock, SimTransport, InlineFeed>;

}  // namespace

TEST_CASE("sim.pool: balance_room is the largest single member's room, open_qty the whole pool's") {
  const InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1'700'000'000).ns}};
  SimTransportConfig tc;
  tc.seed = 7;
  tc.fees = FeeModel::from_bps(0.0, 0.0);
  REQUIRE(tc.pools.add(kMember, kPrimary));
  tc.accounts = {account(kPrimary, "0.3", "100000"), account(kMember, "0.5", "100000")};
  SimTransport transport(clock, table, tc);
  InlineFeed feed{1 << 22};
  SizingProbe probe;
  EngineConfig cfg;
  cfg.risk.max_order_qty = qt("1");
  cfg.risk.max_position = qt("5");
  cfg.risk.max_open_orders = 16;
  cfg.pools = tc.pools;
  SizingEngine engine(cfg, table, clock, transport, feed, probe);
  MarketGenerator gen(gen_params(), 7, kBtc, clock.now(), clock.now() + seconds(1));
  SimDriver driver(clock, transport, feed, EngineHooks::for_engine(engine));
  driver.set_generator(&gen, 20);
  driver.run_all();
  driver.finish();

  REQUIRE(probe.step == 1);
  CHECK(probe.room_before == qt("0.5"));               // not 0.8: one order, one account
  CHECK(probe.fitted_before == qt("0.5"));             // fit_to_balance cuts, nothing is refused
  CHECK(probe.too_big == RejectReason::BalanceShort);  // no single member covers 0.6
  CHECK(engine.stats().risk_rejects_by_reason[RejectReason::BalanceShort] == 1);
  REQUIRE(probe.biggest.valid());
  CHECK(probe.updates.at(probe.biggest.value).front().venue == kMember);
  CHECK(probe.updates.at(probe.biggest.value).back().state == OrderState::Live);
  CHECK(probe.room_after == qt("0.3"));
  CHECK(probe.open_after == qt("0.5"));
  CHECK(probe.fitted_after == qt("0.8"));  // 0.3 of room plus the 0.5 resting
  CHECK(engine.balance(kMember, "BTC").free.is_zero());
  CHECK(engine.balance(kPrimary, "BTC").free == nt("0.3"));
}
