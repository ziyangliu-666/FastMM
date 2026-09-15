// Declarations only: the Sim factory of ShadowBasicMM is instantiated in shadow_sim.cpp.
#include "strategies.hpp"
#include "test_module_mm.hpp"

#include "fastmm/strategies/module.hpp"

void test_mm::register_shadow_basic_mm(fastmm::StrategyRegistry& r) {
  fastmm::register_strategy<ShadowBasicMM, fastmm::Transports::Sim>(r);
}
