#include "test_module_mm.hpp"

#include "fastmm/strategies/factory_sim.hpp"

FASTMM_INSTANTIATE_STRATEGY(test_mm::ShadowBasicMM, sim);
