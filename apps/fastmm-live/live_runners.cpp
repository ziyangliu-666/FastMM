#include "live_runners.hpp"

namespace fastmm::live {

std::size_t register_live_strategies() {
  StrategyRegistry& r = StrategyRegistry::instance();
  register_live_basic_mm(r);
  register_live_avellaneda_stoikov(r);
  return r.entries().size();
}

}  // namespace fastmm::live
