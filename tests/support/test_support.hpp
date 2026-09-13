#pragma once
// Shared helpers for fastmm tests.
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fastmm::test {

inline std::filesystem::path fixtures_dir() {
  return FASTMM_FIXTURES_DIR;
}
inline std::filesystem::path tmp_dir() {
  return FASTMM_TEST_TMP_DIR;
}

inline std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline std::string fixture(const std::string& rel) {
  return read_file(fixtures_dir() / rel);
}

}  // namespace fastmm::test
