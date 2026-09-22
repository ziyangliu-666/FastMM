#pragma once
// Branch-free lowercase hex (SWAR): 8 nibbles to 8 characters in a few ALU operations.
#include <cstdint>

namespace fastmm {

// v's 8 nibbles as lowercase hex, most significant first when stored little-endian.
[[nodiscard]] constexpr std::uint64_t hex8(std::uint32_t v) noexcept {
  std::uint64_t x = v;
  x = ((x & 0xFFFF'0000ULL) << 16) | (x & 0xFFFFULL);
  x = ((x & 0x0000'FF00'0000'FF00ULL) << 8) | (x & 0x0000'00FF'0000'00FFULL);
  x = ((x & 0x00F0'00F0'00F0'00F0ULL) << 4) | (x & 0x000F'000F'000F'000FULL);
  // Byte i now holds nibble i: add '0', and 'a' - '0' - 10 more where the nibble is >= 10.
  const std::uint64_t alpha = ((x + 0x0606'0606'0606'0606ULL) >> 4) & 0x0101'0101'0101'0101ULL;
  x += 0x3030'3030'3030'3030ULL + alpha * static_cast<std::uint64_t>('a' - '0' - 10);
  return __builtin_bswap64(x);
}
static_assert(hex8(0x0123'abcdU) == 0x6463'6261'3332'3130ULL);  // "0123abcd"

}  // namespace fastmm
