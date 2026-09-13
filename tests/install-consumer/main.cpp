#include "fastmm/backtest/registrations.hpp"
#include "fastmm/version.hpp"

#include <cstdio>

int main() {
  const std::size_t n = fastmm::bt::register_builtin_strategies();
  std::printf("%s | %zu strategies registered\n", fastmm::build_info(), n);
  return n >= 2 ? 0 : 1;
}
