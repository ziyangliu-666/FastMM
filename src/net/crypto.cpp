// The low-level SHA256_* functions are deprecated in OpenSSL 3 but remain the only allocation-free
// way to keep and copy a midstate (HmacSha256Key).
#define OPENSSL_SUPPRESS_DEPRECATED
#include "fastmm/net/crypto.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

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

// ---- HMAC-SHA256 (RFC 2104) over precomputed pad midstates -----------------------------------

static_assert(sizeof(SHA256_CTX) <= HmacSha256Key::kCtxBytes);
static_assert(alignof(SHA256_CTX) <= 16);

HmacSha256Key::HmacSha256Key(std::string_view key) noexcept {
  constexpr std::size_t kBlock = 64;
  std::uint8_t k[kBlock] = {};
  if (key.size() > kBlock) {
    SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), k);
  } else if (!key.empty()) {
    std::memcpy(k, key.data(), key.size());
  }
  std::uint8_t pad[kBlock];
  SHA256_CTX c;
  for (std::size_t i = 0; i < kBlock; ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36U);
  SHA256_Init(&c);
  SHA256_Update(&c, pad, kBlock);
  std::memcpy(inner_.data(), &c, sizeof c);
  for (std::size_t i = 0; i < kBlock; ++i) pad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5cU);
  SHA256_Init(&c);
  SHA256_Update(&c, pad, kBlock);
  std::memcpy(outer_.data(), &c, sizeof c);
  OPENSSL_cleanse(k, sizeof k);
  OPENSSL_cleanse(pad, sizeof pad);
  OPENSSL_cleanse(&c, sizeof c);
}

void HmacSha256Key::sign(std::string_view data,
                         std::span<std::uint8_t, kSha256Size> out) const noexcept {
  SHA256_CTX c;
  std::memcpy(&c, inner_.data(), sizeof c);
  SHA256_Update(&c, data.data(), data.size());
  std::uint8_t ih[kSha256Size];
  SHA256_Final(ih, &c);
  std::memcpy(&c, outer_.data(), sizeof c);
  SHA256_Update(&c, ih, sizeof ih);
  SHA256_Final(out.data(), &c);
}

HexSha256 HmacSha256Key::sign_hex(std::string_view data) const noexcept {
  Sha256Digest d{};
  sign(data, d);
  return HexSha256(std::span<const std::uint8_t, kSha256Size>(d));
}

bool hmac_sha256(std::string_view key,
                 std::string_view data,
                 std::span<std::uint8_t, kSha256Size> out) noexcept {
  HmacSha256Key(key).sign(data, out);
  return true;
}

HexSha256 hmac_sha256_hex(std::string_view key, std::string_view data) noexcept {
  return HmacSha256Key(key).sign_hex(data);
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

std::size_t base64url_encode(std::span<const std::uint8_t> in, std::span<char> out) noexcept {
  // Standard base64 in place, then the URL alphabet and no padding.
  const std::size_t need = base64url_encoded_size(in.size());
  if (out.size() < need) return 0;
  std::size_t o = 0;
  std::size_t i = 0;
  auto put = [&](std::uint32_t v, int chars) {
    for (int k = 0; k < chars; ++k) {
      char c = kB64Alphabet[(v >> (18 - 6 * k)) & 63];
      if (c == '+') c = '-';
      if (c == '/') c = '_';
      out[o++] = c;
    }
  };
  for (; i + 3 <= in.size(); i += 3)
    put((static_cast<std::uint32_t>(in[i]) << 16) | (static_cast<std::uint32_t>(in[i + 1]) << 8) |
            static_cast<std::uint32_t>(in[i + 2]),
        4);
  const std::size_t rem = in.size() - i;
  if (rem == 1) put(static_cast<std::uint32_t>(in[i]) << 16, 2);
  if (rem == 2)
    put((static_cast<std::uint32_t>(in[i]) << 16) | (static_cast<std::uint32_t>(in[i + 1]) << 8),
        3);
  return o;
}

std::string base64url_encode(std::string_view in) {
  std::string s(base64url_encoded_size(in.size()), '\0');
  const std::size_t n = base64url_encode(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(in.data()), in.size()),
      std::span<char>(s));
  s.resize(n);
  return s;
}

std::size_t base64url_decode(std::string_view in, std::span<std::uint8_t> out) noexcept {
  constexpr std::size_t kBad = std::numeric_limits<std::size_t>::max();
  if (in.size() % 4 == 1 || in.size() > 4096) return kBad;
  char buf[4100];
  std::size_t n = 0;
  for (const char c : in) {
    if (c == '=' || c == '+' || c == '/') return kBad;
    buf[n++] = c == '-' ? '+' : c == '_' ? '/' : c;
  }
  while (n % 4 != 0) buf[n++] = '=';
  return base64_decode(std::string_view(buf, n), out);
}

