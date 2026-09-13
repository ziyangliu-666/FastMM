#include "fastmm/net/crypto.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include <cstring>
#include <limits>
#include <memory>

namespace fastmm::net {

namespace {

struct MdCtxDeleter {
  void operator()(EVP_MD_CTX* p) const noexcept { EVP_MD_CTX_free(p); }
};
struct PkeyDeleter {
  void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};
struct BioDeleter {
  void operator()(BIO* p) const noexcept { BIO_free(p); }
};
struct MacDeleter {
  void operator()(EVP_MAC* p) const noexcept { EVP_MAC_free(p); }
};
struct MacCtxDeleter {
  void operator()(EVP_MAC_CTX* p) const noexcept { EVP_MAC_CTX_free(p); }
};

bool digest(const EVP_MD* md, std::string_view data, std::span<std::uint8_t> out) noexcept {
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
  if (!ctx) return false;
  unsigned int len = 0;
  return EVP_DigestInit_ex(ctx.get(), md, nullptr) == 1 &&
         EVP_DigestUpdate(ctx.get(), data.data(), data.size()) == 1 &&
         EVP_DigestFinal_ex(ctx.get(), out.data(), &len) == 1 && len == out.size();
}

constexpr char kB64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 0..63 for valid symbols, 64 for '=', 255 for anything else.
constexpr std::array<std::uint8_t, 256> make_b64_reverse() {
  std::array<std::uint8_t, 256> t{};
  for (auto& v : t) v = 255;
  for (std::uint8_t i = 0; i < 64; ++i) t[static_cast<std::uint8_t>(kB64Alphabet[i])] = i;
  t[static_cast<std::uint8_t>('=')] = 64;
  return t;
}
constexpr std::array<std::uint8_t, 256> kB64Reverse = make_b64_reverse();

}  // namespace

bool hmac_sha256(std::string_view key,
                 std::string_view data,
                 std::span<std::uint8_t, kSha256Size> out) noexcept {
  // OpenSSL 3 EVP_MAC API (HMAC_* is deprecated in 3.0).
  std::unique_ptr<EVP_MAC, MacDeleter> mac(EVP_MAC_fetch(nullptr, "HMAC", nullptr));
  if (!mac) return false;
  std::unique_ptr<EVP_MAC_CTX, MacCtxDeleter> ctx(EVP_MAC_CTX_new(mac.get()));
  if (!ctx) return false;
  char digest_name[] = "SHA256";
  const OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string("digest", digest_name, 0),
                               OSSL_PARAM_construct_end()};
  std::size_t len = 0;
  return EVP_MAC_init(
             ctx.get(), reinterpret_cast<const unsigned char*>(key.data()), key.size(), params) ==
             1 &&
         EVP_MAC_update(
             ctx.get(), reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 1 &&
         EVP_MAC_final(ctx.get(), out.data(), &len, out.size()) == 1 && len == kSha256Size;
}

HexSha256 hmac_sha256_hex(std::string_view key, std::string_view data) noexcept {
  Sha256Digest d{};
  hmac_sha256(key, data, d);
  return HexSha256(std::span<const std::uint8_t, kSha256Size>(d));
}

bool sha1(std::string_view data, std::span<std::uint8_t, kSha1Size> out) noexcept {
  return digest(EVP_sha1(), data, out);
}

bool sha256(std::string_view data, std::span<std::uint8_t, kSha256Size> out) noexcept {
  return digest(EVP_sha256(), data, out);
}

Sha1Digest sha1(std::string_view data) noexcept {
  Sha1Digest d{};
  sha1(data, d);
  return d;
}

Sha256Digest sha256(std::string_view data) noexcept {
  Sha256Digest d{};
  sha256(data, d);
  return d;
}

std::size_t base64_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept {
  const std::size_t need = base64_encoded_size(in.size());
  if (out.size() < need) return 0;
  std::size_t o = 0;
  std::size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) |
                            (static_cast<std::uint32_t>(in[i + 1]) << 8) |
                            static_cast<std::uint32_t>(in[i + 2]);
    out[o++] = kB64Alphabet[(v >> 18) & 63];
    out[o++] = kB64Alphabet[(v >> 12) & 63];
    out[o++] = kB64Alphabet[(v >> 6) & 63];
    out[o++] = kB64Alphabet[v & 63];
  }
  const std::size_t rem = in.size() - i;
  if (rem == 1) {
    const std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16;
    out[o++] = kB64Alphabet[(v >> 18) & 63];
    out[o++] = kB64Alphabet[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  } else if (rem == 2) {
    const std::uint32_t v =
        (static_cast<std::uint32_t>(in[i]) << 16) | (static_cast<std::uint32_t>(in[i + 1]) << 8);
    out[o++] = kB64Alphabet[(v >> 18) & 63];
    out[o++] = kB64Alphabet[(v >> 12) & 63];
    out[o++] = kB64Alphabet[(v >> 6) & 63];
    out[o++] = '=';
  }
  return o;
}

