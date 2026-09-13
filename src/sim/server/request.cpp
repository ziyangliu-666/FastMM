#include "fastmm/sim/server/request.hpp"

#include "fastmm/net/crypto.hpp"

#include <simdjson.h>

#include <algorithm>

namespace fastmm::sim::server {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

namespace {

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() &&
         (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
    s.remove_prefix(1);
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

// Splits "k=v&k=v" and calls fn(raw_key, raw_value) for every non-empty pair.
template <class Fn>
void for_each_pair(std::string_view text, Fn&& fn) {
  while (!text.empty()) {
    const std::size_t amp = text.find('&');
    const std::string_view pair = amp == std::string_view::npos ? text : text.substr(0, amp);
    if (!pair.empty()) {
      const std::size_t eq = pair.find('=');
      fn(eq == std::string_view::npos ? pair : pair.substr(0, eq),
         eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1));
    }
    if (amp == std::string_view::npos) break;
    text.remove_prefix(amp + 1);
  }
}

// JSON value -> parameter text: strings unescaped, numbers/booleans as written, arrays and
// objects as raw JSON, null as "".
bool value_text(od::value v, std::string& out) {
  od::json_type t{};
  if (v.type().get(t) != sj::SUCCESS) return false;
  switch (t) {
    case od::json_type::string: {
      std::string_view s;
      if (v.get_string().get(s) != sj::SUCCESS) return false;
      out.assign(s);
      return true;
    }
    case od::json_type::number:
    case od::json_type::boolean:
      out.assign(trim(v.raw_json_token()));
      return true;
    case od::json_type::null:
      out.clear();
      return true;
    case od::json_type::array:
    case od::json_type::object: {
      std::string_view raw;
      if (v.raw_json().get(raw) != sj::SUCCESS) return false;
      out.assign(trim(raw));
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

std::string ParamList::sorted_payload() const {
  std::vector<const Param*> sorted;
  sorted.reserve(items_.size());
  for (const Param& p : items_) {
    if (p.key != "signature") sorted.push_back(&p);
  }
  std::stable_sort(
      sorted.begin(), sorted.end(), [](const Param* a, const Param* b) { return a->key < b->key; });
  std::string out;
  for (const Param* p : sorted) {
    if (!out.empty()) out.push_back('&');
    out += p->key;
    out.push_back('=');
    out += p->value;
  }
  return out;
}

std::string percent_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '%' && i + 2 < in.size()) {
      const int hi = hex_value(in[i + 1]);
      const int lo = hex_value(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(c == '+' ? ' ' : c);
  }
  return out;
}

void parse_query(std::string_view query, ParamList& out) {
  for_each_pair(query, [&](std::string_view k, std::string_view v) {
    out.add(percent_decode(k), percent_decode(v));
  });
}

std::string rest_signature_payload(std::string_view query,
                                   std::string_view body,
                                   std::string& signature) {
  std::string payload;
  auto strip = [&](std::string_view text) {
    std::string part;
    for_each_pair(text, [&](std::string_view k, std::string_view v) {
      if (percent_decode(k) == "signature") {
        signature = percent_decode(v);
        return;
      }
      if (!part.empty()) part.push_back('&');
      part.append(k.data(), k.size());
      part.push_back('=');
      part.append(v.data(), v.size());
    });
    return part;
  };
  payload = strip(query);
  payload += strip(body);
  return payload;
}

bool verify_hmac_signature(std::string_view secret,
                           std::string_view payload,
                           std::string_view signature_hex) noexcept {
  const net::HexSha256 expected = net::hmac_sha256_hex(secret, payload);
  const std::string_view e = expected.view();
  if (signature_hex.size() != e.size()) return false;
  unsigned diff = 0;
  for (std::size_t i = 0; i < e.size(); ++i) {
    char c = signature_hex[i];
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    diff |= static_cast<unsigned>(static_cast<unsigned char>(c) ^ static_cast<unsigned char>(e[i]));
  }
  return diff == 0;
}

bool parse_ws_api_request(std::string_view text, WsApiRequest& out) {
  out = WsApiRequest{};
  const sj::padded_string padded(text);
  od::parser parser;
  od::document doc;
  if (parser.iterate(padded).get(doc) != sj::SUCCESS) return false;
  od::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return false;
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    od::value v;
    if (field.value().get(v) != sj::SUCCESS) return false;
    if (key == "id") {
      out.id_json.assign(trim(v.raw_json_token()));
      od::json_type t{};
      if (v.type().get(t) != sj::SUCCESS) return false;
      if (t == od::json_type::string) {
        std::string_view ignored;
        if (v.get_string().get(ignored) != sj::SUCCESS) return false;
      } else if (t == od::json_type::array || t == od::json_type::object) {
        return false;
      }
    } else if (key == "method") {
      std::string_view m;
      if (v.get_string().get(m) != sj::SUCCESS) return false;
      out.method.assign(m);
    } else if (key == "params") {
      od::object params;
      if (v.get_object().get(params) != sj::SUCCESS) return false;
      for (auto p : params) {
        std::string_view pk;
        if (p.unescaped_key().get(pk) != sj::SUCCESS) return false;
        od::value pv;
        if (p.value().get(pv) != sj::SUCCESS) return false;
        std::string value;
        if (!value_text(pv, value)) return false;
        out.params.add(std::string(pk), std::move(value));
      }
    }
  }
  return !out.method.empty();
}

bool parse_stream_control(std::string_view text, StreamControlRequest& out) {
  out = StreamControlRequest{};
  const sj::padded_string padded(text);
  od::parser parser;
  od::document doc;
  if (parser.iterate(padded).get(doc) != sj::SUCCESS) return false;
  od::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return false;
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    od::value v;
    if (field.value().get(v) != sj::SUCCESS) return false;
    if (key == "id") {
      out.id_json.assign(trim(v.raw_json_token()));
    } else if (key == "method") {
      std::string_view m;
      if (v.get_string().get(m) != sj::SUCCESS) return false;
      out.method.assign(m);
    } else if (key == "params") {
      od::array arr;
      if (v.get_array().get(arr) != sj::SUCCESS) return false;
      for (auto item : arr) {
        std::string_view s;
        if (item.get_string().get(s) != sj::SUCCESS) return false;
        out.params.emplace_back(s);
      }
    }
  }
  return !out.method.empty();
}

std::vector<std::string> parse_symbol_list(std::string_view text) {
  std::vector<std::string> out;
  text = trim(text);
  if (text.empty()) return out;
  if (text.front() != '[') {
    out.emplace_back(text);
    return out;
  }
  text.remove_prefix(1);
  if (!text.empty() && text.back() == ']') text.remove_suffix(1);
  while (!text.empty()) {
    const std::size_t comma = text.find(',');
    std::string_view item = trim(comma == std::string_view::npos ? text : text.substr(0, comma));
    if (item.size() >= 2 && item.front() == '"' && item.back() == '"')
      item = item.substr(1, item.size() - 2);
    if (!item.empty()) out.emplace_back(item);
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return out;
}

bool is_decimal_syntax(std::string_view s) noexcept {
  if (s.empty()) return false;
  std::size_t i = 0;
  std::size_t int_digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
    ++int_digits;
  }
  if (int_digits == 0 || int_digits > 20) return false;
  if (i == s.size()) return true;
  if (s[i] != '.') return false;
  ++i;
  std::size_t frac = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
    ++frac;
  }
  return i == s.size() && frac > 0 && frac <= 20;
}

std::optional<std::int64_t> parse_int(std::string_view s) noexcept {
  if (s.empty() || s.size() > 19) return std::nullopt;
  bool neg = false;
  std::size_t i = 0;
  if (s[0] == '-') {
    neg = true;
    i = 1;
    if (s.size() == 1) return std::nullopt;
  }
  std::int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    v = v * 10 + (s[i] - '0');
  }
  return neg ? -v : v;
}

}  // namespace fastmm::sim::server
