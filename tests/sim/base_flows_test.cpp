// [engine] base_flows (core/engine.hpp flow_tick): a change of a spot instrument's base asset that
// no fill and no transfer of the engine's made is found once it has held base_flows_settle.
// "report" logs and journals it once; "adjust" books it to the position at the mid, which leaves
// the PnL where it was. A fill that reaches the engine after the balance that has it is no flow; a
// fill the engine missed is booked as one and taken back once its execution arrives. An asset that
// is also a quote is not watched; nothing is looked at while a venue reconciles. A replay of the
// journal finds the same flows.
#include "test_support.hpp"

#include "fastmm/core/engine.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/sim/journal_feed.hpp"
#include "fastmm/sim/replay_transport.hpp"
#include "fastmm/sim/sim_driver.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

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
constexpr VenueId kA{0};  // the primary
constexpr VenueId kB{1};

InstrumentTable make_table(bool eth_btc = false) {
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
  REQUIRE(t.add(s));
  if (eth_btc) {
    Instrument e = s;
    e.symbol = "ETHBTC";
    e.base = "ETH";
    e.quote = "BTC";
    e.tick = px("0.00001");
    REQUIRE(t.add(e));
  }
  return t;
}

struct Transport {
  bool send(const EventHeader&) noexcept { return true; }
  std::size_t send(std::span<const EventHeader* const> batch) noexcept { return batch.size(); }
  [[nodiscard]] bool supports_replace(VenueId) const noexcept { return true; }
};

struct Mute {
  static std::string_view name() noexcept { return "mute"; }
};

EngineConfig make_config(BaseFlows mode, bool pool = false, bool from_zero = false) {
  EngineConfig cfg;
  cfg.fees = FeeTable::from_bps(0, 0);
  cfg.risk.max_open_orders = 100;
  cfg.base_flows = mode;
  cfg.base_flows_settle = seconds(5);
  cfg.base_flows_from_zero = from_zero;
  if (pool) REQUIRE(cfg.pools.add(kB, kA));
  return cfg;
}

// Sends what the engine needs into an InlineFeed and lets the clock run.
struct Inputs {
  SimClock& clock;
  InlineFeed& feed;
  std::function<void()> drain;
  std::uint32_t execs = 0;

  template <class M>
  void push(M& m) {
    m.hdr.recv_ts = clock.now();
    REQUIRE(feed.push(m.hdr));
    drain();
  }
  // Time passes in quarter seconds, the timers firing on the way.
  void wait(std::int64_t ms) {
    for (std::int64_t t = 0; t < ms; t += 250) {
      clock.advance(milliseconds(250));
      drain();
    }
  }
  void book(const char* bid, const char* ask, InstrumentId id = kBtc) {
    std::byte* p = feed.reserve(BookDeltaMsg::size_for(1, 1));
    REQUIRE(p != nullptr);
    auto* d = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*d, EventType::BookSnapshot, id, kA, BookDeltaMsg::size_for(1, 1));
    d->hdr.flags |= EventHeader::kSnapshot;
    d->hdr.recv_ts = clock.now();
    d->bid_count = d->ask_count = 1;
    d->levels()[0] = Level{px(bid), qt("5")};
    d->levels()[1] = Level{px(ask), qt("5")};
    feed.commit();
    drain();
  }
  // The store's position, as fastmm-live restores it.
  void restore(const char* qty, const char* avg) {
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, kBtc, kA);
    m.kind = ReconcileMsg::Kind::Position;
    m.position_qty = qt(qty);
    m.avg_px = px(avg);
    push(m);
  }
  // One asset's total on account `v` (Binance outboundAccountPosition), stamped now.
  void report(VenueId v, const char* asset, const char* total) {
    BalanceMsg m{};
    init_header(m, EventType::Balance, InstrumentId::invalid(), v);
    m.asset.assign(asset);
    m.free = nt(total);
    m.total = m.free;
    m.equity = m.total;
    m.hdr.exch_ts = clock.now();
    push(m);
  }
  // An execution of an order the engine does not know (a manual trade, a missed fill), stamped
  // `ago` before now.
  void fill(VenueId v, Side side, const char* qty, const char* price, Duration ago = Duration{}) {
    OrderFillMsg f{};
    init_header(f, EventType::OrderFill, kBtc, v);
    f.exec_id.assign(std::to_string(++execs));
    f.side = side;
    f.qty = qt(qty);
    f.price = px(price);
    f.liquidity = Liquidity::Maker;
    f.fee_asset = FeeAsset::Quote;
    f.hdr.exch_ts = Timestamp{clock.now().ns - ago.ns};
    push(f);
  }
  void reconcile(ReconcileMsg::Kind kind, VenueId v = kA) {
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, InstrumentId::invalid(), v);
    m.kind = kind;
    m.flags = ReconcileMsg::kExecutionsExact;
    push(m);
  }
};

