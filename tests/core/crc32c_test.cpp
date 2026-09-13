#include "fastmm/core/crc32c.hpp"

#include "test_support.hpp"

#include <string>
#include <vector>

using namespace fastmm;

TEST_CASE("core.crc32c: known vectors, hw == sw") {
  // RFC 3720 / common test vectors
  CHECK(crc32c("", 0) == 0x00000000U);
  CHECK(crc32c("a", 1) == 0xC1D04330U);
  CHECK(crc32c("123456789", 9) == 0xE3069283U);
  const std::string quick = "The quick brown fox jumps over the lazy dog";
  CHECK(crc32c(quick.data(), quick.size()) == 0x22620404U);
  std::vector<unsigned char> zeros(32, 0);
  CHECK(crc32c(zeros.data(), zeros.size()) == 0x8A9136AAU);
  std::vector<unsigned char> ones(32, 0xFF);
  CHECK(crc32c(ones.data(), ones.size()) == 0x62A8AB43U);
  std::vector<unsigned char> inc(32);
  for (int i = 0; i < 32; ++i) inc[static_cast<std::size_t>(i)] = static_cast<unsigned char>(i);
  CHECK(crc32c(inc.data(), inc.size()) == 0x46DD794EU);

  // hardware and software must agree on odd lengths and seeds
  std::string buf;
  for (int i = 0; i < 1000; ++i) buf.push_back(static_cast<char>(i * 31 + 7));
  for (std::size_t len : {0U, 1U, 7U, 8U, 9U, 63U, 64U, 65U, 999U, 1000U}) {
    CHECK(crc32c(buf.data(), len) == crc32c_software(buf.data(), len));
  }
  // chaining with seed equals one-shot
  const std::uint32_t a = crc32c(buf.data(), 500);
  CHECK(crc32c(buf.data() + 500, 500, a) == crc32c(buf.data(), 1000));
  CHECK(crc32c_software(buf.data() + 500, 500, a) == crc32c_software(buf.data(), 1000));
  INFO("hardware crc32c: " << crc32c_hardware());
}
