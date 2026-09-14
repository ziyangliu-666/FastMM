// Tick-to-order inside the simulator (5.9 / 5.12): Engine<BasicMM, SimClock, SimTransport,
// InlineFeed> fed pre-generated BookDeltaMsgs that alternately move the touch up and down
// by two ticks, so every event makes BasicMM requote both quotes.
//
//   BM_TickToOrder_Sim   one delta: bytes into the feed -> Engine::step() -> Out* serialized
//                        into SimTransport (latency model, scheduler, outbound SHA-256).
//                        Per-event rdtsc deltas go into a LogLinearHistogram; counters p50 /
//                        p99 (ns) cover the events that produced orders. Venue processing and
//                        ack delivery run outside the timed region, until nothing is in flight and
//                        both quotes are working, so each timed event starts from a settled state
//                        and sends a Cancel per side. The benchmark fails when fewer than
//                        kMinOrderEventsPct of the events sent orders.
//   BM_EngineStep_Sim    Engine::step() throughput on a 32-event batch (items_per_second), from
//                        the same settled state. Only the batch's first event can requote: its
//                        orders are still in flight for the other 31. Counter out_msgs_per_step.
//
// Why settle fully: QuoteManager never touches a Pending* order, it records the target and applies
// it on the ack. A tick that meets an order still in flight therefore sends nothing, and with a
// single 1 ms venue pass per event every timed tick met one (order_events_pct 0).
#include "fastmm/core/engine.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/basic_mm.hpp"

#include <benchmark/benchmark.h>

#include <cstring>
#include <memory>
#include <vector>

using namespace fastmm;
using namespace fastmm::sim;

