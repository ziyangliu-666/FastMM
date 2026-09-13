#include "fastmm/version.hpp"

#include <cstdio>
int main() {
  std::puts(fastmm::build_info());
  return 0;
}
