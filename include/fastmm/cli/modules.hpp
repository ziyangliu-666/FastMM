#pragma once
// Shared by the command-line entry points fastmm::cli::live, backtest and replay
// (fastmm::strategies library).
#include "fastmm/strategies/module.hpp"

#include <span>
#include <string>
#include <string_view>

namespace fastmm::cli {

// basename(argv[0]), or `fallback` when there is none: the prefix of usage and error messages.
[[nodiscard]] std::string program_name(int argc, char** argv, std::string_view fallback);

// Registers the built-in strategies, then every module in order, in StrategyRegistry::instance().
// On an error (a strategy name registered by different code) prints "<program>: <message>" to
// stderr and returns false; the command lines exit with code 3.
[[nodiscard]] bool register_strategy_modules(std::string_view program,
                                             std::span<const StrategyModule> modules);

}  // namespace fastmm::cli
