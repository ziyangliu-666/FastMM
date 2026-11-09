// Home placement ([venues.<primary>.pool], core/pool_homes.hpp): what each account holds, the home
// an instrument's adding orders go to, the holders its reducing orders go to (the pieces first),
// the reconciliation once every account has reported, a home that cannot be reached, the
// operator's and the strategy's home changes, and the observe mode that only keeps the homes.
#include "fastmm/core/engine.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
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

constexpr InstrumentId kSpot{0};  // BTCUSDT spot on the pool of venue 0
constexpr InstrumentId kEth{1};   // ETHUSDT spot on the same pool
constexpr InstrumentId kPerp{2};  // BTCUSDT perpetual on the pool of venue 3
constexpr VenueId kA{0};          // spot primary
constexpr VenueId kB{1};
constexpr VenueId kC{2};
constexpr VenueId kP{3};  // perp primary
constexpr VenueId kQ{4};

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument s{};
  s.symbol = "BTCUSDT";
  s.base = "BTC";
  s.quote = "USDT";
  s.venue = kA;
  s.asset_class = AssetClass::Spot;
  s.flags = Instrument::kEnabled;
  s.tick = px("0.01");
  s.lot = qt("0.0001");
  s.min_qty = qt("0.0001");
  s.min_notional = nt("5");
  REQUIRE(t.add(s));
  Instrument e = s;
  e.symbol = "ETHUSDT";
  e.base = "ETH";
  REQUIRE(t.add(e));
  Instrument p = s;
  p.venue = kP;
  p.asset_class = AssetClass::Perpetual;
  p.lot = qt("0.001");
  p.min_qty = qt("0.001");
  REQUIRE(t.add(p));
  return t;
}

struct Transport {
  std::vector<std::vector<std::byte>> out;
  bool send(const EventHeader& m) noexcept {
    const auto* b = reinterpret_cast<const std::byte*>(&m);
    out.emplace_back(b, b + m.len);
    return true;
  }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept {
    for (const EventHeader* m : batch) static_cast<void>(send(*m));
    return batch.size();
  }
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return true; }
  // Each account's order windows, when a test gives them (venue_known).
  std::array<OrderBudget, 8> budget{};
  bool venue_budget(VenueId v, OrderBudget& o) const noexcept {
    if (v.value >= budget.size() || !budget[v.value].venue_known) return false;
    o = budget[v.value];
    return true;
  }
  // The OutNewOrders and OutCancels sent so far, in order.
  [[nodiscard]] std::vector<const OutNewOrderMsg*> news() const {
    std::vector<const OutNewOrderMsg*> r;
    for (const auto& b : out) {
      const auto* h = reinterpret_cast<const EventHeader*>(b.data());
      if (h->type == EventType::OutNewOrder)
        r.push_back(reinterpret_cast<const OutNewOrderMsg*>(h));
    }
    return r;
  }
  [[nodiscard]] std::vector<ClientOrderId> cancels() const {
    std::vector<ClientOrderId> r;
    for (const auto& b : out) {
      const auto* h = reinterpret_cast<const EventHeader*>(b.data());
      if (h->type == EventType::OutCancel)
        r.push_back(reinterpret_cast<const OutCancelMsg*>(h)->cl_ord_id);
    }
    return r;
  }
};

struct Mute {
  static std::string_view name() noexcept { return "mute"; }
};

EngineConfig make_config(Placement placement, std::uint8_t reduce_accounts = 1) {
  EngineConfig cfg;
  cfg.fees = FeeTable::from_bps(0, 0);
  cfg.risk.max_open_orders = 100;
  REQUIRE(cfg.pools.add(kB, kA));
  REQUIRE(cfg.pools.add(kC, kA));
  REQUIRE(cfg.pools.add(kQ, kP));
  for (const VenueId v : {kA, kP}) {
    PoolPlacement& p = cfg.homes.pool[v.value];
    p.configured = true;
    p.placement = placement;
    p.reduce_accounts = reduce_accounts;
    p.plan_interval = Duration{};
  }
  return cfg;
}

