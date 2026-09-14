// Registers a strategy named basic_mm next to the built-in one: the command line exits with code 3.
#include "module/strategies.hpp"

#include "fastmm/cli/backtest.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::backtest(
      argc, argv, {test_mm::register_strategies, test_mm::register_shadow_basic_mm});
}
