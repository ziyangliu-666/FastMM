#pragma once
// Binance Spot request signing (6.4).
//
// REST  (https://github.com/binance/binance-spot-api-docs/blob/master/rest-api.md,
//        "SIGNED Endpoint security"): totalParams = query string + request body; signature =
//        HMAC-SHA256(secret, totalParams) as lowercase hex, sent as the `signature` query
//        parameter; the key travels in the `X-MBX-APIKEY` header. HMAC signatures are not
//        case-sensitive. `timestamp` is ms and `recvWindow` (default 5000, max 60000 ms).
// WS API (web-socket-api.md, "SIGNED request security"): sort params by name, join as
//        k=v&k=v, sign the same way, add `signature` to params. After `session.logon`
//        (Ed25519 keys only) apiKey/signature may be omitted from subsequent requests.
// Ed25519 (web-socket-api.md, "SIGNED Request Example (Ed25519)"): sign the UTF-8 payload
//        with the PKCS#8 PEM private key, base64-encode (case-sensitive).
//
// Secrets are wrapped in Secret<> so a log statement can never print them.
#include "fastmm/core/log.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/net/url.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::venues::binance {

enum class KeyType : std::uint8_t { Hmac = 0, Ed25519 = 1 };
[[nodiscard]] constexpr std::string_view to_string(KeyType t) noexcept {
  return t == KeyType::Hmac ? "hmac" : "ed25519";
}

struct Credentials {
  std::string api_key;
  Secret<std::string> secret{};           // HMAC secret (empty for Ed25519 keys)
  Secret<std::string> private_key_pem{};  // Ed25519 PKCS#8 PEM (empty for HMAC keys)
  KeyType type = KeyType::Hmac;

  // Presence only; Signer::usable() also requires the Ed25519 PEM to parse.
  [[nodiscard]] bool usable() const noexcept {
    if (api_key.empty()) return false;
    return type == KeyType::Hmac ? !secret.value.empty() : !private_key_pem.value.empty();
  }
};

// "X-MBX-APIKEY: <key>\r\n" header block for REST and WS upgrade requests.
inline std::string api_key_header(std::string_view api_key) {
  return "X-MBX-APIKEY: " + std::string(api_key) + "\r\n";
}

// Holds the parsed key material: the HMAC pad midstates (net::HmacSha256Key) or the parsed
// Ed25519 key, so no request re-reads the secret. Copies share the Ed25519 key.
class Signer {
 public:
  Signer() = default;
  explicit Signer(Credentials c) : creds_(std::move(c)) {
    if (creds_.type == KeyType::Hmac) {
      hmac_ = net::HmacSha256Key(creds_.secret.value);
    } else if (!creds_.private_key_pem.value.empty()) {
      auto k = net::Ed25519Key::from_private_pem(creds_.private_key_pem.value);
      if (k.has_private()) ed_ = std::make_shared<const net::Ed25519Key>(std::move(k));
    }
  }

  [[nodiscard]] const Credentials& credentials() const noexcept { return creds_; }
  [[nodiscard]] KeyType type() const noexcept { return creds_.type; }
  [[nodiscard]] std::string_view api_key() const noexcept { return creds_.api_key; }
  // HMAC: key and secret present. Ed25519: key present and the PEM parsed as an Ed25519
  // private key.
  [[nodiscard]] bool usable() const noexcept {
    if (creds_.api_key.empty()) return false;
    return creds_.type == KeyType::Hmac ? !creds_.secret.value.empty() : ed_ != nullptr;
  }

  // Hot path (HMAC keys): no allocation.
  [[nodiscard]] net::HexSha256 sign_hmac(std::string_view payload) const noexcept {
    return hmac_.sign_hex(payload);
  }
  // Ed25519: base64 signature into `out` (>= net::kEd25519Base64Size); 0 on failure.
  std::size_t sign_ed25519(std::string_view payload, std::span<char> out) const noexcept {
    return ed_ != nullptr ? ed_->sign_base64(payload, out) : 0;
  }
  // Control path: either key type; Ed25519 output is base64.
  [[nodiscard]] std::string sign(std::string_view payload) const {
    if (creds_.type == KeyType::Hmac) return std::string(sign_hmac(payload).view());
    return ed_ != nullptr ? ed_->sign_base64(payload) : std::string{};
  }

  // Appends "&signature=<sig>" to a REST query built with QueryBuilder. Percent-encodes
  // (base64 contains '+', '/', '='). Returns false on overflow (query left untouched).
  template <std::size_t N>
  bool sign_query(net::QueryBuilder<N>& q) const {
    if (!q.ok()) return false;
    if (creds_.type == KeyType::Hmac) {
      const net::HexSha256 sig = sign_hmac(q.view());
      q.add("signature", sig.view());
    } else {
      char sig[net::kEd25519Base64Size];
      const std::size_t n = sign_ed25519(q.view(), sig);
      if (n == 0) return false;
      q.add("signature", std::string_view(sig, n));
    }
    return q.ok();
  }

 private:
  Credentials creds_;
  net::HmacSha256Key hmac_{};
  std::shared_ptr<const net::Ed25519Key> ed_;
};

}  // namespace fastmm::venues::binance
