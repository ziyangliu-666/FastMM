#include "live_runners.hpp"

#include "fastmm/strategies/options_mm.hpp"

namespace fastmm::live {

namespace {
std::unique_ptr<IEngineRunner> make_options_mm(TransportKind kind, RunnerDeps& deps) {
  return make_live_runner<OptionsMM>(kind, deps);
}
}  // namespace

void register_live_options_mm(StrategyRegistry& registry) {
  static_cast<void>(
      registry.add(OptionsMM::name(), &OptionsMM::schema(), TransportKind::Live, &make_options_mm));
}

}  // namespace fastmm::live