namespace {

using SimEngine = Engine<BasicMM, SimClock, SimTransport, InlineFeed>;

// Below this share of events with outbound orders the timings describe something else.
constexpr double kMinOrderEventsPct = 50.0;

const TscClock& tsc() {
  static const TscClock c = [] {
    TscClock t;
    t.calibrate(milliseconds(20));
    return t;
  }();
  return c;
}

// One message of a pre-generated tape, stored in a 64-aligned buffer of size_for() bytes.
struct TapeMsg {
  alignas(64) std::byte bytes[BookDeltaMsg::size_for(16, 16)];
  [[nodiscard]] const EventHeader& hdr() const noexcept {
    return *reinterpret_cast<const EventHeader*>(bytes);
  }
};

class Rig {
 public:
  Rig() {
    Instrument inst{};
    inst.symbol = "BTCUSDT";
    inst.flags = Instrument::kEnabled;
    inst.tick = Price::from_decimal("0.01").value();
    inst.lot = Qty::from_decimal("0.00001").value();
    inst.min_qty = inst.lot;
    static_cast<void>(table_.add(inst));
    tick_ = inst.tick.raw;
    SimTransportConfig tc;
    tc.order_out = LatencyParams{microseconds(100), Duration{}};
    tc.ack_in = LatencyParams{microseconds(100), Duration{}};
    tc.seed = 1;
    clock_ = std::make_unique<SimClock>(Timestamp{seconds(1'700'000'000).ns});
    transport_ = std::make_unique<SimTransport>(*clock_, table_, tc);
    feed_ = std::make_unique<InlineFeed>(1U << 22);
    // Half spread 0.005 bps = 3 ticks at 60000: quotes m-3 / m+3, then m-1 / m+6 after the up
    // tick. Cancel-then-new sends each side's New when its cancel ack arrives, while the other side
    // may still rest at its old price; at 0 bps (one tick wide) that New crossed the old opposite
    // quote and risk rejected it (self-trade prevention), leaving the slot empty.
    static_cast<void>(strategy_.configure({{"half_spread_bps", "0.005"},
                                           {"skew_bps_per_unit", "0"},
                                           {"quote_qty", "0.002"},
                                           {"max_inventory", "0"},
                                           {"requote_threshold_ticks", "1"},
                                           {"pull_on_stale_ms", "0"}}));
    EngineConfig ec;
    ec.max_events_per_step = 64;
    ec.quotes.min_requote_interval = Duration{};
    engine_ = std::make_unique<SimEngine>(ec, table_, *clock_, *transport_, *feed_, strategy_);
    engine_->warm_up();
    engine_->start();
    build_tape();
    push(snapshot_);
    engine_->step();
    settled_ = settle();
  }

  // Venue side + acks, untimed: 1 ms passes of virtual time that deliver orders to the venue and
  // acks to the engine, repeated until nothing is in flight (a cancel ack sends the New of a
  // cancel-then-new, which needs another pass). True when the venue is quiet and both quotes are
  // working, so the next tick can requote them.
  [[nodiscard]] bool settle() noexcept {
    for (int pass = 0; pass < kMaxSettlePasses; ++pass) {
      clock_->advance(milliseconds(1));
      const Timestamp now = clock_->now();
      while (transport_->next_order_arrival() <= now) transport_->process_order_arrival();
      while (transport_->next_inbound_ts() <= now) {
        static_cast<void>(transport_->deliver_next_inbound(*feed_));
        engine_->step();
      }
      if (transport_->next_order_arrival() == Timestamp::max() && !transport_->inbound_pending())
        return quotes_working();
    }
    return false;
  }
  void push(const TapeMsg& m) noexcept { static_cast<void>(feed_->push(m.hdr())); }
  [[nodiscard]] const TapeMsg& tick_msg(std::size_t i) const noexcept {
    return tape_[i % tape_.size()];
  }
  [[nodiscard]] SimEngine& engine() noexcept { return *engine_; }
  [[nodiscard]] SimTransport& transport() noexcept { return *transport_; }
  [[nodiscard]] bool settled() const noexcept { return settled_; }

 private:
  static constexpr int kMaxSettlePasses = 16;

  [[nodiscard]] bool quotes_working() const noexcept {
    const Oms& oms = engine_->oms();
    for (const Side s : {Side::Buy, Side::Sell}) {
      const Handle<Order> h = engine_->quote_manager().slot_handle(InstrumentId{0}, s, 0);
      if (!h.valid() || !oms.is_live(h) || !oms.get(h).is_working()) return false;
    }
    return true;
  }
  void level_msg(TapeMsg& m,
                 bool snapshot,
                 std::initializer_list<Level> bids,
                 std::initializer_list<Level> asks) {
    auto* d = reinterpret_cast<BookDeltaMsg*>(m.bytes);
    const auto nb = static_cast<std::uint32_t>(bids.size());
    const auto na = static_cast<std::uint32_t>(asks.size());
    init_header(*d,
                snapshot ? EventType::BookSnapshot : EventType::BookDelta,
                InstrumentId{0},
                VenueId{0},
                BookDeltaMsg::size_for(nb, na));
    if (snapshot) d->hdr.flags |= EventHeader::kSnapshot;
    d->bid_count = nb;
    d->ask_count = na;
    std::size_t k = 0;
    for (const Level& l : bids) d->levels()[k++] = l;
    for (const Level& l : asks) d->levels()[k++] = l;
  }
  Price p(std::int64_t ticks) const noexcept { return Price::from_raw(ticks * tick_); }
  void build_tape() {
    const Qty q = Qty::from_decimal("0.01").value();
    const std::int64_t m = 6'000'000;  // 60000.00 in ticks
    level_msg(snapshot_,
              true,
              {{p(m - 1), q}, {p(m - 2), q}, {p(m - 3), q}, {p(m - 4), q}},
              {{p(m + 1), q}, {p(m + 2), q}, {p(m + 3), q}, {p(m + 4), q}});
    tape_.resize(2);
    // up: new bid at m+2 lifts both asks at or below it; mid 60000.00 -> 60000.025
    level_msg(tape_[0], false, {{p(m + 2), q}}, {{p(m + 1), Qty{}}, {p(m + 2), Qty{}}});
    // down: restore
    level_msg(tape_[1], false, {{p(m + 2), Qty{}}}, {{p(m + 1), q}, {p(m + 2), q}});
  }

  InstrumentTable table_;
  std::int64_t tick_ = 0;
  std::unique_ptr<SimClock> clock_;
  std::unique_ptr<SimTransport> transport_;
  std::unique_ptr<InlineFeed> feed_;
  BasicMM strategy_;
  std::unique_ptr<SimEngine> engine_;
  TapeMsg snapshot_{};
  std::vector<TapeMsg> tape_;
  bool settled_ = false;
};

}  // namespace

static void BM_TickToOrder_Sim(benchmark::State& state) {
  auto rig = std::make_unique<Rig>();
  if (!rig->settled()) {
    state.SkipWithError("rig did not settle after the snapshot");
    return;
  }
  LogLinearHistogram with_orders;
  LogLinearHistogram all;
  std::size_t i = 0;
  bool settled = true;
  for (auto _ : state) {
    const std::uint64_t sent_before = rig->transport().outbound_hash().count();
    const TapeMsg& m = rig->tick_msg(i++);
    const Cycles t0 = rdtsc();
    rig->push(m);
    rig->engine().step();
    const Cycles t1 = rdtsc();
    const auto ns = static_cast<std::uint64_t>(tsc().cycles_to_ns(t1 - t0));
    all.record(ns);
    if (rig->transport().outbound_hash().count() != sent_before) with_orders.record(ns);
    state.PauseTiming();
    settled = rig->settle() && settled;
    state.ResumeTiming();
  }
  const double order_events_pct =
      all.count() == 0
          ? 0.0
          : 100.0 * static_cast<double>(with_orders.count()) / static_cast<double>(all.count());
  state.counters["p50"] = static_cast<double>(with_orders.percentile(0.50));
  state.counters["p99"] = static_cast<double>(with_orders.percentile(0.99));
  state.counters["all_p50"] = static_cast<double>(all.percentile(0.50));
  state.counters["order_events_pct"] = order_events_pct;
  state.counters["kills"] = static_cast<double>(rig->engine().stats().kills);
  if (!settled) {
    state.SkipWithError("venue did not settle between timed events");
  } else if (order_events_pct < kMinOrderEventsPct) {
    state.SkipWithError("fewer than 50% of the timed events sent orders; p50/p99 are meaningless");
  }
}
BENCHMARK(BM_TickToOrder_Sim);

static void BM_EngineStep_Sim(benchmark::State& state) {
  static constexpr std::int64_t kBatch = 32;
  auto rig = std::make_unique<Rig>();
  if (!rig->settled()) {
    state.SkipWithError("rig did not settle after the snapshot");
    return;
  }
  std::size_t i = 0;
  bool settled = true;
  std::uint64_t out_msgs = 0;
  for (auto _ : state) {
    state.PauseTiming();
    for (std::int64_t k = 0; k < kBatch; ++k) rig->push(rig->tick_msg(i++));
    const std::uint64_t sent_before = rig->transport().outbound_hash().count();
    state.ResumeTiming();
    benchmark::DoNotOptimize(rig->engine().step());
    state.PauseTiming();
    out_msgs += rig->transport().outbound_hash().count() - sent_before;
    settled = rig->settle() && settled;
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
  state.counters["out_msgs_per_step"] =
      state.iterations() == 0
          ? 0.0
          : static_cast<double>(out_msgs) / static_cast<double>(state.iterations());
  if (!settled) state.SkipWithError("venue did not settle between steps");
}
BENCHMARK(BM_EngineStep_Sim);
