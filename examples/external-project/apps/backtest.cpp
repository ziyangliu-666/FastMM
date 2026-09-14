// mm-backtest: fastmm-backtest with this project's strategies next to the built-in ones.
//   mm-backtest --config configs/backtest-example.toml --data synthetic --strategy microprice_mm
#include "fastmm/cli/backtest.hpp"

#include "mm/strategies.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::backtest(argc, argv, {mm::register_strategies});
}
