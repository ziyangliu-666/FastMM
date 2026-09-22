#pragma once
// Cryptographic primitives needed by the venue connectors: HMAC-SHA256 (REST/WS signing),
// SHA-1 (WebSocket accept key), SHA-256, base64 (own implementation), CSPRNG bytes and Ed25519
// (Binance session logon / Ed25519 API keys). OpenSSL 3 underneath; the header keeps OpenSSL out
// of the public interface.
//
// HmacSha256Key hashes the key pads once and keeps the inner/outer SHA-256 midstates, so a
// signature costs the message blocks plus two compressions, with no allocation and no provider
// lookup (OpenSSL's SHA-256 block function uses SHA-NI / AVX2 when the CPU has them).
#include <array>
#include <cstddef>
#include <cstdint>
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
    static constexpr char kDigits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < N; ++i) {
      data_[2 * i] = kDigits[bytes[i] >> 4];
      data_[2 * i + 1] = kDigits[bytes[i] & 0x0F];
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

// HMAC-SHA256 with the key schedule precomputed (see the header comment). Copyable; a
// default-constructed key signs with the empty key.
class HmacSha256Key {
 public:
  HmacSha256Key() noexcept : HmacSha256Key(std::string_view{}) {}
  explicit HmacSha256Key(std::string_view key) noexcept;

  void sign(std::string_view data, std::span<std::uint8_t, kSha256Size> out) const noexcept;
  [[nodiscard]] HexSha256 sign_hex(std::string_view data) const noexcept;

  // Opaque storage for one OpenSSL SHA256_CTX (112 bytes; checked in crypto.cpp).
  static constexpr std::size_t kCtxBytes = 128;

 private:
  alignas(16) std::array<std::uint8_t, kCtxBytes> inner_{};
  alignas(16) std::array<std::uint8_t, kCtxBytes> outer_{};
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
// Returns an empty string if the key cannot be loaded or is not Ed25519. Parses the PEM on
// every call: keep an Ed25519Key for repeated signing.
std::string ed25519_sign_base64(std::string_view private_key_pem, std::string_view data);

inline constexpr std::size_t kEd25519SignatureSize = 64;
inline constexpr std::size_t kEd25519Base64Size = 88;  // base64_encoded_size(64)

// A parsed Ed25519 key (private, or public only for verification). Move-only.
class Ed25519Key {
 public:
  Ed25519Key() noexcept = default;
  ~Ed25519Key();
  Ed25519Key(Ed25519Key&& o) noexcept : pkey_(o.pkey_), private_(o.private_) { o.pkey_ = nullptr; }
  Ed25519Key& operator=(Ed25519Key&& o) noexcept;
  Ed25519Key(const Ed25519Key&) = delete;
  Ed25519Key& operator=(const Ed25519Key&) = delete;

  // PKCS#8 "PRIVATE KEY" PEM. Not valid() if the PEM is not an Ed25519 private key.
  [[nodiscard]] static Ed25519Key from_private_pem(std::string_view pem);
  // SubjectPublicKeyInfo "PUBLIC KEY" PEM (what Binance asks for when registering the key).
  [[nodiscard]] static Ed25519Key from_public_pem(std::string_view pem);

  [[nodiscard]] bool valid() const noexcept { return pkey_ != nullptr; }
  [[nodiscard]] bool has_private() const noexcept { return pkey_ != nullptr && private_; }

  // Writes the 64-byte signature; false without a private key.
  bool sign(std::string_view data,
            std::span<std::uint8_t, kEd25519SignatureSize> out) const noexcept;
  // Base64 signature into `out` (>= kEd25519Base64Size); returns bytes written, 0 on failure.
  std::size_t sign_base64(std::string_view data, std::span<char> out) const noexcept;
  [[nodiscard]] std::string sign_base64(std::string_view data) const;
  [[nodiscard]] bool verify(std::string_view data,
                            std::span<const std::uint8_t> signature) const noexcept;
  // Verifies a base64 signature (Binance's `signature` parameter).
  [[nodiscard]] bool verify_base64(std::string_view data,
                                   std::string_view signature) const noexcept;
  // "-----BEGIN PUBLIC KEY-----" PEM of the key (empty when invalid).
  [[nodiscard]] std::string public_pem() const;

 private:
  void* pkey_ = nullptr;  // EVP_PKEY*
  bool private_ = false;
};

}  // namespace fastmm::net
