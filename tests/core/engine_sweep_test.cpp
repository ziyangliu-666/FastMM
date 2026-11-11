// A pool's sweep and transfer lane in the engine (core/transfer_lane.hpp): a piece goes home once
// it has rested, held out of the payer until the flows or the history confirm it; the out leg or
// the in leg first, a stray flow, two assets at once; unknown answers, refusals, a blocked asset,
// the breaker, the operator, a restart's holds, the dry run; and the routing of a sweeping pool,
// which sends spot sells home and moves a piece's resting sell there.
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

constexpr InstrumentId kBtc{0};
constexpr InstrumentId kEth{1};
constexpr VenueId kA{0};  // the primary
constexpr VenueId kB{1};
constexpr VenueId kC{2};

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
  [[nodiscard]] std::vector<const OutNewOrderMsg*> news() const {
    std::vector<const OutNewOrderMsg*> r;
    for (const auto& b : out) {
      const auto* h = reinterpret_cast<const EventHeader*>(b.data());
      if (h->type == EventType::OutNewOrder)
        r.push_back(reinterpret_cast<const OutNewOrderMsg*>(h));
    }
    return r;
  }
  [[nodiscard]] std::size_t count(EventType t) const {
    std::size_t n = 0;
    for (const auto& b : out)
      n += reinterpret_cast<const EventHeader*>(b.data())->type == t ? 1U : 0U;
    return n;
  }
};

struct Mute {
  static std::string_view name() noexcept { return "mute"; }
};

EngineConfig make_config(bool dry_run = false) {
  EngineConfig cfg;
  cfg.fees = FeeTable::from_bps(0, 0);
  cfg.risk.max_open_orders = 100;
  REQUIRE(cfg.pools.add(kB, kA));
  REQUIRE(cfg.pools.add(kC, kA));
  PoolPlacement& p = cfg.homes.pool[kA.value];
  p.configured = true;
  p.placement = Placement::Home;
  p.plan_interval = Duration{};
  LaneConfig& l = cfg.homes.lane[kA.value];
  l.sweep = SweepMode::On;
  l.sweep_delay = seconds(3);
  l.sweep_dust = seconds(3600);
  l.sweep_retry = seconds(30);
  l.timeout = seconds(60);
  l.breaker = seconds(900);
  l.dry_run = dry_run;
  cfg.homes.pin[kBtc.value] = kA;
  cfg.homes.pin[kEth.value] = kA;
  return cfg;
}

