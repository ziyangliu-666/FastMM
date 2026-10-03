#pragma once
// Gate APIv4 request signing, for the USDT perpetual futures connector (gate_usdt_venue.hpp).
//
// REST (https://www.gate.com/docs/developers/apiv4/en/#apiv4-signed-request-requirements, read
// 2026-10-03): headers KEY, Timestamp (Unix seconds; at most 60 s from the venue's clock) and
//   SIGN = hex HMAC_SHA512(secret, METHOD "\n" path "\n" query "\n" hex SHA512(body) "\n" ts)
// where `path` includes the /api/v4 prefix, `query` is the query string exactly as sent (empty
// when there is none) and `body` the bytes sent (the SHA-512 of the empty string for GET).
// WebSocket subscriptions (futures WS docs, "Authentication"): the request carries
//   "auth":{"method":"api_key","KEY":key,"SIGN":hex HMAC_SHA512(secret,
//          "channel=" channel "&event=" event "&time=" time)}
// with `time` the request's Unix seconds. WebSocket API (futures.login and the order channels,
// "Futures Account Trade"): payload.signature = hex HMAC_SHA512(secret,
//   "api\n" channel "\n" req_param "\n" timestamp), req_param the empty string for the login.
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

namespace fastmm::venues::gate {

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

  // METHOD "\n" path "\n" query "\n" hex SHA512(body) "\n" ts. `path` is the full request path
  // (/api/v4/futures/usdt/orders), `query` without the '?'.
  [[nodiscard]] net::HexSha512 sign_rest(std::string_view method,
                                         std::string_view path,
                                         std::string_view query,
                                         std::string_view body,
                                         std::int64_t timestamp_s) const {
    std::string pre;
    pre.reserve(method.size() + path.size() + query.size() + 160);
    pre.append(method).push_back('\n');
    pre.append(path).push_back('\n');
    pre.append(query).push_back('\n');
    pre.append(net::sha512_hex(body).view()).push_back('\n');
    char buf[24];
    pre.append(buf, format_int64(timestamp_s, buf));
    return net::hmac_sha512_hex(creds_.secret.value, pre);
  }

  // "KEY: ..\r\nTimestamp: ..\r\nSIGN: ..\r\n" (and the JSON content type for a body).
  [[nodiscard]] std::string rest_headers(std::string_view method,
                                         std::string_view path,
                                         std::string_view query,
                                         std::string_view body,
                                         std::int64_t timestamp_s) const {
    const net::HexSha512 sig = sign_rest(method, path, query, body, timestamp_s);
    std::string h;
    h.reserve(256);
    char buf[24];
    h.append("KEY: ").append(creds_.api_key).append("\r\n");
    h.append("Timestamp: ").append(buf, format_int64(timestamp_s, buf)).append("\r\n");
    h.append("SIGN: ").append(sig.view()).append("\r\n");
    if (!body.empty()) h.append("Content-Type: application/json\r\n");
    return h;
  }

  // "channel=<channel>&event=<event>&time=<time>" (WebSocket subscription auth).
  [[nodiscard]] net::HexSha512 sign_ws_subscribe(std::string_view channel,
                                                 std::string_view event,
                                                 std::int64_t time_s) const {
    char buf[160];
    std::size_t n = 0;
    auto put = [&](std::string_view s) {
      for (char c : s) {
        if (n < sizeof buf) buf[n++] = c;
      }
    };
    put("channel=");
    put(channel);
    put("&event=");
    put(event);
    put("&time=");
    char t[24];
    put(std::string_view(t, format_int64(time_s, t)));
    return net::hmac_sha512_hex(creds_.secret.value, std::string_view(buf, n));
  }

  // "api\n<channel>\n<req_param>\n<timestamp>" (WebSocket API: login and order requests).
  [[nodiscard]] net::HexSha512 sign_ws_api(std::string_view channel,
                                           std::string_view req_param,
                                           std::int64_t time_s) const {
    std::string pre;
    pre.reserve(channel.size() + req_param.size() + 40);
    pre.append("api\n").append(channel).push_back('\n');
    pre.append(req_param).push_back('\n');
    char t[24];
    pre.append(t, format_int64(time_s, t));
    return net::hmac_sha512_hex(creds_.secret.value, pre);
  }

 private:
  Credentials creds_;
};

}  // namespace fastmm::venues::gate
