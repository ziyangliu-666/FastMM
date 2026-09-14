#pragma once
// Live strategy factories: fastmm-live registers a TransportKind::Live factory per strategy in
// the StrategyRegistry, next to the Sim/Replay factories the backtest library registers. The
// registration is explicit (register_live_strategies() at startup), in the same style as
// bt::register_builtin_strategies(); one .cpp per strategy keeps the heavy
// Engine<S, TscClock, LiveTransport, RingFeed> instantiations in parallel compile units.
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/strategies/registry.hpp"

#include <cstddef>
#include <memory>

namespace fastmm::live {

// What RunnerDeps::backend points at for TransportKind::Live.
struct LiveBackend {
  TscClock* clock = nullptr;
  LiveTransport* transport = nullptr;
  RingFeed* feed = nullptr;
};

// Factory body shared by the live registrations.
template <StrategyLike S>
std::unique_ptr<IEngineRunner> make_live_runner(TransportKind kind, RunnerDeps& deps) {
  if (kind != TransportKind::Live || deps.backend == nullptr) return nullptr;
  auto* b = static_cast<LiveBackend*>(deps.backend);
  return make_engine_runner<S>(deps, *b->clock, *b->transport, *b->feed);
}

// One per strategy (runner_<strategy>.cpp).
void register_live_basic_mm(StrategyRegistry& registry);
void register_live_avellaneda_stoikov(StrategyRegistry& registry);
void register_live_options_mm(StrategyRegistry& registry);

// Registers every live factory; repeated calls are harmless. Returns the number of registry
// entries.
std::size_t register_live_strategies();

}  // namespace fastmm::live
