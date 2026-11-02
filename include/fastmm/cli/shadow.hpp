#pragma once
// fastmm-shadow's command line (src/backtest/cli_shadow.cpp), for an application whose strategies
// are its own: `return fastmm::cli::shadow(argc, argv, {my_module});`. Run `--help` for the flags
// and exit codes.
#include "fastmm/strategies/module.hpp"

#include <initializer_list>
#include <span>

namespace fastmm::cli {

int shadow(int argc, char** argv, std::span<const StrategyModule> modules);

inline int shadow(int argc, char** argv, std::initializer_list<StrategyModule> modules = {}) {
  return shadow(argc, argv, std::span<const StrategyModule>(modules.begin(), modules.size()));
}

}  // namespace fastmm::cli
