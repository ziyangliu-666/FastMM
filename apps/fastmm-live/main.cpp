// fastmm-live - placeholder entry point; replaced when the app is implemented.
#include "fastmm/version.hpp"

#include <cstdio>

int main() {
  std::printf("%s (%s)\n", "fastmm-live", fastmm::build_info());
  return 0;
}
