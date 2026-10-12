#pragma once
// Lighter (zkLighter) transaction signing, as lighter-go and poseidon_crypto v0.0.15 do it
// (signer/key_manager.go, signature/schnorr, curve/ecgfp5, hash/poseidon2_goldilocks_plonky2):
//
//   field     Goldilocks, p = 2^64 - 2^32 + 1; Fp5 = Fp[x]/(x^5 - 3)
//   hash      Poseidon2 (width 12, rate 8, 8 full and 22 partial rounds, x^7), absorbing 8
//             elements at a time without padding; the first five outputs are an Fp5 element
//   curve     ECgFp5 (prime order n ~ 2^319); a point is encoded as one Fp5 element
//   key       a scalar mod n: 40 bytes little endian; the public key is Encode(sk * G)
//   sign      k random, r = Encode(k * G), e = H(r || m) as a scalar, s = k - e * sk;
//             signature = s || e, 40 bytes each, little endian
//
// A transaction's message m is the hash of its fields as field elements (lighter_tx.hpp). Every
// value is checked byte for byte against vectors the official SDK produced
// (tests/fixtures/lighter/signing_golden.json). Signing one message takes a fixed-base
// multiplication from a table built once per process, one Poseidon2 hash and two scalar
// products: some 100 us at -O2; with a nonce prepared ahead of time (prepare(), k and k * G off
// the hot path) the multiplication drops out and a transaction is its hashes, some 20 us.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::lighter {

using Fp5Bytes = std::array<std::uint8_t, 40>;     // an Fp5 element, canonical, little endian
using ScalarBytes = std::array<std::uint8_t, 40>;  // a scalar mod n, little endian
using SigBytes = std::array<std::uint8_t, 80>;     // s || e

// Poseidon2 of `elems` (raw u64 values, reduced mod p by the permutation), the first five
// outputs as an Fp5 element.
[[nodiscard]] Fp5Bytes hash_to_quintic(std::span<const std::uint64_t> elems) noexcept;
// The same over `bytes` read as 8-byte little-endian elements, the last one zero padded: the
// auth token's message.
[[nodiscard]] Fp5Bytes hash_bytes_to_quintic(std::string_view bytes) noexcept;

// Lowercase hex of `bytes`, no prefix; and its inverse ("0x" optional), false on a bad length or
// digit.
std::string to_hex(std::span<const std::uint8_t> bytes);
bool from_hex(std::string_view s, std::span<std::uint8_t> out) noexcept;

// Encode(k * G) for the scalar `k` (reduced mod n): the curve operations alone, for tests.
[[nodiscard]] Fp5Bytes generator_mul(const ScalarBytes& k) noexcept;

class Signer {
 public:
  Signer() = default;
  // The API key's private key: 80 hex digits (40 bytes little endian), "0x" optional. A bad key
  // leaves the signer unusable with the reason in error().
  explicit Signer(std::string_view private_key_hex);
  ~Signer();
  Signer(const Signer&) = delete;
  Signer& operator=(const Signer&) = delete;
  Signer(Signer&& o) noexcept;
  Signer& operator=(Signer&& o) noexcept;

  [[nodiscard]] bool usable() const noexcept { return usable_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // Encode(sk * G), 40 bytes; and its hex, as the venue's apikeys endpoint lists it.
  [[nodiscard]] const Fp5Bytes& public_key() const noexcept { return pk_; }
  [[nodiscard]] std::string public_key_hex() const { return to_hex(pk_); }

  // Signs a 40-byte message hash with a fresh random nonce, a prepared one when there is one;
  // false when unusable.
  bool sign(const Fp5Bytes& message, SigBytes& out) const noexcept;
  // Draws nonces and their k * G until `n` are ready (each is used once). Off the hot path.
  void prepare(std::size_t n);
  [[nodiscard]] std::size_t prepared() const noexcept { return pool_.size(); }
  // Signs with the given nonce (reduced mod n): reproduces the SDK's vectors. Tests only.
  bool sign_with_nonce(const Fp5Bytes& message, const ScalarBytes& k, SigBytes& out) const noexcept;

  // A new private key from the system's random source, as 80 hex digits.
  [[nodiscard]] static std::string generate_private_key_hex();

 private:
  struct Nonce {
    std::array<std::uint64_t, 5> k{};
    std::array<std::uint64_t, 5> r{};  // Encode(k * G)
  };
  std::array<std::uint64_t, 5> sk_{};
  Fp5Bytes pk_{};
  mutable std::vector<Nonce> pool_;
  bool usable_ = false;
  std::string error_;
};

// Checks a signature against a public key: s * G + e * pk encodes r with e = H(r || m).
[[nodiscard]] bool verify(const Fp5Bytes& public_key,
                          const Fp5Bytes& message,
                          const SigBytes& sig) noexcept;

}  // namespace fastmm::venues::lighter