struct Rig {
  using E = Engine<Mute, SimClock, Transport, InlineFeed>;
  InstrumentTable table = make_table();
  SimClock clock{Timestamp{seconds(100'000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  Mute strategy;
  MsgRing journal{1U << 22};
  std::unique_ptr<E> engine;
  std::uint32_t execs = 0;

  explicit Rig(const EngineConfig& cfg) {
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy, &journal);
    engine->warm_up();
    engine->start();
    book(kBtc, "49990", "50010");
    book(kEth, "2999", "3001");
  }
  void drain() {
    while (engine->step() > 0) {
    }
  }
  // Time passes in sweep periods, the timers firing on the way.
  void wait(std::int64_t ms) {
    for (std::int64_t t = 0; t < ms; t += 250) {
      clock.advance(milliseconds(250));
      drain();
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
  void snapshot(VenueId v, const char* btc, const char* eth, const char* usdt) {
    for (const auto& [asset, amount] :
         {std::pair{"BTC", btc}, std::pair{"ETH", eth}, std::pair{"USDT", usdt}}) {
      BalanceMsg m{};
      init_header(m, EventType::Balance, InstrumentId::invalid(), v);
      m.asset.assign(asset);
      m.free = nt(amount);
      m.total = m.free;
      m.equity = m.total;
      m.hdr.exch_ts = clock.now();
      m.flags = BalanceMsg::kSnapshot;
      push(m);
    }
    BalanceMsg end{};
    init_header(end, EventType::Balance, InstrumentId::invalid(), v);
    end.hdr.exch_ts = clock.now();
    end.flags = BalanceMsg::kSnapshot | BalanceMsg::kSnapshotEnd;
    push(end);
  }
  // One asset's report outside a snapshot (Binance outboundAccountPosition).
  void report(VenueId v, const char* asset, const char* free, const char* locked) {
    BalanceMsg m{};
    init_header(m, EventType::Balance, InstrumentId::invalid(), v);
    m.asset.assign(asset);
    m.free = nt(free);
    m.locked = nt(locked);
    m.total = Notional::from_raw(m.free.raw + m.locked.raw);
    m.equity = m.total;
    m.hdr.exch_ts = clock.now();
    push(m);
  }
  void pool(const char* a_btc, const char* b_btc, const char* c_btc = "0") {
    snapshot(kA, a_btc, "0", "1000");
    snapshot(kB, b_btc, "0", "1000");
    snapshot(kC, c_btc, "0", "1000");
  }
  void flow(VenueId v, const char* asset, const char* amount) {
    AccountFlowMsg f{};
    init_header(f, EventType::AccountFlow, InstrumentId::invalid(), v);
    f.hdr.exch_ts = clock.now();
    f.asset.assign(asset);
    f.amount = nt(amount);
    push(f);
  }
  // The worker's answer about transfer `seq`.
  void answer(TreasuryEventKind kind,
              std::uint64_t seq,
              TransferState state,
              int code = 0,
              int http = 0,
              const char* amount = "0",
              VenueId from = kB,
              const char* asset = "BTC") {
    ControlTransferMsg m{};
    init_header(m, EventType::Control, InstrumentId::invalid(), kA);
    m.command = ControlCommand::Transfer;
    m.event = static_cast<std::uint8_t>(kind);
    m.state = static_cast<std::uint8_t>(state);
    m.flags = ControlTransferMsg::kLane | ControlTransferMsg::kHeld;
    m.from = from;
    m.to = kA;
    m.arg = static_cast<std::uint64_t>(nt(amount).raw);
    m.asset.assign(asset);
    m.seq = seq;
    m.code = code;
    m.http_status = static_cast<std::uint16_t>(http);
    push(m);
  }
  void sweep(std::uint64_t arg, InstrumentId id = InstrumentId::invalid()) {
    ControlMsg m{};
    init_header(m, EventType::Control, id, kA);
    m.command = ControlCommand::PoolSweep;
    m.arg = arg;
    push(m);
  }
  std::vector<LaneIntent> intents(bool all = false) {
    std::vector<LaneIntent> r;
    LaneIntent in;
    while (engine->pop_transfer_intent(in)) {
      if (all || in.kind == LaneIntent::Kind::Send) r.push_back(in);
    }
    return r;
  }
  [[nodiscard]] Notional free(VenueId v, const char* asset = "BTC") {
    return engine->balance(v, asset).free;
  }
  [[nodiscard]] Notional total(VenueId v, const char* asset = "BTC") {
    return engine->balance(v, asset).total;
  }
  [[nodiscard]] const LaneStats& stats() { return engine->lane(kA)->stats(); }
  // The lane records the journal holds: (event, synthetic).
  std::vector<std::pair<TreasuryEventKind, bool>> lane_records() {
    std::vector<std::pair<TreasuryEventKind, bool>> out;
    while (const std::byte* p = journal.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      if (h->type == EventType::Control && h->len >= sizeof(ControlTransferMsg)) {
        ControlTransferMsg m;
        std::memcpy(&m, p, sizeof m);
        if (m.command == ControlCommand::Transfer && (m.flags & ControlTransferMsg::kLane) != 0)
          out.emplace_back(static_cast<TreasuryEventKind>(m.event),
                           (h->flags & EventHeader::kSynthetic) != 0);
      }
      journal.release();
    }
    return out;
  }
  Result<ClientOrderId, RejectReason> sell(const char* qty) {
    return engine->send_order(NewOrderRequest::limit(kBtc, Side::Sell, px("51000"), qt(qty)));
  }
};

}  // namespace

TEST_CASE("core.engine sweep: a piece goes home once it has rested, held until both flows land") {
  Rig r(make_config());
  r.pool("0.1", "0.05");
  CHECK(r.engine->sweeping(kA));
  r.wait(2750);
  CHECK(r.intents().empty());  // not yet rested sweep_delay
  r.wait(500);
  const auto in = r.intents();
  REQUIRE(in.size() == 1);
  CHECK(in[0].kind == LaneIntent::Kind::Send);
  CHECK(in[0].from == kB);
  CHECK(in[0].to == kA);
  CHECK(in[0].asset.view() == "BTC");
  CHECK(in[0].amount == nt("0.05").raw);
  // Held: B's free estimate no longer has it, A's does not yet.
  CHECK(r.free(kB) == Notional{});
  CHECK(r.free(kA) == nt("0.1"));
  CHECK(r.stats().in_flight == 1);
  r.answer(TreasuryEventKind::Sent, in[0].seq, TransferState::Pending);
  // The payer's outflow, then the receiver's inflow.
  r.flow(kB, "BTC", "-0.05");
  CHECK(r.total(kB) == Notional{});
  CHECK(r.stats().in_flight == 1);
  r.flow(kA, "BTC", "0.05");
  CHECK(r.total(kA) == nt("0.15"));
  CHECK(r.stats().in_flight == 0);
  CHECK(r.stats().done_by_flows == 1);
  // The worker is told; the venue's reports agree; nothing more to sweep.
  const auto done = r.intents(true);
  REQUIRE(done.size() == 1);
  CHECK(done[0].kind == LaneIntent::Kind::Resolved);
  CHECK(done[0].seq == in[0].seq);
  r.snapshot(kA, "0.15", "0", "1000");
  r.snapshot(kB, "0", "0", "1000");
  r.wait(5000);
  CHECK(r.intents().empty());
  CHECK(r.total(kA) == nt("0.15"));
  // The journal has the engine's request and done (records) and the worker's sent (input).
  const auto recs = r.lane_records();
  REQUIRE(recs.size() == 3);
  CHECK(recs[0] == std::pair{TreasuryEventKind::Request, true});
  CHECK(recs[1] == std::pair{TreasuryEventKind::Sent, false});
  CHECK(recs[2] == std::pair{TreasuryEventKind::Done, true});
}

TEST_CASE("core.engine sweep: an amount a cancel unlocks rests sweep_delay before it goes") {
  Rig r(make_config());
  r.snapshot(kA, "0.1", "0", "1000");
  // The piece's whole holding is locked by a resting sell for a long time: nothing to move.
  r.report(kB, "BTC", "0", "0.05");
  r.wait(60'000);
  CHECK(r.intents().empty());
  // The sell is cancelled: the holding is unchanged, its free part is new. Binance refuses a
  // transfer of it for a moment (-9000), so it rests like a fill's amount would.
  r.report(kB, "BTC", "0.05", "0");
  r.wait(2750);
  CHECK(r.intents().empty());
  r.wait(500);
  const auto in = r.intents();
  REQUIRE(in.size() == 1);
  CHECK(in[0].from == kB);
  CHECK(in[0].amount == nt("0.05").raw);
}

TEST_CASE("core.engine sweep: refusals for want of balance do not trip the breaker as failures") {
  Rig r(make_config());
  r.snapshot(kA, "0.1", "1", "1000");
  r.snapshot(kB, "0.05", "0", "1000");
  r.snapshot(kC, "0", "0.5", "1000");
  // Two refusals of each asset for want of balance, then one other failure: five refusals in ten
  // minutes, of which one says the venue failed. The breaker stays closed, no asset is blocked.
  for (int k = 0; k < 2; ++k) {
    r.wait(k == 0 ? 3250 : 30'250);
    const auto in = r.intents();
    REQUIRE(in.size() == 2);
    for (const LaneIntent& i : in)
      r.answer(TreasuryEventKind::Failed,
               i.seq,
               TransferState::Failed,
               -9000,
               400,
               "0",
               i.from,
               i.asset.view() == "BTC" ? "BTC" : "ETH");
  }
  r.wait(30'250);
  auto in = r.intents();
  REQUIRE(in.size() == 2);
  r.answer(TreasuryEventKind::Failed,
           in[0].seq,
           TransferState::Failed,
           -1000,
           400,
           "0",
           in[0].from,
           in[0].asset.view() == "BTC" ? "BTC" : "ETH");
  CHECK(r.stats().failed == 5);
  CHECK(r.stats().trips == 0);
  CHECK(r.engine->sweeping(kA));
  CHECK_FALSE(r.engine->lane(kA)->blocked("BTC"));
  CHECK_FALSE(r.engine->lane(kA)->blocked("ETH"));
}

TEST_CASE("core.engine sweep: the in leg first, a stray flow, and two assets at once") {
  Rig r(make_config());
  r.snapshot(kA, "0.1", "1", "1000");
  r.snapshot(kB, "0.05", "0", "1000");
  r.snapshot(kC, "0", "0.5", "1000");
  r.wait(3250);
  auto in = r.intents();
  REQUIRE(in.size() == 2);  // one per asset
  const bool btc_first = in[0].asset.view() == "BTC";
  const LaneIntent btc = btc_first ? in[0] : in[1];
  const LaneIntent eth = btc_first ? in[1] : in[0];
  CHECK(eth.from == kC);
  r.flow(kA, "BTC", "0.04");  // a stray amount: not this transfer
  CHECK(r.stats().in_flight == 2);
  r.flow(kA, "BTC", "0.05");  // the in leg first
  r.flow(kA, "ETH", "0.5");
  r.flow(kC, "ETH", "-0.5");
  CHECK(r.stats().in_flight == 1);
  r.flow(kB, "BTC", "-0.05");
  CHECK(r.stats().in_flight == 0);
  CHECK(r.stats().done == 2);
  CHECK(r.total(kA, "ETH") == nt("1.5"));
  CHECK(r.total(kA) == nt("0.15"));  // a flow moves no estimate: the reports do
  CHECK(btc.seq != eth.seq);
}

TEST_CASE("core.engine sweep: an unknown answer keeps the hold until a flow or the history") {
  SUBCASE("then the flows") {
    Rig r(make_config());
    r.pool("0.1", "0.05");
    r.wait(3250);
    const auto in = r.intents();
    REQUIRE(in.size() == 1);
    r.answer(TreasuryEventKind::Sent, in[0].seq, TransferState::Unknown);
    CHECK(r.stats().unknown == 1);
    CHECK(r.free(kB) == Notional{});
    r.flow(kB, "BTC", "-0.05");
    r.flow(kA, "BTC", "0.05");
    CHECK(r.stats().done == 1);
  }
  SUBCASE("then the history finds none: the hold goes back and the piece waits sweep_retry") {
    Rig r(make_config());
    r.pool("0.1", "0.05");
    r.wait(3250);
    const auto in = r.intents();
    REQUIRE(in.size() == 1);
    r.answer(TreasuryEventKind::Sent, in[0].seq, TransferState::Unknown);
    r.wait(61'000);
    CHECK(r.stats().late == 1);
    r.answer(TreasuryEventKind::Failed, in[0].seq, TransferState::Failed);
    CHECK(r.free(kB) == nt("0.05"));
    CHECK(r.stats().in_flight == 0);
    r.wait(29'000);
    CHECK(r.intents().empty());
    r.wait(1'500);
    CHECK(r.intents().size() == 1);
  }
  SUBCASE("then the history says done") {
    Rig r(make_config());
    r.pool("0.1", "0.05");
    r.wait(3250);
    const auto in = r.intents();
    REQUIRE(in.size() == 1);
    r.answer(TreasuryEventKind::Sent, in[0].seq, TransferState::Unknown);
    r.answer(TreasuryEventKind::Done, in[0].seq, TransferState::Done);
    CHECK(r.stats().done == 1);
    CHECK(r.total(kB) == Notional{});
    CHECK(r.total(kA) == nt("0.15"));
    CHECK(r.intents().empty());  // the worker resolved it itself
  }
}

TEST_CASE("core.engine sweep: refusals for want of balance block an asset, its pieces trade off") {
  Rig r(make_config());
  r.pool("0.1", "0.05");
  for (int k = 0; k < 3; ++k) {
    r.wait(k == 0 ? 3250 : 30'250);
    const auto in = r.intents();
    REQUIRE(in.size() == 1);
    r.answer(TreasuryEventKind::Failed, in[0].seq, TransferState::Failed, -9000, 400);
  }
  CHECK(r.engine->lane(kA)->blocked("BTC"));
  r.wait(60'000);
  CHECK(r.intents().empty());
  // BTC is not swept: its sell goes to the piece first, as home placement routes it.
  REQUIRE(r.sell("0.05"));
  CHECK(r.transport.news().back()->hdr.venue == kB);
}

TEST_CASE("core.engine sweep: the breaker pauses the sweep and the routing falls back") {
  Rig r(make_config());
  r.pool("0.1", "0.05");
  r.snapshot(kC, "0", "0.5", "1000");
  r.wait(3250);
  for (const LaneIntent& in : r.intents())
    r.answer(TreasuryEventKind::Failed,
             in.seq,
             TransferState::Failed,
             -1003,
             429,
             "0",
             in.from,
             in.asset.view() == "BTC" ? "BTC" : "ETH");
  CHECK(r.stats().trips == 1);
  CHECK_FALSE(r.engine->sweeping(kA));
  REQUIRE(r.sell("0.05"));
  CHECK(r.transport.news().back()->hdr.venue == kB);  // pieces first again
  r.wait(60'000);
  CHECK(r.intents().empty());
  // It resumes by itself after breaker_s.
  r.wait(900'000);
  CHECK(r.engine->sweeping(kA));
  // The operator turns it off and on.
  r.sweep(kSweepOff);
  CHECK_FALSE(r.engine->sweeping(kA));
  r.wait(1000);
  CHECK_FALSE(r.engine->sweeping(kA));
  r.sweep(kSweepOn);
  r.wait(250);
  CHECK(r.engine->sweeping(kA));
}

TEST_CASE("core.engine sweep: a sweeping pool sells from the home and sizes against it") {
  Rig r(make_config());
  r.pool("0.1", "0.05");
  auto& ctx = r.engine->context();
  // The piece exists, but the sell goes home.
  CHECK(ctx.balance_room(kBtc, Side::Sell, px("51000")) == qt("0.1"));
  REQUIRE(r.sell("0.08"));
  CHECK(r.transport.news().back()->hdr.venue == kA);
  // A sell the strategy names an account for goes there, as before.
  NewOrderRequest named = NewOrderRequest::limit(kBtc, Side::Sell, px("51000"), qt("0.01"));
  named.account = kB;
  REQUIRE(r.engine->send_order(named));
  CHECK(r.transport.news().back()->hdr.venue == kB);
}

TEST_CASE("core.engine sweep: the planner counts the orders the home carries") {
  // Home placement: every order. Balance placement with a sweep: the spot sells only, since the
  // buys go wherever there is room.
  for (const Placement placement : {Placement::Home, Placement::Balance}) {
    EngineConfig cfg = make_config();
    cfg.homes.pool[kA.value].placement = placement;
    Rig r(cfg);
    r.pool("0.1", "0");
    REQUIRE(r.sell("0.05"));
    REQUIRE(
        r.engine->send_order(NewOrderRequest::limit(kBtc, Side::Buy, px("49000"), qt("0.001"))));
    PoolLoad load;
    r.engine->pool_load(kA, load);
    REQUIRE(load.count == 2);
    CHECK(load.instrument[0] == kBtc);
    CHECK(load.orders[0] == (placement == Placement::Home ? 2U : 1U));
  }
}

TEST_CASE("core.engine sweep: a piece's resting sell moves home at its next reprice") {
  Rig r(make_config());
  r.pool("0.1", "0.05");
  // Paused: the routing is home placement's, so the quoted ask goes to the piece.
  r.sweep(kSweepPause);
  DesiredQuotes q;
  REQUIRE(q.asks.push_back(Level{px("51000"), qt("0.05")}));
  REQUIRE(r.engine->context().set_quotes(kBtc, q));
  REQUIRE(r.transport.news().size() == 1);
  const OutNewOrderMsg first = *r.transport.news().back();
  CHECK(first.hdr.venue == kB);
  OrderAckMsg ack{};
  init_header(ack, EventType::OrderAck, kBtc, kB);
  ack.cl_ord_id = first.cl_ord_id;
  ack.venue_order_id.assign("v1");
  r.push(ack);
  // Sweeping again: the reprice is a cancel, and the new order goes home once it is acknowledged.
  r.sweep(kSweepOn);
  r.wait(1000);
  CHECK(r.engine->sweeping(kA));
  q.asks[0] = Level{px("51100"), qt("0.05")};
  REQUIRE(r.engine->context().set_quotes(kBtc, q));
  CHECK(r.transport.count(EventType::OutReplace) == 0);
  REQUIRE(r.transport.count(EventType::OutCancel) == 1);
  OrderCancelAckMsg done{};
  init_header(done, EventType::OrderCancelAck, kBtc, kB);
  done.cl_ord_id = first.cl_ord_id;
  r.push(done);
  REQUIRE(r.engine->context().set_quotes(kBtc, q));
  REQUIRE(r.transport.news().size() == 2);
  CHECK(r.transport.news().back()->hdr.venue == kA);
  CHECK(r.transport.news().back()->price == px("51100"));
}

TEST_CASE("core.engine sweep: a restart holds again what an earlier process left in flight") {
  Rig r(make_config());
  r.pool("0.1", "0.05");
  r.wait(250);
  r.answer(TreasuryEventKind::Reserve, 777, TransferState::Unknown, 0, 0, "0.05");
  CHECK(r.stats().in_flight == 1);
  CHECK(r.free(kB) == Notional{});
  r.wait(5000);
  CHECK(r.intents().empty());  // BTC is in flight
  r.answer(TreasuryEventKind::Done, 777, TransferState::Done);
  CHECK(r.stats().in_flight == 0);
  CHECK(r.total(kA) == nt("0.15"));
}

TEST_CASE("core.engine sweep: the dry run journals what it would send, holds and routes nothing") {
  Rig r(make_config(true));
  r.pool("0.1", "0.05");
  CHECK_FALSE(r.engine->sweeping(kA));
  r.wait(3250);
  CHECK(r.intents().empty());
  CHECK(r.stats().planned == 1);
  CHECK(r.free(kB) == nt("0.05"));
  // Not planned again while the piece is unchanged; again once it changes.
  r.wait(10'000);
  CHECK(r.stats().planned == 1);
  r.snapshot(kB, "0.06", "0", "1000");
  r.wait(3250);
  CHECK(r.stats().planned == 2);
  // Routing is home placement's: the piece's sell goes there.
  REQUIRE(r.sell("0.06"));
  CHECK(r.transport.news().back()->hdr.venue == kB);
  const auto recs = r.lane_records();
  REQUIRE(recs.size() == 2);
  CHECK(recs[0] == std::pair{TreasuryEventKind::Planned, true});
}

TEST_CASE("core.engine sweep: the budget caps the transfers, dust waits, the operator sweeps now") {
  EngineConfig cfg = make_config();
  cfg.homes.lane[kA.value].per_minute = 1;
  Rig r(cfg);
  r.snapshot(kA, "0.1", "1", "1000");
  r.snapshot(kB, "0.05", "0", "1000");
  r.snapshot(kC, "0", "0.5", "1000");
  r.wait(3250);
  auto in = r.intents();
  REQUIRE(in.size() == 1);  // a minute's budget of one: the larger piece
  CHECK(in[0].asset.view() == "BTC");
  r.flow(kB, "BTC", "-0.05");
  r.flow(kA, "BTC", "0.05");
  r.wait(30'000);
  CHECK(r.intents().empty());
  r.wait(30'000);
  in = r.intents();
  REQUIRE(in.size() == 1);
  CHECK(in[0].asset.view() == "ETH");
  r.flow(kC, "ETH", "-0.5");
  r.flow(kA, "ETH", "0.5");
  // Dust on a home with nothing to sell it with waits sweep_dust; the operator moves it at once.
  r.snapshot(kA, "0", "1.5", "1000");
  r.snapshot(kB, "0.00001", "0", "1000");
  r.wait(70'000);
  CHECK(r.intents().empty());
  r.sweep(kSweepNow, kBtc);
  r.wait(250);
  in = r.intents();
  REQUIRE(in.size() == 1);
  CHECK(in[0].amount == nt("0.00001").raw);
}
