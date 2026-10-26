#pragma once
// SimDriver: the discrete-event loop of a backtest (8.2). Advances the SimClock to the
// earliest pending event among
//
//   venue side   : generator action, historical source event, order arrival at the venue,
//                  market-data flush
//   engine side  : next wire message (ack/fill or market data), next parameter update
//                  (ParamSchedule), next engine timer
//   slow tier    : next run of the slow methods of a Python strategy (SlowHooks)
//
// and performs exactly that event, so venue-side and engine-side state evolve in one
// consistent virtual time. Ties resolve venue-first, then in the fixed order listed above.
// The engine is reached through EngineHooks (plain function pointers) so the driver works
// for any Engine<Strategy, SimClock, SimTransport, InlineFeed> and through the type-erased
// registry path alike.
//
// ReplayDriver replays a journal through Engine<S, SimClock, ReplayTransport, JournalFeed>:
// one journal event per step at its original time; the engine does not poll its timer wheel,
// because the journal already contains the synthetic TimerMsg records that fired, and it fires each
// one there (Engine::kFeedReplaysTimers). With a v2 journal the clock follows the recorded engine
// clock exactly (including backward steps of a recalibrated TscClock), also for start() and
// finish(); a v1 journal only has receive and fire times, which never move the clock backwards.
#include "fastmm/core/journal.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/market_view.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/sim/journal_feed.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/md_source.hpp"
#include "fastmm/sim/param_schedule.hpp"
#include "fastmm/sim/sim_transport.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm::sim {

struct EngineHooks {
  void* ctx = nullptr;
  std::size_t (*step)(void*) = nullptr;
  // step() with `event` taken first, read where it lies (Engine::step_with): a multiverse's runs
  // read the market data they share without a copy into each feed.
  std::size_t (*step_with)(void*, const EventHeader&) = nullptr;
  // Engine::follow_market and md_interest: the engine follows a multiverse's market data, which
  // hands it only the messages it needs (backtest/multiverse.hpp).
  void (*follow_market)(void*, const MarketView&) = nullptr;
  const MdInterest* (*md_interest)(void*) = nullptr;
  Timestamp (*next_timer)(void*) = nullptr;
  void (*warm_up)(void*) = nullptr;
  void (*start)(void*) = nullptr;
  void (*finish)(void*) = nullptr;
  // Optional: true once the engine was asked to stop (ctx.request_stop(), a failed Python
  // strategy). SimDriver ends the run after the step that asked; ReplayDriver ignores it, because a
  // replay always drains the journal (stopping early would change the replay hash).
  bool (*stopped)(void*) = nullptr;

  template <class Engine>
  [[nodiscard]] static EngineHooks for_engine(Engine& e) noexcept {
    EngineHooks h;
    h.ctx = &e;
    h.step = [](void* c) { return static_cast<Engine*>(c)->step(); };
    h.step_with = [](void* c, const EventHeader& m) {
      return static_cast<Engine*>(c)->step_with(m);
    };
    h.follow_market = [](void* c, const MarketView& m) {
      static_cast<Engine*>(c)->follow_market(m);
    };
    h.md_interest = [](void* c) { return &static_cast<Engine*>(c)->md_interest(); };
    h.next_timer = [](void* c) { return static_cast<Engine*>(c)->timers().next_expiry(); };
    h.warm_up = [](void* c) { static_cast<Engine*>(c)->warm_up(); };
    h.start = [](void* c) { static_cast<Engine*>(c)->start(); };
    h.finish = [](void* c) { static_cast<Engine*>(c)->finish(); };
    h.stopped = [](void* c) { return static_cast<Engine*>(c)->stopped(); };
    return h;
  }
  [[nodiscard]] bool valid() const noexcept { return ctx != nullptr && step != nullptr; }
};

// Code that runs at simulated times between engine events: the slow methods of a Python strategy
// in a backtest, and what a backtest does at its own times (backtest_runner.cpp). A message it
// pushes into the engine's feed is taken by an engine step right after the run.
struct SlowHooks {
  void* ctx = nullptr;
  // Runs what is due at `now` (the clock is at `now`) and returns the next time to run
  // (Timestamp::max(): none). A time not after `now` is taken as 1 ns after it.
  Timestamp (*run)(void* ctx, Timestamp now) = nullptr;
  Timestamp first = Timestamp::max();  // the first time to run
};

