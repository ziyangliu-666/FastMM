#include "fastmm/core/host_tuning.hpp"

#include "test_support.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace fastmm;

namespace {

std::string tmp_path(const char* name) {
  return std::string(FASTMM_TEST_TMP_DIR) + "/" + name;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("core.host_tuning: a CPU latency request writes the 32-bit target and holds the fd") {
  const std::string path = tmp_path("cpu_dma_latency");
  { std::ofstream out(path, std::ios::trunc); }
  CpuLatencyRequest req;
  CHECK_FALSE(req.active());
  REQUIRE(req.open(7, path.c_str()) == 0);
  CHECK(req.active());
  const std::string bytes = read_file(path);
  REQUIRE(bytes.size() == sizeof(std::int32_t));
  std::int32_t v = -1;
  std::memcpy(&v, bytes.data(), sizeof v);
  CHECK(v == 7);
  CpuLatencyRequest moved = std::move(req);
  CHECK(moved.active());
  CHECK_FALSE(req.active());  // NOLINT(bugprone-use-after-move)
  moved.close();
  CHECK_FALSE(moved.active());
  std::filesystem::remove(path);
}

TEST_CASE("core.host_tuning: a CPU latency request that cannot open the device reports errno") {
  CpuLatencyRequest req;
  CHECK(req.open(0, tmp_path("no-such-dir/cpu_dma_latency").c_str()) == ENOENT);
  CHECK_FALSE(req.active());
  CHECK(req.open(-1, tmp_path("unused").c_str()) == EINVAL);
  CHECK_FALSE(req.active());
}