struct Rig {
  using E = Engine<Mute, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(1000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  Mute strategy;
  MsgRing journal{1U << 20};
  std::unique_ptr<E> engine;
  std::uint32_t execs = 0;

  explicit Rig(const EngineConfig& cfg) {
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy, &journal);
    engine->warm_up();
    engine->start();
    book(kSpot, "49990", "50010");
    book(kEth, "2999", "3001");
    book(kPerp, "49990", "50010");
  }
  void drain() {
    while (engine->step() > 0) {
    }
  }
  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    drain();
  }
  void book(InstrumentId id, const char* bid, const char* ask) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d, EventType::BookSnapshot, id, table.get(id).venue, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
    drain();
  }
  // One account's snapshot: its BTC, ETH and USDT, then the end of the snapshot.
  void snapshot(VenueId v, const char* btc, const char* eth, const char* usdt) {
    for (const auto& [asset, amount] :
         {std::pair{"BTC", btc}, std::pair{"ETH", eth}, std::pair{"USDT", usdt}}) {
      BalanceMsg m{};
      init_header(m, EventType::Balance, InstrumentId::invalid(), v);
      m.asset.assign(asset);
      m.free = nt(amount);
      m.total = m.free;
      m.equity = m.total;
      m.flags = BalanceMsg::kSnapshot;
      push(m);
    }
    BalanceMsg end{};
    init_header(end, EventType::Balance, InstrumentId::invalid(), v);
    end.flags = BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd;
    push(end);
  }
  Result<ClientOrderId, RejectReason> send(
      InstrumentId id, Side side, const char* price, const char* qty, bool reduce_only = false) {
    NewOrderRequest r = NewOrderRequest::limit(id, side, px(price), qt(qty));
    r.reduce_only = reduce_only;
    return engine->send_order(r);
  }
  void ack(InstrumentId id, ClientOrderId cl, VenueId v) {
    OrderAckMsg m{};
    init_header(m, EventType::OrderAck, id, v);
    m.cl_ord_id = cl;
    m.venue_order_id.assign("v" + std::to_string(cl.value));
    push(m);
  }
  void fill(
      InstrumentId id, ClientOrderId cl, VenueId v, Side side, const char* price, const char* q) {
    OrderFillMsg m{};
    init_header(m, EventType::OrderFill, id, v);
    m.cl_ord_id = cl;
    m.side = side;
    m.price = px(price);
    m.qty = qt(q);
    m.cum_qty = qt(q);
    m.leaves_qty = Qty{};
    m.exec_id.assign(std::to_string(++execs));
    push(m);
  }
  void link(VenueId v, bool live) {
    ConnectionStateMsg m{};
    init_header(m, EventType::ConnectionState, InstrumentId::invalid(), v);
    m.channel = 1;
    m.state = live ? ConnState::Live : ConnState::Disconnected;
    push(m);
  }
  void control_home(InstrumentId id, VenueId to, std::uint16_t flags = 0) {
    ControlMsg m{};
    init_header(m, EventType::Control, id, table.get(id).venue);
    m.command = ControlCommand::PoolHome;
    m.arg = pool_home_arg(to, HomeSource::Operator);
    m.hdr.flags = static_cast<std::uint8_t>(m.hdr.flags | flags);
    push(m);
  }
  [[nodiscard]] VenueId last_account() const { return transport.news().back()->hdr.venue; }
  // The PoolHome records the journal holds, in order: (instrument, to, source).
  [[nodiscard]] std::vector<std::tuple<std::uint32_t, VenueId, HomeSource>> journaled_homes() {
    std::vector<std::tuple<std::uint32_t, VenueId, HomeSource>> out;
    while (const std::byte* p = journal.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      if (h->type == EventType::Control && h->len >= sizeof(ControlMsg)) {
        ControlMsg m;
        std::memcpy(&m, p, sizeof m);
        if (m.command == ControlCommand::PoolHome)
          out.emplace_back(m.hdr.instrument.value, pool_home_to(m.arg), pool_home_source(m.arg));
      }
      journal.release();
    }
    return out;
  }
};

}  // namespace

TEST_CASE("core.engine home: without a pool table there are no homes and routing is by balance") {
  EngineConfig cfg = make_config(Placement::Balance);
  cfg.homes = PoolHomesPlan{};
  Rig r(cfg);
  auto& ctx = r.engine->context();
  CHECK(ctx.placement(kA) == Placement::Balance);
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.01"));
  CHECK_FALSE(ctx.home(kSpot).valid());
  CHECK(ctx.pieces(kSpot) == 0);
}

