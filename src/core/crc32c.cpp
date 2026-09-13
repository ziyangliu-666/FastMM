#include "fastmm/core/crc32c.hpp"

#include <nmmintrin.h>

#include <cstring>

namespace fastmm {

namespace {

struct Table {
  std::uint32_t t[256];
  constexpr Table() : t{} {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1U) != 0 ? (c >> 1) ^ 0x82F63B78U : c >> 1;
      t[i] = c;
    }
  }
};
constexpr Table kTable{};

__attribute__((target("sse4.2"))) std::uint32_t crc32c_hw(const void* data,
                                                          std::size_t len,
                                                          std::uint32_t seed) noexcept {
  const auto* p = static_cast<const unsigned char*>(data);
  std::uint64_t crc = ~seed;
  while (len >= 8) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 8);
    crc = _mm_crc32_u64(crc, v);
    p += 8;
    len -= 8;
  }
  auto c32 = static_cast<std::uint32_t>(crc);
  while (len > 0) {
    c32 = _mm_crc32_u8(c32, *p++);
    --len;
  }
  return ~c32;
}

using Fn = std::uint32_t (*)(const void*, std::size_t, std::uint32_t) noexcept;

Fn resolve() noexcept {
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2") ? &crc32c_hw : &crc32c_software;
}

Fn g_impl = resolve();

}  // namespace

std::uint32_t crc32c_software(const void* data, std::size_t len, std::uint32_t seed) noexcept {
  const auto* p = static_cast<const unsigned char*>(data);
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < len; ++i) crc = kTable.t[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8);
  return ~crc;
}

bool crc32c_hardware() noexcept {
  return g_impl == &crc32c_hw;
}

std::uint32_t crc32c(const void* data, std::size_t len, std::uint32_t seed) noexcept {
  return g_impl(data, len, seed);
}

}  // namespace fastmm
