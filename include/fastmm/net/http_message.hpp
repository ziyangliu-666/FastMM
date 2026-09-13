#pragma once
// HTTP/1.1 message heads (request line / status line + headers) parsed into views over the
// caller's buffer. Shared by HttpClient, HttpServer and the WebSocket handshake.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fastmm::net {

inline constexpr std::size_t kMaxHttpHeaders = 32;

constexpr char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

// True if `value` is a comma-separated token list containing `token` (case-insensitive),
// e.g. Connection: keep-alive, Upgrade.
inline bool header_has_token(std::string_view value, std::string_view token) noexcept {
  while (!value.empty()) {
    const std::size_t comma = value.find(',');
    std::string_view item = comma == std::string_view::npos ? value : value.substr(0, comma);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
    if (iequals(item, token)) return true;
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return false;
}

inline std::string_view trim_ows(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

struct HttpHeader {
  std::string_view name;
  std::string_view value;
};

struct HttpHeaders {
  std::array<HttpHeader, kMaxHttpHeaders> items{};
  std::size_t count = 0;

  // First matching header value (case-insensitive name) or empty.
  std::string_view get(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      if (iequals(items[i].name, name)) return items[i].value;
    }
    return {};
  }
  bool has(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      if (iequals(items[i].name, name)) return true;
    }
    return false;
  }
  std::span<const HttpHeader> view() const noexcept { return {items.data(), count}; }
};

enum class HttpParseStatus : std::uint8_t { Ok, Incomplete, Invalid, TooManyHeaders };

struct HttpRequestHead {
  std::string_view method;
  std::string_view target;  // as sent: path?query
  std::string_view path;
  std::string_view query;
  std::string_view version;  // "HTTP/1.1"
  HttpHeaders headers;
  std::size_t head_len = 0;  // bytes up to and including the blank line
};

struct HttpResponseHead {
  int status = 0;
  std::string_view reason;
  std::string_view version;
  HttpHeaders headers;
  std::size_t head_len = 0;
};

// Position just past "\r\n\r\n", or npos if the head is incomplete.
inline std::size_t find_head_end(std::string_view in) noexcept {
  const std::size_t p = in.find("\r\n\r\n");
  return p == std::string_view::npos ? p : p + 4;
}

// Parses "Name: value\r\n" lines in [pos, end) into `out`. `end` points at the blank line.
inline HttpParseStatus parse_header_lines(std::string_view in,
                                          std::size_t pos,
                                          std::size_t end,
                                          HttpHeaders& out) noexcept {
  out.count = 0;
  while (pos < end) {
    const std::size_t eol = in.find("\r\n", pos);
    if (eol == std::string_view::npos || eol > end) return HttpParseStatus::Invalid;
    if (eol == pos) break;  // blank line
    const std::string_view line = in.substr(pos, eol - pos);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return HttpParseStatus::Invalid;
    if (out.count >= kMaxHttpHeaders) return HttpParseStatus::TooManyHeaders;
    out.items[out.count++] = HttpHeader{line.substr(0, colon), trim_ows(line.substr(colon + 1))};
    pos = eol + 2;
  }
  return HttpParseStatus::Ok;
}

inline HttpParseStatus parse_request_head(std::string_view in, HttpRequestHead& out) noexcept {
  const std::size_t end = find_head_end(in);
  if (end == std::string_view::npos) return HttpParseStatus::Incomplete;
  const std::size_t eol = in.find("\r\n");
  const std::string_view line = in.substr(0, eol);
  const std::size_t sp1 = line.find(' ');
  if (sp1 == std::string_view::npos) return HttpParseStatus::Invalid;
  const std::size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos) return HttpParseStatus::Invalid;
  out.method = line.substr(0, sp1);
  out.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  out.version = line.substr(sp2 + 1);
  if (out.method.empty() || out.target.empty() || out.version.substr(0, 5) != "HTTP/")
    return HttpParseStatus::Invalid;
  const std::size_t q = out.target.find('?');
  out.path = q == std::string_view::npos ? out.target : out.target.substr(0, q);
  out.query = q == std::string_view::npos ? std::string_view{} : out.target.substr(q + 1);
  const HttpParseStatus st = parse_header_lines(in, eol + 2, end - 2, out.headers);
  if (st != HttpParseStatus::Ok) return st;
  out.head_len = end;
  return HttpParseStatus::Ok;
}

inline HttpParseStatus parse_response_head(std::string_view in, HttpResponseHead& out) noexcept {
  const std::size_t end = find_head_end(in);
  if (end == std::string_view::npos) return HttpParseStatus::Incomplete;
  const std::size_t eol = in.find("\r\n");
  const std::string_view line = in.substr(0, eol);
  // "HTTP/1.1 200 OK" - reason phrase may be empty.
  if (line.size() < 12 || line.substr(0, 5) != "HTTP/" || line[8] != ' ')
    return HttpParseStatus::Invalid;
  out.version = line.substr(0, 8);
  int status = 0;
  for (std::size_t i = 9; i < 12; ++i) {
    if (line[i] < '0' || line[i] > '9') return HttpParseStatus::Invalid;
    status = status * 10 + (line[i] - '0');
  }
  out.status = status;
  out.reason = line.size() > 13 ? line.substr(13) : std::string_view{};
  const HttpParseStatus st = parse_header_lines(in, eol + 2, end - 2, out.headers);
  if (st != HttpParseStatus::Ok) return st;
  out.head_len = end;
  return HttpParseStatus::Ok;
}

// Decodes a decimal Content-Length; false on garbage or overflow.
inline bool parse_content_length(std::string_view v, std::size_t& out) noexcept {
  v = trim_ows(v);
  if (v.empty() || v.size() > 18) return false;
  std::size_t n = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<std::size_t>(c - '0');
  }
  out = n;
  return true;
}

// Looks up `key` in a query string ("a=1&b=2"); values are returned raw (not decoded).
inline std::string_view query_param(std::string_view query, std::string_view key) noexcept {
  while (!query.empty()) {
    const std::size_t amp = query.find('&');
    const std::string_view pair = amp == std::string_view::npos ? query : query.substr(0, amp);
    const std::size_t eq = pair.find('=');
    const std::string_view k = eq == std::string_view::npos ? pair : pair.substr(0, eq);
    if (k == key) return eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return {};
}

}  // namespace fastmm::net
