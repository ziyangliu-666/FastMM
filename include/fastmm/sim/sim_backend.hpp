#pragma once
// SimBackend / ReplayBackend: the bundles RunnerDeps::backend points at when the registry
// builds an IEngineRunner for TransportKind::Sim / Replay (8.6). Each owns the clock,
// transport and feed, and after make_runner<S>() also the EngineHooks the drivers need.
//
//   sim::SimBackend backend(instruments, cfg, start);
//   RunnerDeps deps{...}; deps.backend = &backend;
//   auto runner = StrategyRegistry::instance().make("basic_mm", TransportKind::Sim, deps);
//   sim::SimDriver driver(backend.clock, backend.transport, backend.feed, backend.hooks);
//
// sim_factory<S> and replay_factory<S> (strategies/factory_sim.hpp, factory_replay.hpp) call
// make_runner<S>() on the backend RunnerDeps::backend points at.
#include "fastmm/core/engine.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/sim/journal_feed.hpp"
#include "fastmm/sim/replay_transport.hpp"
#include "fastmm/sim/sim_driver.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/engine_factory.hpp"
#include "fastmm/strategies/registry.hpp"

#include <memory>

namespace fastmm::sim {

struct SimBackend {
  SimClock clock;
  SimTransport transport;
  InlineFeed feed;
  EngineHooks hooks{};

  SimBackend(const InstrumentTable& instruments,
             const SimTransportConfig& cfg,
             Timestamp start,
             std::size_t feed_bytes = 1U << 22)
      : clock(start), transport(clock, instruments, cfg), feed(feed_bytes) {}

  template <StrategyLike S>
  std::unique_ptr<IEngineRunner> make_runner(RunnerDeps& deps) {
    using E = Engine<S, SimClock, SimTransport, InlineFeed>;
    std::unique_ptr<IEngineRunner> r =
        make_engine_runner<S, SimClock, SimTransport, InlineFeed>(deps, clock, transport, feed);
    auto* er = static_cast<EngineRunner<E, S>*>(r.get());  // we just built it
    hooks = EngineHooks::for_engine(er->engine());
    return r;
  }
};

struct ReplayBackend {
  SimClock clock;
  ReplayTransport transport;
  JournalFeed feed;
  EngineHooks hooks{};

  ReplayBackend(JournalReader& reader, Timestamp start) : clock(start), feed(reader) {}

  template <StrategyLike S>
  std::unique_ptr<IEngineRunner> make_runner(RunnerDeps& deps) {
    using E = Engine<S, SimClock, ReplayTransport, JournalFeed>;
    std::unique_ptr<IEngineRunner> r =
        make_engine_runner<S, SimClock, ReplayTransport, JournalFeed>(deps, clock, transport, feed);
    auto* er = static_cast<EngineRunner<E, S>*>(r.get());
    hooks = EngineHooks::for_engine(er->engine());
    return r;
  }
};

}  // namespace fastmm::sim
