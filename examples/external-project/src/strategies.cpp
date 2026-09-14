// The project's only registration file. factories.hpp brings the Sim, Replay and Live factory
// definitions, so this file compiles the three engines of every strategy registered below. Without
// it the build fails at link time, naming the missing factory.
#include "mm/strategies.hpp"

#include "mm/microprice_mm.hpp"

#include "fastmm/strategies/factories.hpp"

void mm::register_strategies(fastmm::StrategyRegistry& r) {
  fastmm::register_strategy<MicropriceMM>(r);  // Sim + Replay + Live
}
