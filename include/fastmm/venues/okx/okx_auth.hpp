#pragma once
// OKX v5 request signing (https://www.okx.com/docs-v5/en/#overview-rest-authentication and
// #overview-websocket-login, read 2026-09-26).
//
// REST: headers OK-ACCESS-KEY, OK-ACCESS-SIGN, OK-ACCESS-TIMESTAMP (ISO 8601 UTC with
//   milliseconds, "2020-12-08T09:08:57.715Z"), OK-ACCESS-PASSPHRASE;
//   sign = base64(HMAC_SHA256(secret, timestamp + method + requestPath + body)), where requestPath
//   carries the query string of a GET and body is the exact JSON sent with a POST. A request whose
//   timestamp is more than 30 s from the server's is refused (50102).
// WebSocket login: {"op":"login","args":[{"apiKey","passphrase","timestamp","sign"}]} with the
//   timestamp in Unix seconds and sign = base64(HMAC_SHA256(secret, timestamp + "GET" +
//   "/users/self/verify")).
// Demo trading adds the header "x-simulated-trading: 1" to every REST request.
//
// Three credentials: the API key, its secret and the passphrase chosen when the key was made.
// Secrets are wrapped in Secret<> so a log statement can never print them.
#include "fastmm/core/log.hpp"
#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/decimal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace fastmm::venues::okx {

struct Credentials {
  std::string api_key;
  Secret<std::string> secret{};
  Secret<std::string> passphrase{};
  [[nodiscard]] bool usable() const noexcept {
    return !api_key.empty() && !secret.value.empty() && !passphrase.value.empty();
  }
};

// base64 of a SHA-256 HMAC: 44 characters.
struct Base64Sha256 {
  std::array<char, 44> text{};
  [[nodiscard]] std::string_view view() const noexcept { return {text.data(), text.size()}; }
};

// "YYYY-MM-DDTHH:MM:SS.mmmZ" of a Unix time in milliseconds (24 characters). Days from the civil
// calendar per H. Hinnant's days_from_civil inverse; valid for any time after 1970.
struct IsoTimestamp {
  std::array<char, 24> text{};
  [[nodiscard]] std::string_view view() const noexcept { return {text.data(), text.size()}; }
};

[[nodiscard]] constexpr IsoTimestamp iso_timestamp(std::int64_t unix_ms) noexcept {
  IsoTimestamp out;
  if (unix_ms < 0) unix_ms = 0;
  const std::int64_t ms = unix_ms % 1000;
  const std::int64_t secs = unix_ms / 1000;
  const std::int64_t days = secs / 86400;
  const std::int64_t sod = secs % 86400;
  const std::int64_t z = days + 719468;
  const std::int64_t era = z / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  auto put = [&out](std::size_t at, std::int64_t v, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
      out.text[at + width - 1 - i] = static_cast<char>('0' + v % 10);
      v /= 10;
    }
  };
  put(0, y, 4);
  out.text[4] = '-';
  put(5, m, 2);
  out.text[7] = '-';
  put(8, d, 2);
  out.text[10] = 'T';
  put(11, sod / 3600, 2);
  out.text[13] = ':';
  put(14, (sod / 60) % 60, 2);
  out.text[16] = ':';
  put(17, sod % 60, 2);
  out.text[19] = '.';
  put(20, ms, 3);
  out.text[23] = 'Z';
  return out;
}

class Signer {
 public:
  Signer() = default;
  explicit Signer(Credentials c) : creds_(std::move(c)) {
    hmac_ = net::HmacSha256Key(creds_.secret.value);
  }

  [[nodiscard]] bool usable() const noexcept { return creds_.usable(); }
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
  [[nodiscard]] Base64Sha256 sign_rest(std::string_view iso_ts,
                                       std::string_view method,
                                       std::string_view request_path,
                                       std::string_view body) const {
    std::string pre;
    pre.reserve(iso_ts.size() + method.size() + request_path.size() + body.size());
    pre.append(iso_ts).append(method).append(request_path).append(body);
    return sign(pre);
  }

  // timestamp (seconds) + "GET" + "/users/self/verify".
  [[nodiscard]] Base64Sha256 sign_login(std::int64_t unix_s) const noexcept {
    char buf[64];
    const std::size_t n = format_int64(unix_s, buf);
    constexpr std::string_view kTail = "GET/users/self/verify";
    for (std::size_t i = 0; i < kTail.size(); ++i) buf[n + i] = kTail[i];
    return sign(std::string_view(buf, n + kTail.size()));
  }

  // The four OK-ACCESS-* headers, and the JSON content type with a body, "\r\n"-terminated.
  [[nodiscard]] std::string rest_headers(std::int64_t unix_ms,
                                         std::string_view method,
                                         std::string_view request_path,
                                         std::string_view body,
                                         bool simulated) const {
    const IsoTimestamp ts = iso_timestamp(unix_ms);
    const Base64Sha256 sig = sign_rest(ts.view(), method, request_path, body);
    std::string h;
    h.reserve(256);
    h.append("OK-ACCESS-KEY: ").append(creds_.api_key).append("\r\n");
    h.append("OK-ACCESS-SIGN: ").append(sig.view()).append("\r\n");
    h.append("OK-ACCESS-TIMESTAMP: ").append(ts.view()).append("\r\n");
    h.append("OK-ACCESS-PASSPHRASE: ").append(creds_.passphrase.value).append("\r\n");
    if (!body.empty()) h.append("Content-Type: application/json\r\n");
    if (simulated) h.append("x-simulated-trading: 1\r\n");
    return h;
  }

 private:
  Credentials creds_;
  net::HmacSha256Key hmac_{std::string_view{}};  // keyed from creds_.secret
};

}  // namespace fastmm::venues::okx
