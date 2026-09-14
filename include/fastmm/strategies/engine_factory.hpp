#pragma once
// make_engine_runner<S>(): builds strategy + Engine<S, Clock, Transport, Feed> for a backend's
// clock, transport and feed and wraps them in an IEngineRunner. The factory headers
// (factory_sim.hpp, factory_replay.hpp, factory_live.hpp) call it; including this header parses
// the Engine template.
#include "fastmm/core/engine.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/strategies/registry.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace fastmm {

// Throws std::invalid_argument on bad parameters (startup only).
template <StrategyLike S, ClockLike Clock, TransportLike Transport, FeedLike Feed>
std::unique_ptr<IEngineRunner> make_engine_runner(RunnerDeps& deps,
                                                  Clock& clock,
                                                  Transport& transport,
                                                  Feed& feed) {
  using E = Engine<S, Clock, Transport, Feed>;
  auto strategy = std::make_unique<S>();
  if (auto err = strategy->configure(deps.params))
    throw std::invalid_argument(std::string(S::name()) + ": " + *err);
  auto engine = std::make_unique<E>(
      deps.engine, *deps.instruments, clock, transport, feed, *strategy, deps.journal_ring);
  return std::make_unique<EngineRunner<E, S>>(std::move(strategy), std::move(engine));
}

}  // namespace fastmm
