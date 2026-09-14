// tutorial-backtest: fastmm-backtest with the tutorial's strategies next to the built-in ones.
#include "strategies.hpp"

#include "fastmm/cli/backtest.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::backtest(argc, argv, {tutorial::register_strategies});
}