struct Rig {
  using E = Engine<Mute, SimClock, Transport, InlineFeed>;
  InstrumentTable table;
  SimClock clock{Timestamp{seconds(100'000).ns}};
  Transport transport;
  InlineFeed feed{1 << 20};
  Mute strategy;
  MsgRing journal{1U << 22};
  std::unique_ptr<E> engine;
  Inputs in{clock, feed, [this] { drain(); }};

  explicit Rig(const EngineConfig& cfg, bool eth_btc = false) : table(make_table(eth_btc)) {
    engine = std::make_unique<E>(cfg, table, clock, transport, feed, strategy, &journal);
    engine->warm_up();
    engine->start();
    in.book("49990", "50010");
  }
  void drain() {
    while (engine->step() > 0) {
    }
  }
  [[nodiscard]] Qty position() const { return engine->position(kBtc).qty; }
  [[nodiscard]] const EngineStats& stats() const { return engine->stats(); }
  // The engine's BaseFlow records in the journal.
  std::vector<ControlBaseFlowMsg> records() {
    std::vector<ControlBaseFlowMsg> out;
    while (const std::byte* p = journal.try_peek()) {
      const auto* h = reinterpret_cast<const EventHeader*>(p);
      if (h->type == EventType::Control && h->len >= sizeof(ControlBaseFlowMsg)) {
        ControlBaseFlowMsg m;
        std::memcpy(&m, p, sizeof m);
        if (m.command == ControlCommand::BaseFlow) out.push_back(m);
      }
      journal.release();
    }
    return out;
  }
};

}  // namespace

TEST_CASE("sim.base_flows: off looks at nothing") {
  Rig r(make_config(BaseFlows::Off));
  r.in.report(kA, "BTC", "1");
  r.in.wait(10'000);
  r.in.report(kA, "BTC", "1.0175");
  r.in.wait(60'000);
  CHECK(r.stats().base_flows == 0);
  CHECK(r.position() == Qty{});
  CHECK(r.records().empty());
}

TEST_CASE("sim.base_flows: report says a dividend once and leaves the position") {
  Rig r(make_config(BaseFlows::Report));
  // The account holds a coin the session has no position in: that offset is no flow.
  r.in.report(kA, "BTC", "1");
  r.in.wait(10'000);
  // Fills move the balance and the position alike.
  r.in.fill(kA, Side::Buy, "0.1", "50000");
  r.in.report(kA, "BTC", "1.1");
  r.in.wait(10'000);
  CHECK(r.stats().base_flows == 0);
  // A dividend credited as tokens: the balance alone moves.
  r.in.report(kA, "BTC", "1.10175");
  r.in.wait(4'000);
  CHECK(r.stats().base_flows == 0);  // not yet held base_flows_settle
  r.in.wait(3'000);
  CHECK(r.stats().base_flows == 1);
  CHECK(r.stats().base_flows_booked == 0);
  CHECK(r.position() == qt("0.1"));
  r.in.wait(60'000);
  CHECK(r.stats().base_flows == 1);  // said once
  const auto recs = r.records();
  REQUIRE(recs.size() == 1);
  CHECK((recs[0].hdr.flags & EventHeader::kSynthetic) != 0);
  CHECK(recs[0].hdr.instrument == kBtc);
  CHECK(recs[0].asset.view() == "BTC");
  CHECK(static_cast<std::int64_t>(recs[0].arg) == qt("0.00175").raw);
  CHECK(recs[0].balance == qt("1.10175").raw);
  CHECK(recs[0].position == qt("0.1").raw);
  CHECK((recs[0].flags & ControlBaseFlowMsg::kBooked) == 0);
}

TEST_CASE("sim.base_flows: adjust books a pool's credit to the position at the mid") {
  Rig r(make_config(BaseFlows::Adjust, /*pool=*/true, /*from_zero=*/true));
  r.in.restore("7.854", "40000");
  r.in.report(kA, "BTC", "5.854");
  r.in.report(kB, "BTC", "2");
  r.in.report(kA, "USDT", "1000");
  r.in.wait(10'000);
  CHECK(r.stats().base_flows == 0);  // the accounts hold what the position says
  const Notional pnl = r.engine->net_pnl();
  // The credit lands on the second account.
  r.in.report(kB, "BTC", "2.01752845");
  r.in.wait(7'000);
  CHECK(r.stats().base_flows == 1);
  CHECK(r.stats().base_flows_booked == 1);
  CHECK(r.position() == qt("7.87152845"));
  CHECK(r.engine->position(kBtc).fills == 0);  // not a trade
  // Valued at the mid: the PnL is where it was, to the rounding of the average price.
  const std::int64_t moved = r.engine->net_pnl().raw - pnl.raw;
  CHECK(moved > -nt("0.0001").raw);
  CHECK(moved < nt("0.0001").raw);
  r.in.wait(60'000);
  CHECK(r.stats().base_flows == 1);
  const auto recs = r.records();
  REQUIRE(recs.size() == 1);
  CHECK((recs[0].flags & ControlBaseFlowMsg::kBooked) != 0);
  CHECK(recs[0].hdr.venue == kA);
  CHECK(recs[0].balance == qt("7.87152845").raw);
  CHECK(recs[0].position == qt("7.854").raw);
}

TEST_CASE("sim.base_flows: adjust from zero corrects an offset found at the start") {
  Rig r(make_config(BaseFlows::Adjust, false, /*from_zero=*/true));
  r.in.restore("0.5", "40000");
  r.in.report(kA, "BTC", "0.5004");
  r.in.wait(7'000);
  CHECK(r.stats().base_flows_booked == 1);
  CHECK(r.position() == qt("0.5004"));

  // From the session's first value instead, the same offset stays.
  Rig s(make_config(BaseFlows::Adjust));
  s.in.restore("0.5", "40000");
  s.in.report(kA, "BTC", "0.5004");
  s.in.wait(30'000);
  CHECK(s.stats().base_flows == 0);
  CHECK(s.position() == qt("0.5"));
}

TEST_CASE("sim.base_flows: a fill that arrives after the balance that has it is no flow") {
  Rig r(make_config(BaseFlows::Adjust, false, true));
  r.in.restore("1", "40000");
  r.in.report(kA, "BTC", "1");
  r.in.wait(10'000);
  // The account's update comes first (it has the fill), the execution two seconds later.
  r.in.report(kA, "BTC", "1.2");
  r.in.wait(2'000);
  r.in.fill(kA, Side::Buy, "0.2", "50000", seconds(3));
  r.in.wait(30'000);
  CHECK(r.stats().base_flows == 0);
  CHECK(r.position() == qt("1.2"));
}

TEST_CASE("sim.base_flows: a missed fill is booked, then taken back when it arrives") {
  Rig r(make_config(BaseFlows::Adjust, false, true));
  r.in.restore("1", "40000");
  r.in.report(kA, "BTC", "1");
  r.in.wait(10'000);
  r.in.report(kA, "BTC", "1.2");
  r.in.wait(7'000);
  CHECK(r.stats().base_flows_booked == 1);
  CHECK(r.position() == qt("1.2"));
  // The venue's execution history finds it a minute later.
  r.in.wait(60'000);
  r.in.fill(kA, Side::Buy, "0.2", "50000", seconds(70));
  CHECK(r.position() == qt("1.4"));
  r.in.wait(7'000);
  CHECK(r.stats().base_flows_booked == 2);
  CHECK(r.position() == qt("1.2"));
}

TEST_CASE("sim.base_flows: no look while a venue reconciles") {
  Rig r(make_config(BaseFlows::Adjust, false, true));
  r.in.restore("1", "40000");
  r.in.report(kA, "BTC", "1");
  r.in.wait(10'000);
  r.in.reconcile(ReconcileMsg::Kind::Begin);
  r.in.report(kA, "BTC", "1.2");
  r.in.wait(30'000);
  CHECK(r.stats().base_flows == 0);
  r.in.fill(kA, Side::Buy, "0.2", "50000", seconds(31));
  r.in.reconcile(ReconcileMsg::Kind::End);
  r.in.wait(30'000);
  CHECK(r.stats().base_flows == 0);
  CHECK(r.position() == qt("1.2"));
}

TEST_CASE("sim.base_flows: an asset that is also a quote is not watched") {
  Rig r(make_config(BaseFlows::Adjust, false, true), /*eth_btc=*/true);
  r.in.report(kA, "BTC", "1");
  r.in.report(kA, "ETH", "0");
  r.in.wait(30'000);
  CHECK(r.stats().base_flows == 0);
  CHECK(r.position() == Qty{});
}

TEST_CASE("sim.base_flows: a replay of the journal finds the same flows") {
  const auto path = (tmp_dir() / "base_flows.fmj").string();
  const InstrumentTable instruments = make_table();
  const EngineConfig cfg = make_config(BaseFlows::Adjust, false, true);
  Qty recorded_position;
  std::uint64_t recorded_booked = 0;
  {
    MsgRing ring(1 << 22);
    JournalSessionInfo info;
    info.strategy = "mute";
    info.instruments = &instruments;
    info.has_session = true;
    info.session_epoch = 1;
    JournalFileWriter fw(ring, path, info);
    REQUIRE(fw.ok());
    SimClock clock(Timestamp{1'700'000'000'000'000'000LL});
    Transport transport;
    InlineFeed feed(1 << 20);
    Mute strategy;
    Engine<Mute, SimClock, Transport, InlineFeed> engine(
        cfg, instruments, clock, transport, feed, strategy, &ring);
    engine.warm_up();
    engine.start();
    Inputs in{clock, feed, [&] {
                while (engine.step() > 0) {
                }
                fw.drain_once();
              }};
    in.book("49990", "50010");
    in.restore("1", "40000");
    in.report(kA, "BTC", "1");
    in.wait(10'000);
    in.report(kA, "BTC", "1.2");
    in.wait(10'000);
    in.fill(kA, Side::Buy, "0.2", "50000", seconds(10));
    in.wait(10'000);
    in.report(kA, "BTC", "1.20003");
    in.wait(10'000);
    engine.finish();
    fw.drain_once();
    fw.stop();
    recorded_position = engine.position(kBtc).qty;
    recorded_booked = engine.stats().base_flows_booked;
  }
  CHECK(recorded_booked == 3);
  CHECK(recorded_position == qt("1.20003"));

  JournalReader reader;
  REQUIRE(reader.open(path));
  sim::ReplayTransport rt;
  sim::JournalFeed jf(reader);
  SimClock rclock(Timestamp{reader.header().start_ts_ns});
  Mute rstrategy;
  Engine<Mute, SimClock, sim::ReplayTransport, sim::JournalFeed> replay(
      cfg, instruments, rclock, rt, jf, rstrategy);
  sim::ReplayDriver driver(rclock, jf, sim::EngineHooks::for_engine(replay));
  driver.run_all();
  driver.finish();
  CHECK(replay.stats().base_flows_booked == recorded_booked);
  CHECK(replay.position(kBtc).qty == recorded_position);
}
