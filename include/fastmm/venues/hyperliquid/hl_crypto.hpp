#pragma once
// Hyperliquid L1 action signing, as hyperliquid-python-sdk 0.24.0 does it
// (hyperliquid/utils/signing.py: action_hash, construct_phantom_agent, l1_payload, sign_inner):
//
//   connectionId = keccak256(msgpack(action) || nonce as 8 bytes big endian
//                            || 0x00 (no vault) | 0x01 || the vault's 20 bytes
//                            [|| 0x00 || expiresAfter as 8 bytes big endian])
//   digest       = keccak256(0x19 0x01 || domainSeparator || hashStruct(Agent))
//     domain     EIP712Domain{name "Exchange", version "1", chainId 1337, verifyingContract 0x0}
//     Agent      {source "a" (mainnet) | "b" (testnet), connectionId bytes32}
//   signature    secp256k1 ECDSA over the digest, RFC 6979 nonce, low s; r and s as "0x" and
//                their hex without leading zeros (eth_utils.to_hex of an int), v = 27 + recovery id
//
// Keccak-256 is the original Keccak padding (0x01), not SHA3-256's. The signer holds an agent
// wallet's private key (an "API wallet" the account approved) or the account's own key; the key
// never leaves this object and is wiped when it is destroyed. Control path and order path:
// signing one digest takes some 30 us.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues::hyperliquid {

using Hash32 = std::array<std::uint8_t, 32>;

[[nodiscard]] Hash32 keccak256(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] Hash32 keccak256(std::string_view data) noexcept;

// The connectionId of an action whose msgpack encoding is `action`. `vault` is "0x" and 40 hex
// digits, or empty for none; `expires_after_ms` <= 0 for none.
[[nodiscard]] Hash32 connection_id(std::span<const std::uint8_t> action,
                                   std::uint64_t nonce,
                                   std::string_view vault = {},
                                   std::int64_t expires_after_ms = 0) noexcept;
// The EIP-712 digest a signer signs for an L1 action with this connectionId.
[[nodiscard]] Hash32 agent_digest(const Hash32& conn_id, bool mainnet) noexcept;

struct Signature {
  Hash32 r{};
  Hash32 s{};
  int v = 0;  // 27 or 28
};

// "0x" followed by the lowercase hex of a big-endian 256-bit number without leading zeros ("0x0"
// for zero), as eth_utils.to_hex writes an int. `out` holds at least kHexQuantityChars.
inline constexpr std::size_t kHexQuantityChars = 66;
std::size_t hex_quantity(const Hash32& v, char* out) noexcept;
// Lowercase hex of `bytes`, no prefix.
void hex_lower(std::span<const std::uint8_t> bytes, char* out) noexcept;
// "0x" + 2 * N hex digits (either case) -> bytes; false when it is not exactly that.
bool parse_hex(std::string_view s, std::span<std::uint8_t> out) noexcept;

class Signer {
 public:
  Signer();
  // A private key as 64 hex digits, with or without "0x". An invalid key leaves the signer
  // unusable with a reason in error().
  explicit Signer(std::string_view private_key_hex);
  ~Signer();
  Signer(const Signer&) = delete;
  Signer& operator=(const Signer&) = delete;
  Signer(Signer&&) noexcept;
  Signer& operator=(Signer&&) noexcept;

  [[nodiscard]] bool usable() const noexcept { return impl_ != nullptr; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // The key's address, "0x" and 40 lowercase hex digits; empty when unusable.
  [[nodiscard]] const std::string& address() const noexcept { return address_; }

  // Signs a 32-byte digest; false when the signer is unusable.
  bool sign(const Hash32& digest, Signature& out) const noexcept;
  // sign(agent_digest(connection_id(...), mainnet)).
  bool sign_action(std::span<const std::uint8_t> action_msgpack,
                   std::uint64_t nonce,
                   bool mainnet,
                   Signature& out,
                   std::string_view vault = {},
                   std::int64_t expires_after_ms = 0) const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string address_;
  std::string error_;
};

}  // namespace fastmm::venues::hyperliquid
