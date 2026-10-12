#include "fastmm/venues/hyperliquid/hl_crypto.hpp"

#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <sys/random.h>

#include <cstring>
#include <utility>

namespace fastmm::venues::hyperliquid {

namespace {

// ---- Keccak-f[1600] ---------------------------------------------------------------------------

constexpr std::uint64_t kRoundConstants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
constexpr int kRho[24] = {1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
                          27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44};
constexpr int kPi[24] = {10, 7,  11, 17, 18, 3, 5,  16, 8,  21, 24, 4,
                         15, 23, 19, 13, 12, 2, 20, 14, 22, 9,  6,  1};

constexpr std::uint64_t rotl(std::uint64_t x, int s) noexcept {
  return (x << s) | (x >> (64 - s));
}

void keccak_f(std::uint64_t st[25]) noexcept {
  for (const std::uint64_t rc : kRoundConstants) {
    std::uint64_t c[5];
    for (int i = 0; i < 5; ++i) c[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
    for (int i = 0; i < 5; ++i) {
      const std::uint64_t d = c[(i + 4) % 5] ^ rotl(c[(i + 1) % 5], 1);
      for (int j = 0; j < 25; j += 5) st[j + i] ^= d;
    }
    std::uint64_t t = st[1];
    for (int i = 0; i < 24; ++i) {
      const int j = kPi[i];
      const std::uint64_t tmp = st[j];
      st[j] = rotl(t, kRho[i]);
      t = tmp;
    }
    for (int j = 0; j < 25; j += 5) {
      std::uint64_t row[5];
      for (int i = 0; i < 5; ++i) row[i] = st[j + i];
      for (int i = 0; i < 5; ++i) st[j + i] = row[i] ^ (~row[(i + 1) % 5] & row[(i + 2) % 5]);
    }
    st[0] ^= rc;
  }
}

constexpr std::size_t kRate = 136;  // 1088-bit rate of Keccak-256

// Incremental Keccak-256 over several pieces.
class Keccak {
 public:
  void update(std::span<const std::uint8_t> data) noexcept {
    for (const std::uint8_t b : data) {
      st_[pos_ / 8] ^= static_cast<std::uint64_t>(b) << (8 * (pos_ % 8));
      if (++pos_ == kRate) {
        keccak_f(st_);
        pos_ = 0;
      }
    }
  }
  void update(std::string_view s) noexcept {
    update({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
  }
  [[nodiscard]] Hash32 finish() noexcept {
    st_[pos_ / 8] ^= std::uint64_t{0x01} << (8 * (pos_ % 8));
    st_[(kRate - 1) / 8] ^= std::uint64_t{0x80} << (8 * ((kRate - 1) % 8));
    keccak_f(st_);
    Hash32 out{};
    for (std::size_t i = 0; i < out.size(); ++i)
      out[i] = static_cast<std::uint8_t>(st_[i / 8] >> (8 * (i % 8)));
    return out;
  }

 private:
  std::uint64_t st_[25] = {};
  std::size_t pos_ = 0;
};

void put_u64_be(std::uint64_t v, std::uint8_t* out) noexcept {
  for (int i = 7; i >= 0; --i) {
    out[i] = static_cast<std::uint8_t>(v);
    v >>= 8;
  }
}

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)"
// ...): the domain separator, the same for mainnet and testnet.
Hash32 domain_separator() noexcept {
  Keccak k;
  k.update(keccak256(std::string_view(
      "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")));
  k.update(keccak256(std::string_view("Exchange")));
  k.update(keccak256(std::string_view("1")));
  std::uint8_t word[32] = {};
  word[30] = 0x05;  // 1337 = 0x0539
  word[31] = 0x39;
  k.update(word);
  const std::uint8_t zero[32] = {};  // verifyingContract 0x0, left-padded
  k.update(zero);
  return k.finish();
}

secp256k1_context* new_context() noexcept {
  secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
  if (ctx == nullptr) return nullptr;
  // Blinding against side channels; signing stays deterministic (RFC 6979).
  unsigned char seed[32];
  if (getrandom(seed, sizeof seed, 0) == static_cast<ssize_t>(sizeof seed) &&
      secp256k1_context_randomize(ctx, seed) != 1) {
    std::memset(seed, 0, sizeof seed);
    secp256k1_context_destroy(ctx);
    return nullptr;
  }
  std::memset(seed, 0, sizeof seed);
  return ctx;
}

}  // namespace

Hash32 keccak256(std::span<const std::uint8_t> data) noexcept {
  Keccak k;
  k.update(data);
  return k.finish();
}

Hash32 keccak256(std::string_view data) noexcept {
  Keccak k;
  k.update(data);
  return k.finish();
}

Hash32 connection_id(std::span<const std::uint8_t> action,
                     std::uint64_t nonce,
                     std::string_view vault,
                     std::int64_t expires_after_ms) noexcept {
  Keccak k;
  k.update(action);
  std::uint8_t buf[8];
  put_u64_be(nonce, buf);
  k.update(buf);
  std::uint8_t vault_bytes[20];
  if (!vault.empty() && parse_hex(vault, vault_bytes)) {
    const std::uint8_t one = 0x01;
    k.update({&one, 1});
    k.update(vault_bytes);
  } else {
    const std::uint8_t zero = 0x00;
    k.update({&zero, 1});
  }
  if (expires_after_ms > 0) {
    const std::uint8_t zero = 0x00;
    k.update({&zero, 1});
    put_u64_be(static_cast<std::uint64_t>(expires_after_ms), buf);
    k.update(buf);
  }
  return k.finish();
}

Hash32 agent_digest(const Hash32& conn_id, bool mainnet) noexcept {
  static const Hash32 domain = domain_separator();
  static const Hash32 agent_type =
      keccak256(std::string_view("Agent(string source,bytes32 connectionId)"));
  Keccak s;
  s.update(agent_type);
  s.update(keccak256(std::string_view(mainnet ? "a" : "b")));
  s.update(conn_id);
  const Hash32 struct_hash = s.finish();
  Keccak d;
  const std::uint8_t prefix[2] = {0x19, 0x01};
  d.update(prefix);
  d.update(domain);
  d.update(struct_hash);
  return d.finish();
}

std::size_t hex_quantity(const Hash32& v, char* out) noexcept {
  std::size_t n = 0;
  out[n++] = '0';
  out[n++] = 'x';
  bool started = false;
  for (const std::uint8_t b : v) {
    const char hi = kHexDigits[b >> 4];
    const char lo = kHexDigits[b & 0x0f];
    if (started || hi != '0') {
      out[n++] = hi;
      started = true;
    }
    if (started || lo != '0') {
      out[n++] = lo;
      started = true;
    }
  }
  if (!started) out[n++] = '0';
  return n;
}

void hex_lower(std::span<const std::uint8_t> bytes, char* out) noexcept {
  for (const std::uint8_t b : bytes) {
    *out++ = kHexDigits[b >> 4];
    *out++ = kHexDigits[b & 0x0f];
  }
}

bool parse_hex(std::string_view s, std::span<std::uint8_t> out) noexcept {
  if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2);
  if (s.size() != out.size() * 2) return false;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = hex_value(s[2 * i]);
    const int lo = hex_value(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return true;
}

// ---- Signer -----------------------------------------------------------------------------------

struct Signer::Impl {
  secp256k1_context* ctx = nullptr;
  std::uint8_t key[32] = {};
  ~Impl() {
    // A volatile wipe the compiler cannot drop as a dead store.
    volatile std::uint8_t* p = key;
    for (std::size_t i = 0; i < sizeof key; ++i) p[i] = 0;
    if (ctx != nullptr) secp256k1_context_destroy(ctx);
  }
};

Signer::Signer() = default;
Signer::~Signer() = default;
Signer::Signer(Signer&&) noexcept = default;
Signer& Signer::operator=(Signer&&) noexcept = default;

Signer::Signer(std::string_view private_key_hex) {
  auto impl = std::make_unique<Impl>();
  if (!parse_hex(private_key_hex, impl->key)) {
    error_ = "the private key is not 64 hex digits";
    return;
  }
  impl->ctx = new_context();
  if (impl->ctx == nullptr) {
    error_ = "secp256k1 context allocation failed";
    return;
  }
  if (secp256k1_ec_seckey_verify(impl->ctx, impl->key) != 1) {
    error_ = "the private key is not a valid secp256k1 key";
    return;
  }
  secp256k1_pubkey pub;
  if (secp256k1_ec_pubkey_create(impl->ctx, &pub, impl->key) != 1) {
    error_ = "could not derive the public key";
    return;
  }
  std::uint8_t ser[65];
  std::size_t len = sizeof ser;
  secp256k1_ec_pubkey_serialize(impl->ctx, ser, &len, &pub, SECP256K1_EC_UNCOMPRESSED);
  // The address: the last 20 bytes of keccak256 of the 64-byte public key (no 0x04 prefix).
  const Hash32 h = keccak256(std::span<const std::uint8_t>(ser + 1, 64));
  char hex[40];
  hex_lower(std::span<const std::uint8_t>(h.data() + 12, 20), hex);
  address_ = "0x" + std::string(hex, sizeof hex);
  impl_ = std::move(impl);
}

bool Signer::sign(const Hash32& digest, Signature& out) const noexcept {
  if (impl_ == nullptr) return false;
  secp256k1_ecdsa_recoverable_signature sig;
  if (secp256k1_ecdsa_sign_recoverable(
          impl_->ctx, &sig, digest.data(), impl_->key, nullptr, nullptr) != 1)
    return false;
  std::uint8_t compact[64];
  int recid = 0;
  secp256k1_ecdsa_recoverable_signature_serialize_compact(impl_->ctx, compact, &recid, &sig);
  std::memcpy(out.r.data(), compact, 32);
  std::memcpy(out.s.data(), compact + 32, 32);
  out.v = 27 + recid;
  return true;
}

bool Signer::sign_action(std::span<const std::uint8_t> action_msgpack,
                         std::uint64_t nonce,
                         bool mainnet,
                         Signature& out,
                         std::string_view vault,
                         std::int64_t expires_after_ms) const noexcept {
  return sign(agent_digest(connection_id(action_msgpack, nonce, vault, expires_after_ms), mainnet),
              out);
}

}  // namespace fastmm::venues::hyperliquid
