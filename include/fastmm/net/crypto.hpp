#pragma once
// Cryptographic primitives needed by the venue connectors: HMAC-SHA256 (REST/WS signing),
// SHA-1 (WebSocket accept key), SHA-256, base64 (own implementation), CSPRNG bytes and an
// optional Ed25519 signer (Binance WS API session logon). Everything is OpenSSL 3 EVP-based
// but the header keeps OpenSSL out of the public interface.
#include "fastmm/core/hex.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::net {

inline constexpr std::size_t kSha1Size = 20;
inline constexpr std::size_t kSha256Size = 32;

using Sha1Digest = std::array<std::uint8_t, kSha1Size>;
using Sha256Digest = std::array<std::uint8_t, kSha256Size>;

// Fixed-capacity, NUL-terminated hex string (no heap). `size()` excludes the terminator.
template <std::size_t N>
class FixedHexString {
 public:
  static constexpr std::size_t kCapacity = 2 * N;

  constexpr FixedHexString() noexcept { data_[0] = '\0'; }
  explicit FixedHexString(std::span<const std::uint8_t, N> bytes) noexcept {
    static_assert(N % 4 == 0);
    for (std::size_t i = 0; i < N; i += 4) {  // 4 bytes -> 8 characters per step
      const std::uint32_t be = (std::uint32_t{bytes[i]} << 24) |
                               (std::uint32_t{bytes[i + 1]} << 16) |
                               (std::uint32_t{bytes[i + 2]} << 8) | std::uint32_t{bytes[i + 3]};
      const std::uint64_t hex = fastmm::hex8(be);
      std::memcpy(data_.data() + 2 * i, &hex, 8);
    }
    data_[kCapacity] = '\0';
  }

  std::string_view view() const noexcept { return {data_.data(), kCapacity}; }
  const char* c_str() const noexcept { return data_.data(); }
  constexpr std::size_t size() const noexcept { return kCapacity; }
  operator std::string_view() const noexcept {
    return view();
  }  // NOLINT(google-explicit-constructor)

 private:
  std::array<char, kCapacity + 1> data_{};
};

using HexSha256 = FixedHexString<kSha256Size>;

// HMAC-SHA256(key, data). `out` receives 32 bytes. Never throws; returns false on OpenSSL
// failure (should not happen for valid inputs).
bool hmac_sha256(std::string_view key,
                 std::string_view data,
                 std::span<std::uint8_t, kSha256Size> out) noexcept;
HexSha256 hmac_sha256_hex(std::string_view key, std::string_view data) noexcept;

// HMAC-SHA256 with the key schedule done once: the SHA-256 states after the ipad and opad
// blocks are kept, so signing a message is two SHA-256 runs over it and the digest, without
// the OpenSSL context allocation, algorithm fetch and key hashing hmac_sha256() pays per call.
// Same output as hmac_sha256(key, data).
class HmacSha256 {
 public:
  HmacSha256() noexcept = default;
  explicit HmacSha256(std::string_view key) noexcept;
  HmacSha256(const HmacSha256&) noexcept = default;
  HmacSha256& operator=(const HmacSha256&) noexcept = default;
  ~HmacSha256();

  [[nodiscard]] bool keyed() const noexcept { return keyed_; }
  // false when unkeyed.
  bool sign(std::string_view data, std::span<std::uint8_t, kSha256Size> out) const noexcept;
  [[nodiscard]] HexSha256 sign_hex(std::string_view data) const noexcept;

 private:
  static constexpr std::size_t kStateBytes = 128;  // >= sizeof(SHA256_CTX), checked in crypto.cpp
  alignas(16) std::array<std::uint8_t, kStateBytes> inner_{};
  alignas(16) std::array<std::uint8_t, kStateBytes> outer_{};
  bool keyed_ = false;
};

bool sha1(std::string_view data, std::span<std::uint8_t, kSha1Size> out) noexcept;
bool sha256(std::string_view data, std::span<std::uint8_t, kSha256Size> out) noexcept;

Sha1Digest sha1(std::string_view data) noexcept;
Sha256Digest sha256(std::string_view data) noexcept;

// RFC 4648 §4 base64 with '=' padding. `base64_encoded_size(n)` is the exact output size.
constexpr std::size_t base64_encoded_size(std::size_t n) noexcept {
  return ((n + 2) / 3) * 4;
}
constexpr std::size_t base64_max_decoded_size(std::size_t n) noexcept {
  return (n / 4) * 3 + 2;
}

// Writes into `out` (must be >= base64_encoded_size(in.size())); returns bytes written or 0 if
// the output is too small.
std::size_t base64_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept;
std::string base64_encode(std::span<const std::uint8_t> in);
std::string base64_encode(std::string_view in);

// Decodes standard base64 (padding required, no whitespace). Returns bytes written or
// SIZE_MAX on malformed input / insufficient output space.
std::size_t base64_decode(std::string_view in, std::span<std::uint8_t> out) noexcept;
bool base64_decode(std::string_view in, std::string& out);

// CSPRNG (RAND_bytes). Returns false if the RNG is unavailable.
bool random_bytes(std::span<std::uint8_t> out) noexcept;

// Ed25519 signature (base64) of `data` using a PEM-encoded PKCS#8 private key.
// Returns an empty string if the key cannot be loaded or is not Ed25519.
std::string ed25519_sign_base64(std::string_view private_key_pem, std::string_view data);

}  // namespace fastmm::net
