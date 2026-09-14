#pragma once
// Definition of fastmm::live_factory<S> (declared in strategies/module.hpp): builds
// Engine<S, TscClock, LiveTransport, RingFeed> on the live::LiveBackend that RunnerDeps::backend
// points at. Needs only fastmm::core (no networking).
#include "fastmm/live/live_backend.hpp"
#include "fastmm/strategies/module.hpp"

#include <memory>

namespace fastmm {

template <class S>
std::unique_ptr<IEngineRunner> live_factory(TransportKind kind, RunnerDeps& deps) {
  static_assert(StrategyLike<S>,
                "fastmm: live_factory<S> needs a strategy: static name(), static schema() and a "
                "default constructor");
  return live::make_live_runner<S>(kind, deps);
}

}  // namespace fastmm
