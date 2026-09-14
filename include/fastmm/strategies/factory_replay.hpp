#pragma once
// Definition of fastmm::replay_factory<S> (declared in strategies/module.hpp): builds
// Engine<S, SimClock, ReplayTransport, JournalFeed> on the sim::ReplayBackend that
// RunnerDeps::backend points at. Needs fastmm::sim.
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/module.hpp"

#include <memory>

namespace fastmm {

template <class S>
std::unique_ptr<IEngineRunner> replay_factory(TransportKind kind, RunnerDeps& deps) {
  static_assert(StrategyLike<S>,
                "fastmm: replay_factory<S> needs a strategy: static name(), static schema() and a "
                "default constructor");
  if (kind != TransportKind::Replay || deps.backend == nullptr) return nullptr;
  return static_cast<sim::ReplayBackend*>(deps.backend)->template make_runner<S>(deps);
}

}  // namespace fastmm
