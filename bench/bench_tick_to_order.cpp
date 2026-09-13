// Tick-to-order inside the simulator (5.9 / 5.12): Engine<BasicMM, SimClock, SimTransport,
// InlineFeed> fed pre-generated BookDeltaMsgs that alternately move the touch up and down
// by two ticks, so every event makes BasicMM requote.
//
//   BM_TickToOrder_Sim   one delta: bytes into the feed -> Engine::step() -> Out* serialized
//                        into SimTransport (latency model, scheduler, outbound SHA-256).
//                        Per-event rdtsc deltas go into a LogLinearHistogram; counters p50 /
//                        p99 (ns) cover the events that produced orders. Venue processing and
//                        ack delivery run outside the timed region.
//   BM_EngineStep_Sim    Engine::step() throughput on a 32-event batch (items_per_second).
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
    static_cast<void>(strategy_.configure({{"half_spread_bps", "0.003"},
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
    settle();
  }

  // Venue side + acks, untimed: advance virtual time, let orders reach the venue and the
  // acks reach the engine so QuoteManager can requote on the next tick.
  void settle() noexcept {
    clock_->advance(milliseconds(1));
    const Timestamp now = clock_->now();
    while (transport_->next_order_arrival() <= now) transport_->process_order_arrival();
    while (transport_->next_inbound_ts() <= now) {
      static_cast<void>(transport_->deliver_next_inbound(*feed_));
      engine_->step();
    }
  }
  void push(const TapeMsg& m) noexcept { static_cast<void>(feed_->push(m.hdr())); }
  [[nodiscard]] const TapeMsg& tick_msg(std::size_t i) const noexcept {
    return tape_[i % tape_.size()];
  }
  [[nodiscard]] SimEngine& engine() noexcept { return *engine_; }
  [[nodiscard]] SimTransport& transport() noexcept { return *transport_; }

 private:
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
};

}  // namespace

static void BM_TickToOrder_Sim(benchmark::State& state) {
  auto rig = std::make_unique<Rig>();
  LogLinearHistogram with_orders;
  LogLinearHistogram all;
  std::size_t i = 0;
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
    rig->settle();
    state.ResumeTiming();
  }
  state.counters["p50"] = static_cast<double>(with_orders.percentile(0.50));
  state.counters["p99"] = static_cast<double>(with_orders.percentile(0.99));
  state.counters["all_p50"] = static_cast<double>(all.percentile(0.50));
  state.counters["order_events_pct"] =
      all.count() == 0
          ? 0.0
          : 100.0 * static_cast<double>(with_orders.count()) / static_cast<double>(all.count());
  state.counters["kills"] = static_cast<double>(rig->engine().stats().kills);
}
BENCHMARK(BM_TickToOrder_Sim);

static void BM_EngineStep_Sim(benchmark::State& state) {
  static constexpr std::int64_t kBatch = 32;
  auto rig = std::make_unique<Rig>();
  std::size_t i = 0;
  for (auto _ : state) {
    state.PauseTiming();
    for (std::int64_t k = 0; k < kBatch; ++k) rig->push(rig->tick_msg(i++));
    state.ResumeTiming();
    benchmark::DoNotOptimize(rig->engine().step());
    state.PauseTiming();
    rig->settle();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
}
BENCHMARK(BM_EngineStep_Sim);
