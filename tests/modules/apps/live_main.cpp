#include "module/strategies.hpp"

#include "fastmm/cli/live.hpp"

int main(int argc, char** argv) {
  return fastmm::cli::live(argc, argv, {test_mm::register_strategies});
}