bool random_bytes(std::span<std::uint8_t> out) noexcept {
  if (out.empty()) return true;
  return RAND_bytes(out.data(), static_cast<int>(out.size())) == 1;
}

std::string ed25519_sign_base64(std::string_view private_key_pem, std::string_view data) {
  return Ed25519Key::from_private_pem(private_key_pem).sign_base64(data);
}

// ---- Ed25519Key ------------------------------------------------------------------------------

namespace {
EVP_PKEY* as_pkey(void* p) noexcept {
  return static_cast<EVP_PKEY*>(p);
}
EVP_PKEY* read_pem(std::string_view pem, bool priv) noexcept {
  if (pem.empty() || pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return nullptr;
  std::unique_ptr<BIO, BioDeleter> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  if (!bio) return nullptr;
  EVP_PKEY* k = priv ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr)
                     : PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
  if (k != nullptr && EVP_PKEY_id(k) != EVP_PKEY_ED25519) {
    EVP_PKEY_free(k);
    return nullptr;
  }
  return k;
}
}  // namespace

Ed25519Key::~Ed25519Key() {
  EVP_PKEY_free(as_pkey(pkey_));
}

Ed25519Key& Ed25519Key::operator=(Ed25519Key&& o) noexcept {
  if (this != &o) {
    EVP_PKEY_free(as_pkey(pkey_));
    pkey_ = o.pkey_;
    private_ = o.private_;
    o.pkey_ = nullptr;
  }
  return *this;
}

Ed25519Key Ed25519Key::from_private_pem(std::string_view pem) {
  Ed25519Key k;
  k.pkey_ = read_pem(pem, true);
  k.private_ = k.pkey_ != nullptr;
  return k;
}

Ed25519Key Ed25519Key::from_public_pem(std::string_view pem) {
  Ed25519Key k;
  k.pkey_ = read_pem(pem, false);
  return k;
}

bool Ed25519Key::sign(std::string_view data,
                      std::span<std::uint8_t, kEd25519SignatureSize> out) const noexcept {
  if (!has_private()) return false;
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
  if (!ctx) return false;
  // Ed25519 is a one-shot "pure" signature: no digest.
  if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, as_pkey(pkey_)) != 1) return false;
  std::size_t len = out.size();
  return EVP_DigestSign(ctx.get(),
                        out.data(),
                        &len,
                        reinterpret_cast<const unsigned char*>(data.data()),
                        data.size()) == 1 &&
         len == kEd25519SignatureSize;
}

std::size_t Ed25519Key::sign_base64(std::string_view data, std::span<char> out) const noexcept {
  std::array<std::uint8_t, kEd25519SignatureSize> sig{};
  if (!sign(data, sig)) return 0;
  return base64_encode(std::span<const std::uint8_t>(sig), out);
}

std::string Ed25519Key::sign_base64(std::string_view data) const {
  char buf[kEd25519Base64Size];
  const std::size_t n = sign_base64(data, std::span<char>(buf));
  return std::string(buf, n);
}

bool Ed25519Key::verify(std::string_view data,
                        std::span<const std::uint8_t> signature) const noexcept {
  if (pkey_ == nullptr || signature.size() != kEd25519SignatureSize) return false;
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
  if (!ctx) return false;
  if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, as_pkey(pkey_)) != 1) return false;
  return EVP_DigestVerify(ctx.get(),
                          signature.data(),
                          signature.size(),
                          reinterpret_cast<const unsigned char*>(data.data()),
                          data.size()) == 1;
}

bool Ed25519Key::verify_base64(std::string_view data, std::string_view signature) const noexcept {
  std::array<std::uint8_t, kEd25519SignatureSize + 2> sig{};
  const std::size_t n = base64_decode(signature, std::span<std::uint8_t>(sig));
  if (n != kEd25519SignatureSize) return false;
  return verify(data, std::span<const std::uint8_t>(sig.data(), n));
}

std::string Ed25519Key::public_pem() const {
  if (pkey_ == nullptr) return {};
  std::unique_ptr<BIO, BioDeleter> bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_PUBKEY(bio.get(), as_pkey(pkey_)) != 1) return {};
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio.get(), &data);
  return len > 0 ? std::string(data, static_cast<std::size_t>(len)) : std::string{};
}

// ---- EcdsaP256Key -----------------------------------------------------------------------------

