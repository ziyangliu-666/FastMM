#pragma once
// Registration functions of the test strategy library.
namespace fastmm {
class StrategyRegistry;
}

namespace test_mm {

// test_module_mm for Sim, Replay and Live (strategies.cpp, one file with factories.hpp).
void register_strategies(fastmm::StrategyRegistry& r);

// ShadowBasicMM for Sim only: the registration (shadow_register.cpp) declares the factory, the
// instantiation lives in another file (shadow_sim.cpp).
void register_shadow_basic_mm(fastmm::StrategyRegistry& r);

}  // namespace test_mm
