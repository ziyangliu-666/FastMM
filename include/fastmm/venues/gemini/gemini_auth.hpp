#pragma once
// Gemini request signing (https://developer.gemini.com/authentication/api-key.md and
// https://developer.gemini.com/websocket/authentication.md, read 2026-09-30).
//
// REST: every private request is a POST with an empty body. The JSON payload ({"request": path,
//   "nonce": n, ...parameters}) goes base64-encoded in X-GEMINI-PAYLOAD, with
//   X-GEMINI-SIGNATURE = hex(HMAC_SHA384(base64 payload, secret)), X-GEMINI-APIKEY,
//   Content-Type: text/plain, Content-Length: 0 and Cache-Control: no-cache. `request` must be the
//   path (EndpointMismatch otherwise).
// WebSocket (wss://ws.gemini.com): the upgrade request itself carries X-GEMINI-APIKEY,
//   X-GEMINI-NONCE, X-GEMINI-PAYLOAD = base64(nonce text) and X-GEMINI-SIGNATURE =
//   hex(HMAC_SHA384(payload, secret)); a connection cannot authenticate after it is open.
//
// Nonces. The WebSocket takes only account-scoped keys with a time-based nonce, so the key is one:
// "Unix epoch timestamps in seconds ... within +/- 30 seconds of server time". On the sandbox
// (2026-09-30) a time-based nonce must also increase ("Nonce '1790735176' has not increased since
// your last call"), and milliseconds are taken: the connector sends Unix milliseconds, each above
// the last, on REST and on the upgrade. Master keys (and their `account` payload field) are refused
// by the WebSocket and are not supported.
#include "fastmm/core/log.hpp"
#include "fastmm/net/crypto.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::venues::gemini {

struct Credentials {
  std::string api_key;
  Secret<std::string> secret{};
  [[nodiscard]] bool usable() const noexcept { return !api_key.empty() && !secret.value.empty(); }
};

class Signer {
 public:
  Signer() = default;
  explicit Signer(Credentials c) : creds_(std::move(c)) {}

  [[nodiscard]] bool usable() const noexcept { return creds_.usable(); }
  [[nodiscard]] std::string_view api_key() const noexcept { return creds_.api_key; }

  [[nodiscard]] net::HexSha384 sign(std::string_view payload_b64) const noexcept {
    return net::hmac_sha384_hex(creds_.secret.value, payload_b64);
  }

  // The headers of a private REST request whose JSON payload is `payload_json` (it must carry
  // "request" and "nonce"), "\r\n"-terminated.
  [[nodiscard]] std::string rest_headers(std::string_view payload_json) const {
    const std::string b64 = net::base64_encode(payload_json);
    std::string h;
    h.reserve(256 + 2 * b64.size());
    h.append("Content-Type: text/plain\r\nCache-Control: no-cache\r\n");
    h.append("X-GEMINI-APIKEY: ").append(creds_.api_key).append("\r\n");
    h.append("X-GEMINI-PAYLOAD: ").append(b64).append("\r\n");
    h.append("X-GEMINI-SIGNATURE: ").append(sign(b64).view()).append("\r\n");
    return h;
  }

  // The headers of an authenticated WebSocket upgrade with nonce `n`.
  [[nodiscard]] std::string ws_headers(std::int64_t n) const {
    const std::string nonce = std::to_string(n);
    const std::string b64 = net::base64_encode(nonce);
    std::string h;
    h.reserve(256);
    h.append("X-GEMINI-APIKEY: ").append(creds_.api_key).append("\r\n");
    h.append("X-GEMINI-NONCE: ").append(nonce).append("\r\n");
    h.append("X-GEMINI-PAYLOAD: ").append(b64).append("\r\n");
    h.append("X-GEMINI-SIGNATURE: ").append(sign(b64).view()).append("\r\n");
    return h;
  }

 private:
  Credentials creds_;
};

}  // namespace fastmm::venues::gemini