TEST_CASE("core.engine home: adding orders go to the home, the first one gives it") {
  Rig r(make_config(Placement::Home));
  auto& ctx = r.engine->context();
  CHECK(ctx.placement(kA) == Placement::Home);
  CHECK(ctx.placement(kB) == Placement::Home);  // a member reads its pool's
  CHECK_FALSE(ctx.home(kSpot).valid());
  // Flat and no holder: the instrument's turn round the pool (id 0: the primary).
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  CHECK(ctx.home(kSpot) == kA);
  CHECK(r.last_account() == kA);
  // ETH (id 1) starts on the second account; every buy then goes there.
  for (int k = 0; k < 3; ++k) {
    REQUIRE(r.send(kEth, Side::Buy, "2900", "0.01"));
    CHECK(r.last_account() == kB);
  }
  CHECK(ctx.home(kEth) == kB);
  // The first homes are journaled as the engine's own.
  const auto homes = r.journaled_homes();
  REQUIRE(homes.size() == 2);
  CHECK(std::get<2>(homes[0]) == HomeSource::First);
}

TEST_CASE("core.engine home: holdings follow each account's fills and reports") {
  Rig r(make_config(Placement::Home));
  auto& ctx = r.engine->context();
  // Before balances: a spot holding is the account's own fills.
  const auto id = r.send(kSpot, Side::Buy, "49000", "0.002");
  REQUIRE(id);
  r.ack(kSpot, *id, kA);
  r.fill(kSpot, *id, kA, Side::Buy, "49000", "0.002");
  CHECK(ctx.holding(kSpot, kA) == qt("0.002"));
  CHECK(ctx.holding(kSpot, kB) == Qty{});
  // Once reported, a spot holding is the account's base balance.
  r.snapshot(kB, "0.5", "0", "1000");
  CHECK(ctx.holding(kSpot, kB) == qt("0.5"));
  // A derivative's holding: its fills on that account, then the account's position report.
  const auto pid = r.send(kPerp, Side::Sell, "50100", "0.01");
  REQUIRE(pid);
  const VenueId home = ctx.home(kPerp);
  r.ack(kPerp, *pid, home);
  r.fill(kPerp, *pid, home, Side::Sell, "50100", "0.01");
  CHECK(ctx.holding(kPerp, home) == Qty::from_raw(-qt("0.01").raw));
  PositionUpdateMsg pu{};
  init_header(pu, EventType::PositionUpdate, kPerp, kQ);
  pu.qty = qt("0.03");
  pu.avg_px = px("50000");
  r.push(pu);
  CHECK(ctx.holding(kPerp, kQ) == qt("0.03"));
  // An account outside the pool holds nothing of it.
  CHECK(ctx.holding(kSpot, kQ) == Qty{});
}

TEST_CASE("core.engine home: the reconciliation gives the single holder, keeps a largest holder") {
  Rig r(make_config(Placement::Home));
  auto& ctx = r.engine->context();
  // BTC on B only; ETH on A (0.1) and C (0.4); dust of BTC on C below min_notional.
  r.snapshot(kA, "0", "0.1", "1000");
  r.snapshot(kB, "0.2", "0", "1000");
  CHECK_FALSE(ctx.home(kSpot).valid());  // C has not reported yet
  r.snapshot(kC, "0.00001", "0.4", "1000");
  CHECK(ctx.home(kSpot) == kB);
  CHECK(ctx.pieces(kSpot) == 0);  // C's dust is no piece
  CHECK(ctx.home(kEth) == kC);
  CHECK(ctx.pieces(kEth) == 1);
  // balance_room sizes a sell to the account it goes to: the piece's 0.1, not the home's 0.4.
  CHECK(ctx.balance_room(kEth, Side::Sell, px("3100")) == qt("0.1"));
  // Buys go home; the sells work the piece on A off first.
  REQUIRE(r.send(kEth, Side::Buy, "2900", "0.01"));
  CHECK(r.last_account() == kC);
  const auto s = r.send(kEth, Side::Sell, "3100", "0.1");
  REQUIRE(s);
  CHECK(r.last_account() == kA);
  // With reduce_accounts = 1 the home's part waits while the piece is offered: a second sell
  // goes to A again, where the balance check refuses what A does not hold.
  CHECK(r.send(kEth, Side::Sell, "3100", "0.1").error() == RejectReason::BalanceShort);
  // The piece is offered whole: nothing more to sell until it fills.
  CHECK(ctx.balance_room(kEth, Side::Sell, px("3100")) == Qty{});
  CHECK(ctx.balance_room(kEth, Side::Buy, px("2900")) ==
        ctx.balance_room(kEth, Side::Buy, px("2900"), kC));
}

