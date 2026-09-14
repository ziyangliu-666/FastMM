#pragma once
// LiveBackend: what RunnerDeps::backend points at for TransportKind::Live. The live session
// (fastmm::live, src/live/session.cpp) owns the clock, transport and feed; live_factory<S>
// (strategies/factory_live.hpp) builds Engine<S, TscClock, LiveTransport, RingFeed> on them.
// Header-only and core-only: strategy libraries that register Live factories need no networking.
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/strategies/engine_factory.hpp"
#include "fastmm/strategies/registry.hpp"

#include <memory>

namespace fastmm::live {

struct LiveBackend {
  TscClock* clock = nullptr;
  LiveTransport* transport = nullptr;
  RingFeed* feed = nullptr;
};

template <StrategyLike S>
std::unique_ptr<IEngineRunner> make_live_runner(TransportKind kind, RunnerDeps& deps) {
  if (kind != TransportKind::Live || deps.backend == nullptr) return nullptr;
  auto* b = static_cast<LiveBackend*>(deps.backend);
  return make_engine_runner<S>(deps, *b->clock, *b->transport, *b->feed);
}

}  // namespace fastmm::live
