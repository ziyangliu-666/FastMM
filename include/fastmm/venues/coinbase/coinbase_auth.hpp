#pragma once
// Coinbase Exchange request signing (https://docs.cdp.coinbase.com/exchange/rest-api/authentication
// and .../exchange/websocket-feed/authentication, read 2026-09-30).
//
// REST: headers CB-ACCESS-KEY, CB-ACCESS-SIGN, CB-ACCESS-TIMESTAMP (seconds since the Unix epoch,
//   decimals allowed; refused when more than 30 s from the venue's time), CB-ACCESS-PASSPHRASE.
//   sign = base64(HMAC_SHA256(base64decode(secret), timestamp + method + requestPath + body)),
//   where requestPath carries the query string and body is the exact JSON sent.
// WebSocket: a subscribe message carrying "signature", "key", "passphrase" and "timestamp", signed
//   the same way over timestamp + "GET" + "/users/self/verify".
//
// Three credentials: the key, its secret (base64 of 64 bytes) and the passphrase chosen when the
// key was made. Secrets are wrapped in Secret<> so a log statement can never print them.
#include "fastmm/core/log.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/decimal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::venues::coinbase {

struct Credentials {
  std::string api_key;
  Secret<std::string> secret{};  // base64, as the venue shows it
  Secret<std::string> passphrase{};
  [[nodiscard]] bool present() const noexcept {
    return !api_key.empty() && !secret.value.empty() && !passphrase.value.empty();
  }
};

// base64 of a SHA-256 HMAC: 44 characters.
struct Base64Sha256 {
  std::array<char, 44> text{};
  [[nodiscard]] std::string_view view() const noexcept { return {text.data(), text.size()}; }
};

// "<seconds>.<mmm>" of a Unix time in milliseconds.
struct EpochSeconds {
  std::array<char, 32> text{};
  std::size_t n = 0;
  [[nodiscard]] std::string_view view() const noexcept { return {text.data(), n}; }
};

[[nodiscard]] inline EpochSeconds epoch_seconds(std::int64_t unix_ms, bool with_ms) noexcept {
  EpochSeconds out;
  if (unix_ms < 0) unix_ms = 0;
  out.n = format_int64(unix_ms / 1000, out.text.data());
  if (with_ms) {
    const std::int64_t ms = unix_ms % 1000;
    out.text[out.n++] = '.';
    out.text[out.n++] = static_cast<char>('0' + ms / 100);
    out.text[out.n++] = static_cast<char>('0' + (ms / 10) % 10);
    out.text[out.n++] = static_cast<char>('0' + ms % 10);
  }
  return out;
}

class Signer {
 public:
  Signer() = default;
  explicit Signer(Credentials c) : creds_(std::move(c)) {
    std::string key;
    decoded_ = !creds_.secret.value.empty() && net::base64_decode(creds_.secret.value, key);
    if (decoded_) hmac_ = net::HmacSha256Key(key);
    // The decoded key is not kept beyond the HMAC midstates.
    for (char& ch : key) ch = '\0';
  }

  // Key, secret and passphrase present, and the secret is base64.
  [[nodiscard]] bool usable() const noexcept { return creds_.present() && decoded_; }
  // Present but not base64: a configuration error, not a missing key.
  [[nodiscard]] bool secret_malformed() const noexcept { return creds_.present() && !decoded_; }
  [[nodiscard]] std::string_view api_key() const noexcept { return creds_.api_key; }
  [[nodiscard]] std::string_view passphrase() const noexcept { return creds_.passphrase.value; }

  [[nodiscard]] Base64Sha256 sign(std::string_view prehash) const noexcept {
    std::array<std::uint8_t, net::kSha256Size> mac{};
    hmac_.sign(prehash, mac);
    Base64Sha256 out;
    static_cast<void>(net::base64_encode(mac, out.text));
    return out;
  }

  // timestamp + method + requestPath + body.
  [[nodiscard]] Base64Sha256 sign_rest(std::string_view ts,
                                       std::string_view method,
                                       std::string_view request_path,
                                       std::string_view body) const {
    std::string pre;
    pre.reserve(ts.size() + method.size() + request_path.size() + body.size());
    pre.append(ts).append(method).append(request_path).append(body);
    return sign(pre);
  }

  // timestamp + "GET" + "/users/self/verify", for a WebSocket subscribe.
  [[nodiscard]] Base64Sha256 sign_ws(std::string_view ts) const noexcept {
    char buf[64];
    constexpr std::string_view kTail = "GET/users/self/verify";
    if (ts.size() + kTail.size() > sizeof buf) return {};
    std::size_t n = 0;
    for (const char ch : ts) buf[n++] = ch;
    for (const char ch : kTail) buf[n++] = ch;
    return sign(std::string_view(buf, n));
  }

  // The four CB-ACCESS-* headers, and the JSON content type with a body, "\r\n"-terminated.
  [[nodiscard]] std::string rest_headers(std::int64_t unix_ms,
                                         std::string_view method,
                                         std::string_view request_path,
                                         std::string_view body) const {
    const EpochSeconds ts = epoch_seconds(unix_ms, true);
    const Base64Sha256 sig = sign_rest(ts.view(), method, request_path, body);
    std::string h;
    h.reserve(256);
    h.append("CB-ACCESS-KEY: ").append(creds_.api_key).append("\r\n");
    h.append("CB-ACCESS-SIGN: ").append(sig.view()).append("\r\n");
    h.append("CB-ACCESS-TIMESTAMP: ").append(ts.view()).append("\r\n");
    h.append("CB-ACCESS-PASSPHRASE: ").append(creds_.passphrase.value).append("\r\n");
    if (!body.empty()) h.append("Content-Type: application/json\r\n");
    return h;
  }

 private:
  Credentials creds_;
  bool decoded_ = false;
  net::HmacSha256Key hmac_{std::string_view{}};
};

}  // namespace fastmm::venues::coinbase
