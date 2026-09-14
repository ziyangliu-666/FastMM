// mm-replay: fastmm-replay with this project's strategies, so journals recorded by mm-live and
// mm-backtest replay exactly.
//   mm-replay --journal runs/session.fmj --verify
#include "fastmm/cli/replay.hpp"

#include "mm/strategies.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::replay(argc, argv, {mm::register_strategies});
}
