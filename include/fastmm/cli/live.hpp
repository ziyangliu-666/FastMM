#pragma once
// fastmm::cli::live: the fastmm-live command line as a library function (fastmm::live), so a
// strategy project's live app is a 3-line main:
//
//   #include "fastmm/cli/live.hpp"
//   #include "mm/strategies.hpp"
//   int main(int argc, char** argv) { return fastmm::cli::live(argc, argv,
//   {mm::register_strategies}); }
//
// The built-in strategies are always registered, then every module in order; a strategy name
// registered by different code exits with code 3. Usage and error messages are prefixed with the
// program name; log lines keep the `fastmm-live:` tag. A session installs process-wide SIGINT and
// SIGTERM handlers (they trip the kill switch, cancel all orders and stop). Run `--help` for the
// flags and exit codes.
#include "fastmm/strategies/module.hpp"

#include <initializer_list>
#include <span>

namespace fastmm::cli {

int live(int argc, char** argv, std::span<const StrategyModule> modules);

inline int live(int argc, char** argv, std::initializer_list<StrategyModule> modules = {}) {
  return live(argc, argv, std::span<const StrategyModule>(modules.begin(), modules.size()));
}

}  // namespace fastmm::cli
