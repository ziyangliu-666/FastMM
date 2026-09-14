#pragma once
// Definition of fastmm::sim_factory<S> (declared in strategies/module.hpp): builds
// Engine<S, SimClock, SimTransport, InlineFeed> on the sim::SimBackend that RunnerDeps::backend
// points at. Needs fastmm::sim.
#include "fastmm/sim/sim_backend.hpp"
#include "fastmm/strategies/module.hpp"

#include <memory>

namespace fastmm {

template <class S>
std::unique_ptr<IEngineRunner> sim_factory(TransportKind kind, RunnerDeps& deps) {
  static_assert(StrategyLike<S>,
                "fastmm: sim_factory<S> needs a strategy: static name(), static schema() and a "
                "default constructor");
  if (kind != TransportKind::Sim || deps.backend == nullptr) return nullptr;
  return static_cast<sim::SimBackend*>(deps.backend)->template make_runner<S>(deps);
}

}  // namespace fastmm