namespace {
struct EcdsaSigDeleter {
  void operator()(ECDSA_SIG* p) const noexcept { ECDSA_SIG_free(p); }
};
struct BnDeleter {
  void operator()(BIGNUM* p) const noexcept { BN_free(p); }
};
// A P-256 key from PEM, or nullptr.
EVP_PKEY* read_p256_pem(std::string_view pem, bool priv) noexcept {
  if (pem.empty() || pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return nullptr;
  std::unique_ptr<BIO, BioDeleter> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  if (!bio) return nullptr;
  EVP_PKEY* k = priv ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr)
                     : PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
  if (k == nullptr) return nullptr;
  char group[64] = {};
  std::size_t len = 0;
  if (EVP_PKEY_id(k) != EVP_PKEY_EC ||
      EVP_PKEY_get_utf8_string_param(k, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof group, &len) !=
          1 ||
      std::string_view(group, len) != "prime256v1") {
    EVP_PKEY_free(k);
    return nullptr;
  }
  return k;
}
}  // namespace

EcdsaP256Key::~EcdsaP256Key() {
  EVP_PKEY_free(as_pkey(pkey_));
}

EcdsaP256Key& EcdsaP256Key::operator=(EcdsaP256Key&& o) noexcept {
  if (this != &o) {
    EVP_PKEY_free(as_pkey(pkey_));
    pkey_ = o.pkey_;
    private_ = o.private_;
    o.pkey_ = nullptr;
  }
  return *this;
}

EcdsaP256Key EcdsaP256Key::from_private_pem(std::string_view pem) {
  EcdsaP256Key k;
  k.pkey_ = read_p256_pem(pem, true);
  k.private_ = k.pkey_ != nullptr;
  return k;
}

EcdsaP256Key EcdsaP256Key::from_public_pem(std::string_view pem) {
  EcdsaP256Key k;
  k.pkey_ = read_p256_pem(pem, false);
  return k;
}

bool EcdsaP256Key::sign(std::string_view data,
                        std::span<std::uint8_t, kEs256SignatureSize> out) const noexcept {
  if (!has_private()) return false;
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
  if (!ctx) return false;
  if (EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, as_pkey(pkey_)) != 1)
    return false;
  // A DER ECDSA-Sig-Value of P-256 is at most 72 bytes.
  std::array<unsigned char, 80> der{};
  std::size_t len = der.size();
  if (EVP_DigestSign(ctx.get(),
                     der.data(),
                     &len,
                     reinterpret_cast<const unsigned char*>(data.data()),
                     data.size()) != 1)
    return false;
  const unsigned char* p = der.data();
  std::unique_ptr<ECDSA_SIG, EcdsaSigDeleter> sig(
      d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(len)));
  if (!sig) return false;
  const BIGNUM* r = ECDSA_SIG_get0_r(sig.get());
  const BIGNUM* s = ECDSA_SIG_get0_s(sig.get());
  return BN_bn2binpad(r, out.data(), 32) == 32 && BN_bn2binpad(s, out.data() + 32, 32) == 32;
}

bool EcdsaP256Key::verify(std::string_view data,
                          std::span<const std::uint8_t> signature) const noexcept {
  if (pkey_ == nullptr || signature.size() != kEs256SignatureSize) return false;
  std::unique_ptr<ECDSA_SIG, EcdsaSigDeleter> sig(ECDSA_SIG_new());
  if (!sig) return false;
  std::unique_ptr<BIGNUM, BnDeleter> r(BN_bin2bn(signature.data(), 32, nullptr));
  std::unique_ptr<BIGNUM, BnDeleter> s(BN_bin2bn(signature.data() + 32, 32, nullptr));
  if (!r || !s || ECDSA_SIG_set0(sig.get(), r.get(), s.get()) != 1) return false;
  static_cast<void>(r.release());  // owned by sig now
  static_cast<void>(s.release());
  unsigned char* der = nullptr;
  const int der_len = i2d_ECDSA_SIG(sig.get(), &der);
  if (der_len <= 0) return false;
  std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx(EVP_MD_CTX_new());
  bool ok = ctx != nullptr &&
            EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, as_pkey(pkey_)) == 1 &&
            EVP_DigestVerify(ctx.get(),
                             der,
                             static_cast<std::size_t>(der_len),
                             reinterpret_cast<const unsigned char*>(data.data()),
                             data.size()) == 1;
  OPENSSL_free(der);
  return ok;
}

std::string EcdsaP256Key::public_pem() const {
  if (pkey_ == nullptr) return {};
  std::unique_ptr<BIO, BioDeleter> bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_PUBKEY(bio.get(), as_pkey(pkey_)) != 1) return {};
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio.get(), &data);
  return len > 0 ? std::string(data, static_cast<std::size_t>(len)) : std::string{};
}

}  // namespace fastmm::net