TEST_CASE("core.engine home: reduce_accounts 2 offers the home's part beside a piece") {
  Rig r(make_config(Placement::Home, 2));
  r.snapshot(kA, "0", "0.1", "1000");
  r.snapshot(kB, "0", "0", "1000");
  r.snapshot(kC, "0", "0.4", "1000");
  REQUIRE(r.send(kEth, Side::Sell, "3100", "0.1"));
  CHECK(r.last_account() == kA);
  REQUIRE(r.send(kEth, Side::Sell, "3100", "0.4"));
  CHECK(r.last_account() == kC);
}

TEST_CASE("core.engine home: a pinned home survives the reconciliation") {
  EngineConfig cfg = make_config(Placement::Home);
  cfg.homes.pin[kSpot.value] = kC;
  Rig r(cfg);
  auto& ctx = r.engine->context();
  CHECK(ctx.home(kSpot) == kC);  // at the start
  r.snapshot(kA, "0.3", "0", "1000");
  r.snapshot(kB, "0", "0", "1000");
  r.snapshot(kC, "0", "0", "1000");
  CHECK(ctx.home(kSpot) == kC);
  CHECK(ctx.pieces(kSpot) == 1);
  const auto homes = r.journaled_homes();
  REQUIRE_FALSE(homes.empty());
  CHECK(std::get<2>(homes[0]) == HomeSource::Pin);
}

TEST_CASE("core.engine home: a home that cannot be reached refuses, or spills a flat instrument") {
  SUBCASE("never") {
    Rig r(make_config(Placement::Home));
    REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
    r.link(kA, false);
    CHECK(r.send(kSpot, Side::Buy, "49000", "0.001").error() == RejectReason::HomeUnavailable);
    CHECK(r.engine->stats().home_unavailable == 1);
    CHECK(r.engine->context().home(kSpot) == kA);
  }
  SUBCASE("flat") {
    EngineConfig cfg = make_config(Placement::Home);
    cfg.homes.pool[kA.value].spill = HomeSpill::Flat;
    Rig r(cfg);
    REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
    r.link(kA, false);
    REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
    CHECK(r.last_account() != kA);
    CHECK(r.engine->context().home(kSpot) == r.last_account());
  }
}

TEST_CASE(
    "core.engine home: the operator moves a home; the old home's adding orders are cancelled") {
  Rig r(make_config(Placement::Home));
  auto& ctx = r.engine->context();
  r.snapshot(kA, "0.1", "0", "1000");
  r.snapshot(kB, "0", "0", "1000");
  r.snapshot(kC, "0", "0", "1000");
  REQUIRE(ctx.home(kSpot) == kA);
  const auto buy = r.send(kSpot, Side::Buy, "49000", "0.001");
  const auto sell = r.send(kSpot, Side::Sell, "51000", "0.1");
  REQUIRE(buy);
  REQUIRE(sell);
  r.ack(kSpot, *buy, kA);
  r.ack(kSpot, *sell, kA);
  r.control_home(kSpot, kB);
  CHECK(ctx.home(kSpot) == kB);
  // The buy is cancelled; the sell works A's part (now a piece) off.
  CHECK(r.transport.cancels() == std::vector<ClientOrderId>{*buy});
  CHECK(ctx.pieces(kSpot) == 1);
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  CHECK(r.last_account() == kB);
  // A home outside the pool, and the engine's own record coming back as input, change nothing.
  r.control_home(kSpot, kQ);
  r.control_home(kSpot, kC, EventHeader::kSynthetic);
  CHECK(ctx.home(kSpot) == kB);
}

TEST_CASE("core.engine home: the strategy moves a home with set_home") {
  Rig r(make_config(Placement::Home));
  auto& ctx = r.engine->context();
  CHECK(ctx.set_home(kSpot, kC));
  CHECK(ctx.home(kSpot) == kC);
  CHECK_FALSE(ctx.set_home(kSpot, kQ));
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  CHECK(r.last_account() == kC);
  const auto homes = r.journaled_homes();
  REQUIRE(homes.size() == 1);
  CHECK(std::get<2>(homes[0]) == HomeSource::Strategy);
}

