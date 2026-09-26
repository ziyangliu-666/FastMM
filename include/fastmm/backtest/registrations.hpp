#pragma once
// bt::register_builtin_strategies(): the built-in strategies (basic_mm, avellaneda_stoikov,
// options_mm, lead_mm) in the global StrategyRegistry, for every transport. A thin wrapper around
// fastmm::register_builtin_strategies (strategies/builtin.hpp) for the library paths that look
// strategies up by name. Idempotent; call it before starting worker threads. Returns the number of
// strategies in the registry.
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/registry.hpp"

#include <cstddef>

namespace fastmm::bt {

inline std::size_t register_builtin_strategies() {
  StrategyRegistry& r = StrategyRegistry::instance();
  ::fastmm::register_builtin_strategies(r);
  return r.entries().size();
}

}  // namespace fastmm::bt