// Where an event stands in a run's virtual time: its time, then its kind, in the order SimDriver
// takes the kinds of one instant. A multiverse (backtest/multiverse.hpp) steps each run up to the
// key of the next event its runs share and puts that event there, so every run sees the order of
// events it sees alone.
struct SimKey {
  enum Rank : std::uint8_t {
    kGenerator = 0,
    kSource = 1,      // a recorded event at the venue
    kOrder = 2,       // an order message reaching the venue
    kFlush = 3,       // the coupled generator's depth batch
    kInbound = 4,     // an order event or private message reaching the engine
    kMarketData = 5,  // market data reaching the engine
    kParam = 6,
    kTimer = 7,
    kSlow = 8,
    kStop = 9,  // a backtest's equity bar or markout: after everything of its instant
    kEnd = 10,
  };
  Timestamp t;
  std::uint8_t rank = kGenerator;
  constexpr auto operator<=>(const SimKey&) const noexcept = default;
  [[nodiscard]] static constexpr SimKey never() noexcept { return {Timestamp::max(), kEnd}; }
};

// Power of two: SimDriver times 1 in this many market-data steps (SimDriverStats::md_step_ns).
inline constexpr std::uint64_t kMdStepSampleEvery = 16;

struct SimDriverStats {
  std::uint64_t generator_actions = 0;
  std::uint64_t source_events = 0;
  std::uint64_t order_arrivals = 0;
  std::uint64_t flushes = 0;
  std::uint64_t md_delivered = 0;
  std::uint64_t order_events_delivered = 0;
  std::uint64_t timer_steps = 0;
  std::uint64_t param_updates = 0;  // ParamSchedule updates delivered
  std::uint64_t slow_runs = 0;      // SlowHooks::run calls
  std::uint64_t engine_steps = 0;
  std::uint64_t journal_drained = 0;
  // Wall-clock ns per engine step that consumed market data, 1 in kMdStepSampleEvery of them.
  LogLinearHistogram md_step_ns;
};

class SimDriver {
 public:
  SimDriver(SimClock& clock, SimTransport& transport, InlineFeed& feed, const EngineHooks& hooks)
      : clock_(clock), transport_(transport), feed_(feed), hooks_(hooks) {}

  void set_source(MdSource* s) noexcept {
    source_ = s;
    pending_ = nullptr;
    source_done_ = false;
  }
  // Coupled mode: the generator drives the transport's matching engine as account 0 and the
  // aggregator publishes the shared book. `seed_levels` orders per side are placed first.
  void set_generator(MarketGenerator* g, int seed_levels = 20) noexcept {
    generator_ = g;
    seed_levels_ = seed_levels;
  }
  void set_journal_writer(JournalFileWriter* w) noexcept { journal_ = w; }
  // Parameter updates at simulated times.
  void set_param_schedule(ParamSchedule* s) noexcept { params_ = s; }
  // A slow tier at simulated times. Its runs come after the other events of the same time and, like
  // timers, do not keep a run alive.
  void set_slow_hooks(const SlowHooks& h) noexcept {
    slow_ = h;
    slow_next_ = h.run != nullptr ? h.first : Timestamp::max();
  }
  void set_measure_wall_clock(bool v) noexcept { measure_ = v; }

  // on_start; seeds the book and publishes the first snapshot in coupled mode. The venues'
  // accounts send their first balance snapshot, delivered at the first step. A simulation has no
  // first event to keep page faults from, so the engine is not warmed up: a sweep's thousands of
  // engines leave the memory they never use untouched.
  void start() {
    transport_.publish_balances(clock_.now());
    if (generator_ != nullptr) {
      transport_.enable_aggregator(clock_.now());
      generator_->seed_book(transport_.matching_engine(), seed_levels_, clock_.now());
    }
    hooks_.start(hooks_.ctx);
    if (generator_ != nullptr) transport_.flush_md(clock_.now());
    started_ = true;
    poll_stopped();
  }

  // Processes every event with time <= until. Returns false when no event remains or the engine
  // was asked to stop (EngineHooks::stopped).
  bool run_until(Timestamp until) { return run_before(SimKey{until, SimKey::kEnd}); }

