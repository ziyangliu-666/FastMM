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

// Never asserts. Tests put it in INFO and *_MESSAGE streams (a log to show with a failure), and
// doctest evaluates those while it reports the failed assertion: an assertion in there, even one
// that passes, re-enters the reporter and the test hangs instead of failing. A file that cannot be
// opened reads as a line saying so.
inline std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in.good()) return "(cannot open " + p.string() + ")";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline std::string fixture(const std::string& rel) {
  const std::filesystem::path p = fixtures_dir() / rel;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec)) FAIL("no fixture " << p.string());
  return read_file(p);
}

}  // namespace fastmm::test
