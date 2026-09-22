#pragma once
// Bybit v5 request signing (6.5).
//
// REST (https://bybit-exchange.github.io/docs/v5/guide, "Authentication"): headers
//   X-BAPI-API-KEY, X-BAPI-TIMESTAMP (ms), X-BAPI-SIGN, X-BAPI-RECV-WINDOW (default 5000);
//   sign = lowercase hex HMAC_SHA256(secret, timestamp + api_key + recv_window + payload)
//   where payload is the query string for GET and the JSON body string for POST. The body
//   must be signed exactly as sent, so callers sign the bytes they transmit. Valid when
//   server_time - recv_window <= timestamp < server_time + 1000.
// WebSocket (https://bybit-exchange.github.io/docs/v5/ws/connect, "Authentication"):
//   {"op":"auth","args":[api_key, expires_ms, hex HMAC_SHA256(secret, "GET/realtime" + expires)]}
//   with expires in milliseconds and greater than the current time.
//
// Secrets are wrapped in Secret<> so a log statement can never print them.
#include "fastmm/core/log.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/decimal.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::venues::bybit {

inline constexpr int kDefaultRecvWindowMs = 5000;  // docs default

struct Credentials {
  std::string api_key;
  Secret<std::string> secret{};
  [[nodiscard]] bool usable() const noexcept { return !api_key.empty() && !secret.value.empty(); }
};

class Signer {
 public:
  Signer() = default;
  explicit Signer(Credentials c) : creds_(std::move(c)) {
    hmac_ = net::HmacSha256Key(creds_.secret.value);
  }

  [[nodiscard]] bool usable() const noexcept { return creds_.usable(); }
  [[nodiscard]] std::string_view api_key() const noexcept { return creds_.api_key; }

  // timestamp + api_key + recv_window + payload, signed without building a std::string.
  [[nodiscard]] net::HexSha256 sign_rest(std::int64_t timestamp_ms,
                                         int recv_window_ms,
                                         std::string_view payload) const {
    std::string pre;
    pre.reserve(64 + creds_.api_key.size() + payload.size());
    char buf[24];
    pre.append(buf, format_int64(timestamp_ms, buf));
    pre.append(creds_.api_key);
    pre.append(buf, format_int64(recv_window_ms, buf));
    pre.append(payload);
    return hmac_.sign_hex(pre);
  }

  // "GET/realtime" + expires.
  [[nodiscard]] net::HexSha256 sign_ws_auth(std::int64_t expires_ms) const noexcept {
    char buf[48] = "GET/realtime";
    constexpr std::size_t kPrefix = 12;
    const std::size_t n = format_int64(expires_ms, buf + kPrefix);
    return hmac_.sign_hex(std::string_view(buf, kPrefix + n));
  }

  // "X-BAPI-API-KEY: ..\r\nX-BAPI-TIMESTAMP: ..\r\nX-BAPI-RECV-WINDOW: ..\r\nX-BAPI-SIGN: ..\r\n"
  [[nodiscard]] std::string rest_headers(std::int64_t timestamp_ms,
                                         int recv_window_ms,
                                         std::string_view payload,
                                         bool json_body) const {
    const net::HexSha256 sig = sign_rest(timestamp_ms, recv_window_ms, payload);
    std::string h;
    h.reserve(256);
    char buf[24];
    h.append("X-BAPI-API-KEY: ").append(creds_.api_key).append("\r\n");
    h.append("X-BAPI-TIMESTAMP: ").append(buf, format_int64(timestamp_ms, buf)).append("\r\n");
    h.append("X-BAPI-RECV-WINDOW: ").append(buf, format_int64(recv_window_ms, buf)).append("\r\n");
    h.append("X-BAPI-SIGN: ").append(sig.view()).append("\r\n");
    if (json_body) h.append("Content-Type: application/json\r\n");
    return h;
  }

 private:
  Credentials creds_;
  net::HmacSha256Key hmac_{std::string_view{}};  // keyed from creds_.secret
};

}  // namespace fastmm::venues::bybit