  // Processes every event whose key is before `limit`; returns as run_until().
  bool run_before(SimKey limit) {
    if (!started_) start();
    for (;;) {
      if (stopped_) return false;
      if (source_ != nullptr && pending_ == nullptr && !source_done_) {
        pending_ = source_->next();
        source_done_ = pending_ == nullptr;  // never poll an exhausted source again
      }
      const Timestamp t_gen = generator_ != nullptr ? generator_->next_ts() : Timestamp::max();
      const Timestamp t_src = pending_ != nullptr ? source_time(*pending_) : Timestamp::max();
      const Timestamp t_ord = transport_.next_order_arrival();
      const Timestamp t_flush = transport_.next_flush_ts();
      const SimTransport::Inbound in = transport_.next_inbound();
      const Timestamp t_in = in.ts;
      const Timestamp t_timer = hooks_.next_timer(hooks_.ctx);
      if (params_ != nullptr) params_->collect();
      const Timestamp t_param = params_ != nullptr ? params_->next_ts() : Timestamp::max();
      const Timestamp t_slow = slow_next_;
      // Periodic flushes, repeating timers and slow-tier runs alone never keep a run alive: once
      // no external event (generator, source, order in flight, wire message) remains, stop.
      Timestamp t = t_gen;
      if (t_src < t) t = t_src;
      if (t_ord < t) t = t_ord;
      if (t_in < t) t = t_in;
      if (t == Timestamp::max() && (world_ == nullptr || *world_ == Timestamp::max())) return false;
      if (t_flush < t) t = t_flush;
      if (t_param < t) t = t_param;
      if (t_timer < t) t = t_timer;
      if (t_slow < t) t = t_slow;
      if (t == Timestamp::max()) return true;  // waiting for the multiverse's next event
      SimKey key{t, SimKey::kSlow};
      if (t == t_gen) {
        key.rank = SimKey::kGenerator;
      } else if (t == t_src) {
        key.rank = SimKey::kSource;
      } else if (t == t_ord) {
        key.rank = SimKey::kOrder;
      } else if (t == t_flush) {
        key.rank = SimKey::kFlush;
      } else if (t == t_in) {
        key.rank = in.market_data ? SimKey::kMarketData : SimKey::kInbound;
      } else if (t == t_param) {
        key.rank = SimKey::kParam;
      } else if (t == t_timer) {
        key.rank = SimKey::kTimer;
      }
      if (!(key < limit)) return true;
      if (t > clock_.now()) clock_.set(t);
      if (key.rank == SimKey::kGenerator) {
        generator_->step(transport_.matching_engine());
        ++stats_.generator_actions;
      } else if (key.rank == SimKey::kSource) {
        transport_.on_source_event(*pending_);
        pending_ = nullptr;
        ++stats_.source_events;
      } else if (key.rank == SimKey::kOrder) {
        transport_.process_order_arrival();
        ++stats_.order_arrivals;
      } else if (key.rank == SimKey::kFlush) {
        transport_.flush_md(t);
        ++stats_.flushes;
      } else if (key.rank == SimKey::kInbound || key.rank == SimKey::kMarketData) {
        const EventType type = transport_.deliver_next_inbound(feed_);
        const bool md = is_market_data(type);
        if (md) {
          ++stats_.md_delivered;
        } else {
          ++stats_.order_events_delivered;
        }
        engine_step(md);
      } else if (key.rank == SimKey::kParam) {
        const ParamUpdateMsg m = params_->pop();
        if (feed_.push(m.hdr)) {
          ++stats_.param_updates;
          engine_step(false);
        }
      } else if (key.rank == SimKey::kTimer) {
        ++stats_.timer_steps;
        engine_step(false);
      } else {
        ++stats_.slow_runs;
        slow_next_ = slow_.run(slow_.ctx, t);
        if (slow_next_ <= t) slow_next_ = Timestamp{t.ns + 1};
        poll_stopped();
        if (!stopped_ && !feed_.empty()) engine_step(false);  // what the run handed the engine
      }
    }
  }
  void run_all() { static_cast<void>(run_until(Timestamp::max())); }

