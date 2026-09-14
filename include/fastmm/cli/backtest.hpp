#pragma once
// fastmm::cli::backtest: the fastmm-backtest command line as a library function (fastmm::backtest).
//
//   #include "fastmm/cli/backtest.hpp"
//   #include "mm/strategies.hpp"
//   int main(int argc, char** argv) {
//     return fastmm::cli::backtest(argc, argv, {mm::register_strategies});
//   }
//
// The built-in strategies are always registered, then every module in order; a strategy name
// registered by different code exits with code 3. Usage and error messages are prefixed with the
// program name. Run `--help` for the flags and exit codes.
#include "fastmm/strategies/module.hpp"

#include <initializer_list>
#include <span>

namespace fastmm::cli {

int backtest(int argc, char** argv, std::span<const StrategyModule> modules);

inline int backtest(int argc, char** argv, std::initializer_list<StrategyModule> modules = {}) {
  return backtest(argc, argv, std::span<const StrategyModule>(modules.begin(), modules.size()));
}

}  // namespace fastmm::cli
