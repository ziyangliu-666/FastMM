#pragma once
// fastmm::cli::replay: the fastmm-replay command line as a library function (fastmm::backtest).
//
//   #include "fastmm/cli/replay.hpp"
//   #include "mm/strategies.hpp"
//   int main(int argc, char** argv) {
//     return fastmm::cli::replay(argc, argv, {mm::register_strategies});
//   }
//
// A journal recorded by a custom app replays with an app that registers the same modules. The
// built-in strategies are always registered, then every module in order; a strategy name
// registered by different code exits with code 3. Run `--help` for the flags and exit codes.
#include "fastmm/strategies/module.hpp"

#include <initializer_list>
#include <span>

namespace fastmm::cli {

int replay(int argc, char** argv, std::span<const StrategyModule> modules);

inline int replay(int argc, char** argv, std::initializer_list<StrategyModule> modules = {}) {
  return replay(argc, argv, std::span<const StrategyModule>(modules.begin(), modules.size()));
}

}  // namespace fastmm::cli
