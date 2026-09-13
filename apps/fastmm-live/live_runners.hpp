#pragma once
// App-local live runner builders. The global StrategyRegistry keeps one factory per name and
// the backtest library owns the Sim/Replay registrations for basic_mm and
// avellaneda_stoikov, so fastmm-live does NOT register those names; it instantiates
// Engine<S, TscClock, LiveTransport, RingFeed> through this table instead (one .cpp per
// strategy keeps the heavy template instantiations in parallel compile units).
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/registry.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace fastmm::live {

struct LiveRunnerBuilder {
  std::string_view name;
  const ParamSchema& (*schema)();
  std::unique_ptr<IEngineRunner> (*make)(RunnerDeps& deps,
                                         TscClock& clock,
                                         LiveTransport& transport,
                                         RingFeed& feed);
};

std::unique_ptr<IEngineRunner> make_live_basic_mm(RunnerDeps&,
                                                  TscClock&,
                                                  LiveTransport&,
                                                  RingFeed&);
std::unique_ptr<IEngineRunner> make_live_avellaneda_stoikov(RunnerDeps&,
                                                            TscClock&,
                                                            LiveTransport&,
                                                            RingFeed&);
const ParamSchema& basic_mm_schema();
const ParamSchema& avellaneda_stoikov_schema();
std::string_view basic_mm_name() noexcept;
std::string_view avellaneda_stoikov_name() noexcept;

[[nodiscard]] std::span<const LiveRunnerBuilder> live_runner_builders();
[[nodiscard]] const LiveRunnerBuilder* find_live_runner(std::string_view name);

}  // namespace fastmm::live
