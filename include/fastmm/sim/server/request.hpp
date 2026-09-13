#pragma once
// Request parsing for the simulated exchange: REST query strings / form bodies, WebSocket API
// requests ({"id","method","params"}), market-data stream control frames (SUBSCRIBE) and
// Binance HMAC-SHA256 signature verification. JSON parsing uses simdjson inside request.cpp;
// nothing here exposes it.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::sim::server {

struct Param {
  std::string key;
  std::string value;
};

// Ordered list of request parameters (duplicates keep the first occurrence for lookups).
class ParamList {
 public:
  void add(std::string key, std::string value) {
    items_.push_back(Param{std::move(key), std::move(value)});
  }
  [[nodiscard]] const std::string* find(std::string_view key) const noexcept {
    for (const Param& p : items_) {
      if (p.key == key) return &p.value;
    }
    return nullptr;
  }
  [[nodiscard]] std::string_view get(std::string_view key) const noexcept {
    const std::string* v = find(key);
    return v == nullptr ? std::string_view{} : std::string_view(*v);
  }
  // Present and non-empty.
  [[nodiscard]] bool has(std::string_view key) const noexcept { return !get(key).empty(); }
  [[nodiscard]] std::span<const Param> items() const noexcept { return items_; }
  [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

  // WebSocket API signature payload: "k=v&k=v" over the params sorted by key, `signature`
  // excluded, values as sent (web-socket-api.md "SIGNED request security").
  [[nodiscard]] std::string sorted_payload() const;

 private:
  std::vector<Param> items_;
};

[[nodiscard]] std::string percent_decode(std::string_view in);
// Parses "a=1&b=%5B%5D" into `out` (appending; keys and values percent-decoded).
void parse_query(std::string_view query, ParamList& out);

// REST signature payload (rest-api.md "SIGNED endpoint security"): totalParams = query string
// concatenated with the request body, with the `signature` parameter removed; the decoded
// signature value is returned through `signature`.
[[nodiscard]] std::string rest_signature_payload(std::string_view query,
                                                 std::string_view body,
                                                 std::string& signature);

// Constant-time-ish, case-insensitive comparison of hex HMAC-SHA256(secret, payload).
[[nodiscard]] bool verify_hmac_signature(std::string_view secret,
                                         std::string_view payload,
                                         std::string_view signature_hex) noexcept;

struct WsApiRequest {
  std::string id_json = "null";  // raw JSON token of "id", echoed back verbatim
  std::string method;
  ParamList params;  // numbers/booleans as their JSON text, arrays as raw JSON
};
// False for invalid JSON or a non-object frame; `id_json` is still filled when readable.
[[nodiscard]] bool parse_ws_api_request(std::string_view text, WsApiRequest& out);

// {"method":"SUBSCRIBE","params":["btcusdt@trade"],"id":1}
struct StreamControlRequest {
  std::string id_json = "null";
  std::string method;
  std::vector<std::string> params;
};
[[nodiscard]] bool parse_stream_control(std::string_view text, StreamControlRequest& out);

// Parses a JSON array of strings (`["BTCUSDT","ETHUSDT"]`) or a bare symbol.
[[nodiscard]] std::vector<std::string> parse_symbol_list(std::string_view text);

// Strict decimal syntax check ("123", "0.001"): digits, optional '.' and digits.
[[nodiscard]] bool is_decimal_syntax(std::string_view s) noexcept;
[[nodiscard]] std::optional<std::int64_t> parse_int(std::string_view s) noexcept;

}  // namespace fastmm::sim::server
