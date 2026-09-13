#include "live_runners.hpp"

#include "fastmm/strategies/avellaneda_stoikov.hpp"

namespace fastmm::live {

namespace {
std::unique_ptr<IEngineRunner> make_avellaneda_stoikov(TransportKind kind, RunnerDeps& deps) {
  return make_live_runner<AvellanedaStoikov>(kind, deps);
}
}  // namespace

void register_live_avellaneda_stoikov(StrategyRegistry& registry) {
  static_cast<void>(registry.add(AvellanedaStoikov::name(),
                                 &AvellanedaStoikov::schema(),
                                 TransportKind::Live,
                                 &make_avellaneda_stoikov));
}

}  // namespace fastmm::live
