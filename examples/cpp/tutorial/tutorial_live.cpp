// tutorial-live: fastmm-live with the tutorial's strategies next to the built-in ones.
// [start:main]
#include "strategies.hpp"

#include "fastmm/cli/live.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::live(argc, argv, {tutorial::register_strategies});
}
// [end:main]
