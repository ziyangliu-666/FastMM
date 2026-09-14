// tutorial-replay: fastmm-replay with the tutorial's strategies, so journals recorded by
// tutorial-live and tutorial-backtest replay exactly.
#include "strategies.hpp"

#include "fastmm/cli/replay.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::replay(argc, argv, {tutorial::register_strategies});
}
