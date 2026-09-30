#pragma once
// Coinbase Advanced Trade authentication with a CDP API key
// (https://docs.cdp.coinbase.com/coinbase-app/authentication-authorization/api-key-authentication,
// read 2026-09-30): every REST request carries "Authorization: Bearer <JWT>", and the user
// channel's subscribe message a "jwt" field.
//
//   header   {"alg":"ES256","kid":<key name>,"nonce":<random hex>,"typ":"JWT"}
//   payload  {"sub":<key name>,"iss":"cdp","nbf":<now>,"exp":<now + 120>,"uri":<method host path>}
//            (no "uri" for a WebSocket JWT; the path carries no query string)
//   JWT      base64url(header) "." base64url(payload) "." base64url(ES256 r || s)
//
// The key name is "organizations/<org>/apiKeys/<id>"; the private key is the EC P-256 PEM the
// CDP portal shows ("-----BEGIN EC PRIVATE KEY-----"). A .env file often holds it on one line with
// "\n" escapes: those are turned into newlines. Ed25519 CDP keys are refused by the Advanced Trade
// API ("Ed25519 (EdDSA) keys are NOT supported"), so the signer takes only ES256 keys.
// Control path: a signature allocates (OpenSSL).
#include "fastmm/core/log.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/decimal.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::venues::coinbase {

struct CdpCredentials {
  std::string key_name;               // organizations/<org>/apiKeys/<id>
  Secret<std::string> private_key{};  // EC PEM
  [[nodiscard]] bool present() const noexcept {
    return !key_name.empty() && !private_key.value.empty();
  }
};

// A PEM as a .env file may hold it: surrounding quotes dropped, "\n" escapes made newlines.
[[nodiscard]] inline std::string normalize_pem(std::string_view in) {
  if (in.size() >= 2 && (in.front() == '"' || in.front() == '\'') && in.back() == in.front())
    in = in.substr(1, in.size() - 2);
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\\' && i + 1 < in.size() && in[i + 1] == 'n') {
      out += '\n';
      ++i;
    } else {
      out += in[i];
    }
  }
  if (!out.empty() && out.back() != '\n') out += '\n';
  return out;
}

class CdpJwtSigner {
 public:
  static constexpr std::int64_t kLifetimeS = 120;

  CdpJwtSigner() = default;
  explicit CdpJwtSigner(CdpCredentials c) : creds_(std::move(c)) {
    if (!creds_.private_key.value.empty()) {
      std::string pem = normalize_pem(creds_.private_key.value);
      key_ = net::EcdsaP256Key::from_private_pem(pem);
      for (char& ch : pem) ch = '\0';
    }
  }

  [[nodiscard]] bool usable() const noexcept { return creds_.present() && key_.has_private(); }
  // Present but not an EC P-256 private key: a configuration error, not a missing key.
  [[nodiscard]] bool key_malformed() const noexcept {
    return creds_.present() && !key_.has_private();
  }
  [[nodiscard]] std::string_view key_name() const noexcept { return creds_.key_name; }

  // "GET api.coinbase.com/api/v3/brokerage/accounts": the query string is not part of it.
  [[nodiscard]] static std::string rest_uri(std::string_view method,
                                            std::string_view host,
                                            std::string_view target) {
    const std::size_t q = target.find('?');
    std::string uri(method);
    uri += ' ';
    uri += host;
    uri += target.substr(0, q);
    return uri;
  }

  // The JWT for a request made at `unix_s`; `uri` empty for a WebSocket message. Empty when the
  // signer is not usable or signing failed.
  [[nodiscard]] std::string sign(std::int64_t unix_s, std::string_view uri) const {
    if (!usable()) return {};
    std::array<std::uint8_t, 16> nonce_bytes{};
    if (!net::random_bytes(nonce_bytes)) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string nonce;
    for (const std::uint8_t b : nonce_bytes) {
      nonce += kHex[b >> 4];
      nonce += kHex[b & 15];
    }
    return sign_with_nonce(unix_s, uri, nonce);
  }

  // As sign(), with a given nonce (tests).
  [[nodiscard]] std::string sign_with_nonce(std::int64_t unix_s,
                                            std::string_view uri,
                                            std::string_view nonce) const {
    if (!usable()) return {};
    std::string header = R"({"alg":"ES256","kid":")";
    header += creds_.key_name;
    header += R"(","nonce":")";
    header += nonce;
    header += R"(","typ":"JWT"})";
    char num[24];
    std::string payload = R"({"sub":")";
    payload += creds_.key_name;
    payload += R"(","iss":"cdp","nbf":)";
    payload.append(num, format_int64(unix_s, num));
    payload += R"(,"exp":)";
    payload.append(num, format_int64(unix_s + kLifetimeS, num));
    if (!uri.empty()) {
      payload += R"(,"uri":")";
      payload += uri;
      payload += '"';
    }
    payload += '}';
    std::string jwt = net::base64url_encode(header);
    jwt += '.';
    jwt += net::base64url_encode(payload);
    std::array<std::uint8_t, net::kEs256SignatureSize> sig{};
    if (!key_.sign(jwt, sig)) return {};
    jwt += '.';
    jwt += net::base64url_encode(
        std::string_view(reinterpret_cast<const char*>(sig.data()), sig.size()));
    return jwt;
  }

 private:
  CdpCredentials creds_;
  net::EcdsaP256Key key_;
};

}  // namespace fastmm::venues::coinbase
