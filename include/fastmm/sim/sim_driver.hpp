#pragma once
// SimDriver: the discrete-event loop of a backtest (8.2). Advances the SimClock to the
// earliest pending event among
//
//   venue side   : generator action, historical source event, order arrival at the venue,
//                  market-data flush
//   engine side  : next wire message (ack/fill or market data), next engine timer
//
// and performs exactly that event, so venue-side and engine-side state evolve in one
// consistent virtual time. Ties resolve venue-first, then in the fixed order listed above.
// The engine is reached through EngineHooks (plain function pointers) so the driver works
// for any Engine<Strategy, SimClock, SimTransport, InlineFeed> and through the type-erased
// registry path alike.
//
// ReplayDriver replays a journal through Engine<S, SimClock, ReplayTransport, JournalFeed>:
// one journal event per step at its original time; wheel timers are suppressed because the
// journal already contains the synthetic TimerMsg records that fired.
#include "fastmm/core/journal.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/sim/journal_feed.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/md_source.hpp"
#include "fastmm/sim/sim_transport.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm::sim {

struct EngineHooks {
  void* ctx = nullptr;
  std::size_t (*step)(void*) = nullptr;
  Timestamp (*next_timer)(void*) = nullptr;
  void (*warm_up)(void*) = nullptr;
  void (*start)(void*) = nullptr;
  void (*finish)(void*) = nullptr;
  void (*cancel_timers)(void*) = nullptr;

  template <class Engine>
  [[nodiscard]] static EngineHooks for_engine(Engine& e) noexcept {
    EngineHooks h;
    h.ctx = &e;
    h.step = [](void* c) { return static_cast<Engine*>(c)->step(); };
    h.next_timer = [](void* c) { return static_cast<Engine*>(c)->timers().next_expiry(); };
    h.warm_up = [](void* c) { static_cast<Engine*>(c)->warm_up(); };
    h.start = [](void* c) { static_cast<Engine*>(c)->start(); };
    h.finish = [](void* c) { static_cast<Engine*>(c)->finish(); };
    h.cancel_timers = [](void* c) {
      auto& w = static_cast<Engine*>(c)->timers();
      if (w.size() == 0) return;
      for (std::uint32_t i = 0; i < 1024; ++i) {
        if (w.active(TimerId{i})) w.cancel(TimerId{i});
      }
    };
    return h;
  }
  [[nodiscard]] bool valid() const noexcept { return ctx != nullptr && step != nullptr; }
};

struct SimDriverStats {
  std::uint64_t generator_actions = 0;
  std::uint64_t source_events = 0;
  std::uint64_t order_arrivals = 0;
  std::uint64_t flushes = 0;
  std::uint64_t md_delivered = 0;
  std::uint64_t order_events_delivered = 0;
  std::uint64_t timer_steps = 0;
  std::uint64_t engine_steps = 0;
  std::uint64_t journal_drained = 0;
  LogLinearHistogram md_step_ns;  // wall-clock ns per engine step that consumed market data
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
  void set_measure_wall_clock(bool v) noexcept { measure_ = v; }

  // warm_up + on_start; seeds the book and publishes the first snapshot in coupled mode.
  void start() {
    hooks_.warm_up(hooks_.ctx);
    if (generator_ != nullptr) {
      transport_.enable_aggregator(clock_.now());
      generator_->seed_book(transport_.matching_engine(), seed_levels_, clock_.now());
    }
    hooks_.start(hooks_.ctx);
    if (generator_ != nullptr) transport_.flush_md(clock_.now());
    started_ = true;
  }