TEST_CASE("core.engine home: observe mode keeps the homes and routes by balance") {
  Rig r(make_config(Placement::Balance));
  auto& ctx = r.engine->context();
  r.snapshot(kA, "0", "0", "1000");
  r.snapshot(kB, "0.2", "0", "1000");
  r.snapshot(kC, "0", "0", "1000");
  CHECK(ctx.home(kSpot) == kB);  // reconciled all the same
  r.control_home(kSpot, kC);
  CHECK(ctx.home(kSpot) == kC);
  // A buy goes wherever the balance routing sends it; nothing is cancelled for a home.
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  CHECK(r.transport.cancels().empty());
  PoolLoad load;
  r.engine->pool_load(kA, load);
  REQUIRE(load.count == 2);
  CHECK(load.instrument[0] == kSpot);
  CHECK(load.home[0] == kC);
  CHECK(load.orders[0] == 1);
  CHECK(load.members.size() == 3);
}

TEST_CASE("core.engine home: a derivative piece is never taken through zero and is worked off") {
  Rig r(make_config(Placement::Home));
  auto& ctx = r.engine->context();
  REQUIRE(ctx.set_home(kPerp, kP));
  const auto report = [&](VenueId v, const char* qty) {
    PositionUpdateMsg pu{};
    init_header(pu, EventType::PositionUpdate, kPerp, v);
    pu.qty = Qty::from_decimal(qty).value();
    pu.avg_px = px("50000");
    r.push(pu);
  };
  report(kP, "0.05");
  report(kQ, "0.01");
  CHECK(ctx.pieces(kPerp) == 1);
  // Larger than the piece: the home. The piece's own size: the piece, beside the home's order.
  REQUIRE(r.send(kPerp, Side::Sell, "50100", "0.05"));
  CHECK(r.last_account() == kP);
  REQUIRE(r.send(kPerp, Side::Sell, "50100", "0.01"));
  CHECK(r.last_account() == kQ);
  // A piece on the other side: the side that reduces it goes there, whatever the pool holds.
  report(kQ, "-0.02");
  REQUIRE(r.send(kPerp, Side::Buy, "49900", "0.01"));
  CHECK(r.last_account() == kQ);
  REQUIRE(r.send(kPerp, Side::Buy, "49900", "0.05"));
  CHECK(r.last_account() == kP);
}

TEST_CASE("core.engine home: a piece takes the sell slot while only the home carries one") {
  Rig r(make_config(Placement::Home));
  r.snapshot(kA, "0", "0", "1000");
  r.snapshot(kB, "0", "0", "1000");
  r.snapshot(kC, "0", "0.4", "1000");
  REQUIRE(r.engine->context().home(kEth) == kC);
  REQUIRE(r.send(kEth, Side::Sell, "3100", "0.4"));
  CHECK(r.last_account() == kC);
  // A piece appears on A: its sell goes there although the home carries one.
  r.snapshot(kA, "0", "0.1", "1000");
  REQUIRE(r.send(kEth, Side::Sell, "3100", "0.1"));
  CHECK(r.last_account() == kA);
}

TEST_CASE("core.engine home: balance_room follows a spill, so a fitted quote reaches it") {
  EngineConfig cfg = make_config(Placement::Home);
  cfg.homes.pool[kA.value].spill = HomeSpill::Flat;
  Rig r(cfg);
  auto& ctx = r.engine->context();
  r.snapshot(kA, "0", "0", "1000");
  r.snapshot(kB, "0", "0", "2000");
  r.snapshot(kC, "0", "0", "1000");
  REQUIRE(ctx.set_home(kSpot, kA));
  r.link(kA, false);
  const Qty room = ctx.balance_room(kSpot, Side::Buy, px("49000"));
  CHECK(room.is_positive());
  CHECK(room == ctx.balance_room(kSpot, Side::Buy, px("49000"), kB));
  // A pinned home does not spill: nothing to size.
  r.control_home(kSpot, kC);
  r.link(kC, false);
  CHECK(ctx.balance_room(kSpot, Side::Buy, px("49000")) == Qty{});
  CHECK(r.send(kSpot, Side::Buy, "49000", "0.001").error() == RejectReason::HomeUnavailable);
}

namespace {
// spill = "busy" on the spot pool; each account's 10 s window admits `limit` orders.
EngineConfig busy_config() {
  EngineConfig cfg = make_config(Placement::Home);
  cfg.homes.pool[kA.value].spill = HomeSpill::Busy;
  return cfg;
}
void windows(Rig& r, std::int64_t limit) {
  for (const VenueId v : {kA, kB, kC}) {
    OrderBudget& b = r.transport.budget[v.value];
    b.venue_known = true;
    b.orders_10s.window_ms = 10'000;
    b.orders_10s.limit = limit;
  }
}
}  // namespace

