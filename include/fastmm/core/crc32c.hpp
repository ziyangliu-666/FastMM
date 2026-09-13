#pragma once
// CRC-32C (Castagnoli). Hardware SSE4.2 path selected at runtime, table fallback otherwise.
// Used for journal block integrity.
#include <cstddef>
#include <cstdint>

namespace fastmm {

// Standard CRC-32C with init 0xFFFFFFFF and final xor (matches e.g. Python's crc32c).
[[nodiscard]] std::uint32_t crc32c(const void* data,
                                   std::size_t len,
                                   std::uint32_t seed = 0) noexcept;
[[nodiscard]] bool crc32c_hardware() noexcept;
[[nodiscard]] std::uint32_t crc32c_software(const void* data,
                                            std::size_t len,
                                            std::uint32_t seed = 0) noexcept;

}  // namespace fastmm