std::string base64_encode(std::span<const std::uint8_t> in) {
  std::string s(base64_encoded_size(in.size()), '\0');
  base64_encode(in, std::span<char>(s));
  return s;
}

std::string base64_encode(std::string_view in) {
  return base64_encode(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(in.data()), in.size()));
}

std::size_t base64_decode(std::string_view in, std::span<std::uint8_t> out) noexcept {
  constexpr std::size_t kBad = std::numeric_limits<std::size_t>::max();
  if (in.size() % 4 != 0) return kBad;
  std::size_t o = 0;
  for (std::size_t i = 0; i < in.size(); i += 4) {
    const std::uint8_t a = kB64Reverse[static_cast<std::uint8_t>(in[i])];
    const std::uint8_t b = kB64Reverse[static_cast<std::uint8_t>(in[i + 1])];
    const std::uint8_t c = kB64Reverse[static_cast<std::uint8_t>(in[i + 2])];
    const std::uint8_t d = kB64Reverse[static_cast<std::uint8_t>(in[i + 3])];
    // Padding may only appear in the last quantum, and only as "x=" / "==".
    if (a >= 64 || b >= 64 || c == 255 || d == 255) return kBad;
    const bool last = i + 4 == in.size();
    if (c == 64 && d != 64) return kBad;
    if ((c == 64 || d == 64) && !last) return kBad;
    const std::size_t produced = (c == 64) ? 1 : (d == 64) ? 2 : 3;
    if (o + produced > out.size()) return kBad;
    const std::uint32_t v =
        (static_cast<std::uint32_t>(a) << 18) | (static_cast<std::uint32_t>(b) << 12) |
        (static_cast<std::uint32_t>(c & 63) << 6) | static_cast<std::uint32_t>(d & 63);
    out[o++] = static_cast<std::uint8_t>(v >> 16);
    if (produced > 1) out[o++] = static_cast<std::uint8_t>(v >> 8);
    if (produced > 2) out[o++] = static_cast<std::uint8_t>(v);
  }
  return o;
}

bool base64_decode(std::string_view in, std::string& out) {
  out.resize(base64_max_decoded_size(in.size()));
  const std::size_t n = base64_decode(
      in, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(out.data()), out.size()));
  if (n == std::numeric_limits<std::size_t>::max()) {
    out.clear();
    return false;
  }
  out.resize(n);
  return true;
}

bool random_bytes(std::span<std::uint8_t> out) noexcept {
  if (out.empty()) return true;
  return RAND_bytes(out.data(), static_cast<int>(out.size())) == 1;
}

std::string ed25519_sign_base64(std::string_view private_key_pem, std::string_view data) {
  if (private_key_pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
  std::unique_ptr<BIO, BioDeleter> bio(
      BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size())));
  if (!bio) return {};
  std::unique_ptr<EVP_PKEY, PkeyDeleter> key(
      PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
  if (!key || EVP_PKEY_id(key.get()) != EVP_PKEY_ED25519) return {};
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
  if (!ctx) return {};
  // Ed25519 is a one-shot "pure" signature: no digest, use EVP_DigestSign directly.
  if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) return {};
  std::size_t sig_len = 0;
  const auto* msg = reinterpret_cast<const unsigned char*>(data.data());
  if (EVP_DigestSign(ctx.get(), nullptr, &sig_len, msg, data.size()) != 1) return {};
  std::array<std::uint8_t, 64> sig{};
  if (sig_len != sig.size()) return {};
  if (EVP_DigestSign(ctx.get(), sig.data(), &sig_len, msg, data.size()) != 1) return {};
  return base64_encode(std::span<const std::uint8_t>(sig.data(), sig_len));
}

}  // namespace fastmm::net
