#include "live_runners.hpp"

#include "fastmm/strategies/basic_mm.hpp"

namespace fastmm::live {

namespace {
std::unique_ptr<IEngineRunner> make_basic_mm(TransportKind kind, RunnerDeps& deps) {
  return make_live_runner<BasicMM>(kind, deps);
}
}  // namespace

void register_live_basic_mm(StrategyRegistry& registry) {
  static_cast<void>(
      registry.add(BasicMM::name(), &BasicMM::schema(), TransportKind::Live, &make_basic_mm));
}

}  // namespace fastmm::live
