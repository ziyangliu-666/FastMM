#include "module/strategies.hpp"

#include "fastmm/cli/backtest.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::backtest(argc, argv, {test_mm::register_strategies});
}
