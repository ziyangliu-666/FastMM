#include "test_support.hpp"
#include "fastmm/version.hpp"

TEST_CASE("core.smoke: version string is populated") {
  CHECK(fastmm::kVersionMajor >= 0);
  CHECK(std::string(fastmm::kVersionString).find('.') != std::string::npos);
  CHECK(std::string(fastmm::build_info()).find("fastmm") == 0);
}
