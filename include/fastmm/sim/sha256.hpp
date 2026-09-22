#pragma once
// Minimal streaming SHA-256 (FIPS 180-4) for the replay proof: the outbound message stream
// of a run is hashed and compared with the journal's. Every simulated order goes through it,
// so it uses the SHA-NI instructions when the CPU has them (runtime check) and a portable
// implementation otherwise. Kept in fastmm::sim so the simulator does not depend on
// fastmm::net's OpenSSL wrapper.
#include <cstddef>
#include <cstdint>
#include <string>

namespace fastmm::sim {

class Sha256 {
 public:
  Sha256() noexcept { reset(); }
  void reset() noexcept;
  void update(const void* data, std::size_t len) noexcept;
  // Finalizes a copy: the object may keep receiving updates.
  void digest(std::uint8_t out[32]) const noexcept;
  [[nodiscard]] std::string hex() const;

 private:
  void transform(const std::uint8_t* blocks, std::size_t n) noexcept;
  std::uint32_t h_[8];
  std::uint8_t buf_[64];
  std::size_t buf_len_ = 0;
  std::uint64_t total_ = 0;
};

namespace detail {
// Both compress `blocks` 64-byte blocks into h; exposed for tests.
[[nodiscard]] bool sha256_hardware_available() noexcept;
void sha256_blocks_hardware(std::uint32_t h[8],
                            const std::uint8_t* data,
                            std::size_t blocks) noexcept;
void sha256_blocks_portable(std::uint32_t h[8],
                            const std::uint8_t* data,
                            std::size_t blocks) noexcept;
}  // namespace detail

}  // namespace fastmm::sim