  // A multiverse (backtest/multiverse.hpp) reads the recorded events once for all its runs, so a
  // run's driver has no source of its own. follow_world() points it at the time of the next event
  // the multiverse's runs share, which it keeps current: while there is one an idle run is not over
  // (the multiverse steps the run no further than to it). world_event() then takes a recorded event
  // as the source branch of run_until() would, after the shared SimWorld took its half
  // (SimWorld::before()). Null, or Timestamp::max(): no more events.
  void follow_world(const Timestamp* next) noexcept { world_ = next; }
  void world_event(const EventHeader& md) noexcept {
    const Timestamp t = source_time(md);
    if (t > clock_.now()) clock_.set(t);
    transport_.on_source_event_run(md);
    ++stats_.source_events;
  }
  // world_event() in halves, for a multiverse that hands the venue's half only to the runs whose
  // orders the event can reach and forwards the market data of the runs that share it itself
  // (SimTransport::venue_event(), feed_event()).
  void world_venue_event(const EventHeader& md) noexcept {
    const Timestamp t = source_time(md);
    if (t > clock_.now()) clock_.set(t);
    transport_.venue_event(md);
  }
  void world_feed_event(const EventHeader& md) noexcept {
    const Timestamp t = source_time(md);
    if (t > clock_.now()) clock_.set(t);
    transport_.feed_event(md);
  }
  // Market data the multiverse forwarded for this run and others (recv_ts is its arrival): the
  // engine takes it where it lies, at its arrival, as it takes its own wire's.
  void shared_market_data(const EventHeader& md) noexcept {
    if (md.recv_ts > clock_.now()) clock_.set(md.recv_ts);
    transport_.count_md_delivered();
    const bool is_md = is_market_data(md.type);
    if (is_md) {
      ++stats_.md_delivered;
    } else {
      ++stats_.order_events_delivered;
    }
    ++stats_.engine_steps;
    if (measure_ && is_md && (++md_steps_ & (kMdStepSampleEvery - 1)) == 0) {
      const Timestamp a = steady_now();
      hooks_.step_with(hooks_.ctx, md);
      const Timestamp b = steady_now();
      stats_.md_step_ns.record(static_cast<std::uint64_t>((b - a).ns));
    } else {
      hooks_.step_with(hooks_.ctx, md);
    }
    poll_stopped();
    if (journal_ != nullptr && (stats_.engine_steps & 63U) == 0) drain_journal();
  }
  // The key of this run's next own event (not the shared ones): never() when it has none.
  [[nodiscard]] SimKey next_key() noexcept {
    if (stopped_) return SimKey::never();
    SimKey k = SimKey::never();
    const auto take = [&](Timestamp t, std::uint8_t rank) {
      if (t != Timestamp::max() && SimKey{t, rank} < k) k = SimKey{t, rank};
    };
    take(transport_.next_order_arrival(), SimKey::kOrder);
    const SimTransport::Inbound in = transport_.next_inbound();
    take(in.ts, in.market_data ? SimKey::kMarketData : SimKey::kInbound);
    if (params_ != nullptr) {
      params_->collect();
      take(params_->next_ts(), SimKey::kParam);
    }
    take(hooks_.next_timer(hooks_.ctx), SimKey::kTimer);
    take(slow_next_, SimKey::kSlow);
    return k;
  }
  // The earliest of the events an engine step can add: an order on its way to the venue, a timer.
  // A step adds no other kind, so after one the run's next own event is the earlier of this and
  // what it was before.
  [[nodiscard]] SimKey step_key() noexcept {
    if (stopped_) return SimKey::never();
    SimKey k = SimKey::never();
    if (const Timestamp t = transport_.next_order_arrival(); t != Timestamp::max())
      k = SimKey{t, SimKey::kOrder};
    if (const Timestamp t = hooks_.next_timer(hooks_.ctx);
        t != Timestamp::max() && SimKey{t, SimKey::kTimer} < k)
      k = SimKey{t, SimKey::kTimer};
    return k;
  }
  // Market data the multiverse did not hand this run because its engine did not need it: counted
  // as delivered, as alone (the engine caught up with it, Engine::catch_up).
  void credit_market_data(std::uint64_t n) noexcept {
    stats_.md_delivered += n;
    stats_.engine_steps += n;
    transport_.count_md_delivered(n);
  }
  // The clock as a run alone would leave it after the events it shares: at the last of them.
  void reach(Timestamp t) noexcept {
    if (t > clock_.now()) clock_.set(t);
  }

  void finish() {
    if (!started_ || finished_) return;
    finished_ = true;
    hooks_.finish(hooks_.ctx);
    drain_journal();
  }

