// The tutorial's only registration file. factories.hpp brings the Sim, Replay and Live factory
// definitions, so this file compiles FirstMM's three engines.
// [start:register]
#include "strategies.hpp"

#include "first_mm.hpp"

#include "fastmm/strategies/factories.hpp"

void tutorial::register_strategies(fastmm::StrategyRegistry& r) {
  fastmm::register_strategy<FirstMM>(r);  // Sim + Replay + Live
}
// [end:register]
