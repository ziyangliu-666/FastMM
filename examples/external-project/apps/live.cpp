// mm-live: fastmm-live with this project's strategies next to the built-in ones.
//   FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret \
//     mm-live --config configs/sim-local.toml --strategy microprice_mm --duration 60s
#include "fastmm/cli/live.hpp"

#include "mm/strategies.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::live(argc, argv, {mm::register_strategies});
}
