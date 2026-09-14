#include "strategies.hpp"

#include "test_module_mm.hpp"

#include "fastmm/strategies/factories.hpp"

void test_mm::register_strategies(fastmm::StrategyRegistry& r) {
  fastmm::register_strategy<TestModuleMM>(r);
}