TEST_CASE("core.engine home: spill busy sends adding orders off a home whose window is busy") {
  Rig r(busy_config());
  windows(r, 10);
  auto& ctx = r.engine->context();
  REQUIRE(ctx.set_home(kSpot, kA));
  // Below 0.8 of the window: the home. From the eighth order on: another account, the home stays.
  for (int k = 0; k < 8; ++k) {
    REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
    CHECK(r.last_account() == kA);
  }
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  CHECK(r.last_account() != kA);
  CHECK(ctx.home(kSpot) == kA);
  CHECK(r.engine->stats().home_spills == 1);
  // A sell with no holder still goes where it did; nothing re-homes.
  REQUIRE(r.send(kSpot, Side::Sell, "51000", "0.001"));
  CHECK(r.last_account() == kA);
  CHECK(r.journaled_homes().size() == 1);  // the strategy's set_home only
}

TEST_CASE("core.engine home: spill_at sets how full the home's window gets") {
  EngineConfig cfg = busy_config();
  cfg.homes.pool[kA.value].spill_at = 0.5;
  Rig r(cfg);
  windows(r, 10);
  REQUIRE(r.engine->context().set_home(kSpot, kA));
  for (int k = 0; k < 5; ++k) REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  CHECK(r.last_account() == kA);
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  CHECK(r.last_account() != kA);
}

TEST_CASE("core.engine home: spill busy sends a buy its home cannot pay for to one that can") {
  Rig r(busy_config());
  auto& ctx = r.engine->context();
  r.snapshot(kA, "0", "0", "10");
  r.snapshot(kB, "0", "0", "1000");
  r.snapshot(kC, "0", "0", "100");
  REQUIRE(ctx.set_home(kSpot, kA));
  // The home covers it: the home.
  REQUIRE(r.send(kSpot, Side::Buy, "5000", "0.001"));
  CHECK(r.last_account() == kA);
  // It does not: the account with the most room that does.
  const auto buy = r.send(kSpot, Side::Buy, "49000", "0.002");
  REQUIRE(buy);
  CHECK(r.last_account() == kB);
  CHECK(ctx.home(kSpot) == kA);
  // balance_room sizes against any account, as the routing will place it: what B covers with its
  // own open buy, less the pool's open buys.
  CHECK(ctx.balance_room(kSpot, Side::Buy, px("49000")) ==
        ctx.balance_room(kSpot, Side::Buy, px("49000"), kB) + qt("0.002") - qt("0.003"));
  // Its fill makes a piece on B; the sell side reduces B's part first, as before.
  r.ack(kSpot, *buy, kB);
  r.fill(kSpot, *buy, kB, Side::Buy, "49000", "0.002");
  CHECK(ctx.pieces(kSpot) == 1);
  REQUIRE(r.send(kSpot, Side::Sell, "51000", "0.002"));
  CHECK(r.last_account() == kB);
}

TEST_CASE("core.engine home: spill busy sends past a home that is down, and keeps it") {
  Rig r(busy_config());
  auto& ctx = r.engine->context();
  REQUIRE(ctx.set_home(kSpot, kA));
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  r.link(kA, false);
  const auto buy = r.send(kSpot, Side::Buy, "49000", "0.001");
  REQUIRE(buy);
  const VenueId to = r.last_account();
  CHECK(to != kA);
  CHECK(ctx.home(kSpot) == kA);
  r.ack(kSpot, *buy, to);
  // The home's link is back: the spilled order stays.
  r.link(kA, true);
  CHECK(r.transport.cancels().empty());
  REQUIRE(r.send(kSpot, Side::Buy, "49000", "0.001"));
  CHECK(r.last_account() == kA);
  // No account at all: refused as before.
  for (const VenueId v : {kA, kB, kC}) r.link(v, false);
  CHECK(r.send(kSpot, Side::Buy, "49000", "0.001").error() == RejectReason::HomeUnavailable);
}

TEST_CASE("core.engine home: spill busy moves a flat derivative off a home that is down") {
  EngineConfig cfg = busy_config();
  cfg.homes.pool[kP.value].spill = HomeSpill::Busy;
  Rig r(cfg);
  auto& ctx = r.engine->context();
  REQUIRE(ctx.set_home(kPerp, kP));
  r.link(kP, false);
  REQUIRE(r.send(kPerp, Side::Buy, "49000", "0.001"));
  CHECK(r.last_account() == kQ);
  CHECK(ctx.home(kPerp) == kQ);
}
