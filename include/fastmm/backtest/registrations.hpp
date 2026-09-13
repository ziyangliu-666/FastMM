#pragma once
// Registers the shipped strategies (basic_mm, avellaneda_stoikov) for TransportKind::Sim and
// TransportKind::Replay. Idempotent and thread-compatible (call it once before starting
// worker threads); returns the number of strategies in the registry.
#include <cstddef>

namespace fastmm::bt {
std::size_t register_builtin_strategies();
}  // namespace fastmm::bt