  // Processes every event with time <= until. Returns false when no event remains.
  bool run_until(Timestamp until) {
    if (!started_) start();
    for (;;) {
      if (source_ != nullptr && pending_ == nullptr && !source_done_) {
        pending_ = source_->next();
        source_done_ = pending_ == nullptr;  // never poll an exhausted source again
      }
      const Timestamp t_gen = generator_ != nullptr ? generator_->next_ts() : Timestamp::max();
      const Timestamp t_src = pending_ != nullptr ? source_time(*pending_) : Timestamp::max();
      const Timestamp t_ord = transport_.next_order_arrival();
      const Timestamp t_flush = transport_.next_flush_ts();
      const Timestamp t_in = transport_.next_inbound_ts();
      const Timestamp t_timer = hooks_.next_timer(hooks_.ctx);
      // Periodic flushes and repeating timers alone never keep a run alive: once no
      // external event (generator, source, order in flight, wire message) remains, stop.
      Timestamp t = t_gen;
      if (t_src < t) t = t_src;
      if (t_ord < t) t = t_ord;
      if (t_in < t) t = t_in;
      if (t == Timestamp::max()) return false;
      if (t_flush < t) t = t_flush;
      if (t_timer < t) t = t_timer;
      if (t > until) return true;
      if (t > clock_.now()) clock_.set(t);
      if (t == t_gen) {
        generator_->step(transport_.matching_engine());
        ++stats_.generator_actions;
      } else if (t == t_src) {
        transport_.on_source_event(*pending_);
        pending_ = nullptr;
        ++stats_.source_events;
      } else if (t == t_ord) {
        transport_.process_order_arrival();
        ++stats_.order_arrivals;
      } else if (t == t_flush) {
        transport_.flush_md(t);
        ++stats_.flushes;
      } else if (t == t_in) {
        const EventType type = transport_.deliver_next_inbound(feed_);
        const bool md = type == EventType::BookDelta || type == EventType::BookSnapshot ||
                        type == EventType::Trade || type == EventType::BookTicker ||
                        type == EventType::OptionTicker;
        if (md) {
          ++stats_.md_delivered;
        } else {
          ++stats_.order_events_delivered;
        }
        engine_step(md);
      } else {
        ++stats_.timer_steps;
        engine_step(false);
      }
    }
  }
  void run_all() { static_cast<void>(run_until(Timestamp::max())); }

  void finish() {
    if (!started_ || finished_) return;
    finished_ = true;
    hooks_.finish(hooks_.ctx);
    drain_journal();
  }

  [[nodiscard]] const SimDriverStats& stats() const noexcept { return stats_; }
  [[nodiscard]] Timestamp now() const noexcept { return clock_.now(); }

 private:
  static Timestamp source_time(const EventHeader& h) noexcept {
    return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
  }
  void engine_step(bool measure_md) {
    ++stats_.engine_steps;
    if (measure_ && measure_md) {
      const Timestamp a = steady_now();
      hooks_.step(hooks_.ctx);
      const Timestamp b = steady_now();
      stats_.md_step_ns.record(static_cast<std::uint64_t>((b - a).ns));
    } else {
      hooks_.step(hooks_.ctx);
    }
    if (journal_ != nullptr && (stats_.engine_steps & 63U) == 0) drain_journal();
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
  MarketGenerator* generator_ = nullptr;
  JournalFileWriter* journal_ = nullptr;
  int seed_levels_ = 20;
  bool measure_ = true;
  bool started_ = false;
  bool finished_ = false;
  SimDriverStats stats_{};
};

class ReplayDriver {
 public:
  ReplayDriver(SimClock& clock, JournalFeed& feed, const EngineHooks& hooks) noexcept
      : clock_(clock), feed_(feed), hooks_(hooks) {}

  void start() {
    hooks_.warm_up(hooks_.ctx);
    hooks_.start(hooks_.ctx);
    hooks_.cancel_timers(hooks_.ctx);
    started_ = true;
  }
  // Replays every remaining journal event; returns the number processed.
  std::uint64_t run_all() {
    if (!started_) start();
    std::uint64_t n = 0;
    while (feed_.has_next()) {
      const Timestamp ts = feed_.peek_ts();
      if (ts > clock_.now()) clock_.set(ts);
      feed_.arm();
      hooks_.step(hooks_.ctx);
      hooks_.cancel_timers(hooks_.ctx);
      ++n;
    }
    return n;
  }
  void finish() {
    if (!started_ || finished_) return;
    finished_ = true;
    hooks_.finish(hooks_.ctx);
  }

 private:
  SimClock& clock_;
  JournalFeed& feed_;
  EngineHooks hooks_;
  bool started_ = false;
  bool finished_ = false;
};

}  // namespace fastmm::sim