  [[nodiscard]] const SimDriverStats& stats() const noexcept { return stats_; }
  [[nodiscard]] Timestamp now() const noexcept { return clock_.now(); }
  // The engine asked to stop; run_until() processes nothing more.
  [[nodiscard]] bool stopped() const noexcept { return stopped_; }

 private:
 public:
  // What the stats count as market data (the rest of a market-data wire, a feed's connection
  // state, counts as an order event).
  static constexpr bool is_market_data(EventType type) noexcept {
    return type == EventType::BookDelta || type == EventType::BookSnapshot ||
           type == EventType::Trade || type == EventType::BookTicker ||
           type == EventType::OptionTicker || type == EventType::PerpState ||
           type == EventType::OwnLevels;
  }

 private:
  static Timestamp source_time(const EventHeader& h) noexcept {
    return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
  }
  void engine_step(bool measure_md) {
    ++stats_.engine_steps;
    // Two clock reads per step would cost several percent of a backtest; sampling changes
    // nothing the run computes.
    if (measure_ && measure_md && (++md_steps_ & (kMdStepSampleEvery - 1)) == 0) {
      const Timestamp a = steady_now();
      hooks_.step(hooks_.ctx);
      const Timestamp b = steady_now();
      stats_.md_step_ns.record(static_cast<std::uint64_t>((b - a).ns));
    } else {
      hooks_.step(hooks_.ctx);
    }
    poll_stopped();
    if (journal_ != nullptr && (stats_.engine_steps & 63U) == 0) drain_journal();
  }
  void poll_stopped() {
    if (hooks_.stopped != nullptr && hooks_.stopped(hooks_.ctx)) stopped_ = true;
  }
  void drain_journal() {
    if (journal_ != nullptr) stats_.journal_drained += journal_->drain_once();
  }

  SimClock& clock_;
  SimTransport& transport_;
  InlineFeed& feed_;
  EngineHooks hooks_;
  MdSource* source_ = nullptr;
  const EventHeader* pending_ = nullptr;
  bool source_done_ = false;
  const Timestamp* world_ = nullptr;
  MarketGenerator* generator_ = nullptr;
  JournalFileWriter* journal_ = nullptr;
  ParamSchedule* params_ = nullptr;
  SlowHooks slow_{};
  Timestamp slow_next_ = Timestamp::max();
  int seed_levels_ = 20;
  bool measure_ = true;
  std::uint64_t md_steps_ = 0;
  bool started_ = false;
  bool finished_ = false;
  bool stopped_ = false;
  SimDriverStats stats_{};
};

class ReplayDriver {
 public:
  ReplayDriver(SimClock& clock, JournalFeed& feed, const EngineHooks& hooks) noexcept
      : clock_(clock), feed_(feed), hooks_(hooks) {}
  // Called with each event before the engine takes it (a replay stages the state a take-over
  // restores, Engine::stage_strategy_state).
  using BeforeStep = void (*)(void* ctx, const EventHeader& next);
  void set_before_step(BeforeStep fn, void* ctx) noexcept {
    before_ = fn;
    before_ctx_ = ctx;
  }

  void start() {
    if (feed_.has_start_ts()) clock_.jump(feed_.start_ts());
    hooks_.warm_up(hooks_.ctx);
    hooks_.start(hooks_.ctx);
    started_ = true;
  }
  // Replays every remaining journal event; returns the number processed.
  std::uint64_t run_all() {
    if (!started_) start();
    std::uint64_t n = 0;
    while (feed_.has_next()) {
      const Timestamp ts = feed_.peek_ts();
      if (feed_.has_engine_ts()) {
        clock_.jump(ts);
      } else if (ts > clock_.now()) {
        clock_.set(ts);
      }
      if (before_ != nullptr) before_(before_ctx_, *feed_.peek());
      feed_.arm();
      hooks_.step(hooks_.ctx);
      ++n;
    }
    return n;
  }
  void finish() {
    if (!started_ || finished_) return;
    finished_ = true;
    if (feed_.has_finish_ts()) clock_.jump(feed_.finish_ts());
    hooks_.finish(hooks_.ctx);
  }

 private:
  SimClock& clock_;
  JournalFeed& feed_;
  EngineHooks hooks_;
  BeforeStep before_ = nullptr;
  void* before_ctx_ = nullptr;
  bool started_ = false;
  bool finished_ = false;
};

}  // namespace fastmm::sim
