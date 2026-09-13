#include "live_runners.hpp"

#include "fastmm/strategies/avellaneda_stoikov.hpp"

namespace fastmm::live {

std::unique_ptr<IEngineRunner> make_live_avellaneda_stoikov(RunnerDeps& deps,
                                                            TscClock& clock,
                                                            LiveTransport& transport,
                                                            RingFeed& feed) {
  return make_engine_runner<AvellanedaStoikov>(deps, clock, transport, feed);
}
const ParamSchema& avellaneda_stoikov_schema() {
  return AvellanedaStoikov::schema();
}
std::string_view avellaneda_stoikov_name() noexcept {
  return AvellanedaStoikov::name();
}

std::span<const LiveRunnerBuilder> live_runner_builders() {
  static const LiveRunnerBuilder kBuilders[] = {
      {basic_mm_name(), &basic_mm_schema, &make_live_basic_mm},
      {avellaneda_stoikov_name(), &avellaneda_stoikov_schema, &make_live_avellaneda_stoikov},
  };
  return kBuilders;
}

const LiveRunnerBuilder* find_live_runner(std::string_view name) {
  for (const LiveRunnerBuilder& b : live_runner_builders()) {
    if (b.name == name) return &b;
  }
  return nullptr;
}

}  // namespace fastmm::live
