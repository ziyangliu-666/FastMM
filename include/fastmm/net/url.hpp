#pragma once
// Minimal URL parsing / query building for venue endpoints. No heap allocation in QueryBuilder;
// Url::parse works on string_views into the input.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fastmm::net {

struct Url {
  std::string_view scheme;  // "wss", "https", ...
  std::string_view host;    // without brackets for IPv6 literals
  std::uint16_t port = 0;   // explicit or scheme default (80/443)
  std::string_view path;    // always at least "/"
  std::string_view query;   // without '?'
  bool tls = false;         // scheme is wss/https

  // Path + "?" + query as sent in the request line; `buf` must hold both.
  std::size_t request_target(std::span<char> buf) const noexcept {
    if (buf.size() < path.size() + (query.empty() ? 0 : query.size() + 1)) return 0;
    std::size_t n = 0;
    for (char c : path) buf[n++] = c;
    if (!query.empty()) {
      buf[n++] = '?';
      for (char c : query) buf[n++] = c;
    }
    return n;
  }

  // Accepts scheme://host[:port][/path][?query]. Fragment is not supported (venue URLs
  // never carry one). Returns nullopt for a missing scheme/host or a bad port.
  static std::optional<Url> parse(std::string_view s) noexcept {
    Url u;
    const std::size_t scheme_end = s.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) return std::nullopt;
    u.scheme = s.substr(0, scheme_end);
    std::string_view rest = s.substr(scheme_end + 3);

    std::size_t authority_end = rest.find_first_of("/?");
    std::string_view authority =
        authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);
    std::string_view tail =
        authority_end == std::string_view::npos ? "" : rest.substr(authority_end);
    if (authority.empty()) return std::nullopt;

    std::string_view port_str;
    if (authority.front() == '[') {  // IPv6 literal
      const std::size_t close = authority.find(']');
      if (close == std::string_view::npos) return std::nullopt;
      u.host = authority.substr(1, close - 1);
      std::string_view after = authority.substr(close + 1);
      if (!after.empty()) {
        if (after.front() != ':') return std::nullopt;
        port_str = after.substr(1);
      }
    } else {
      const std::size_t colon = authority.rfind(':');
      if (colon == std::string_view::npos) {
        u.host = authority;
      } else {
        u.host = authority.substr(0, colon);
        port_str = authority.substr(colon + 1);
      }
    }
    if (u.host.empty()) return std::nullopt;

    u.tls = (u.scheme == "wss" || u.scheme == "https");
    if (port_str.empty()) {
      u.port = u.tls ? 443 : 80;
    } else {
      std::uint32_t p = 0;
      for (char c : port_str) {
        if (c < '0' || c > '9') return std::nullopt;
        p = p * 10 + static_cast<std::uint32_t>(c - '0');
        if (p > 65535) return std::nullopt;
      }
      if (p == 0) return std::nullopt;
      u.port = static_cast<std::uint16_t>(p);
    }

    if (tail.empty() || tail.front() == '?') {
      u.path = "/";
      u.query = tail.empty() ? "" : tail.substr(1);
    } else {
      const std::size_t q = tail.find('?');
      u.path = q == std::string_view::npos ? tail : tail.substr(0, q);
      u.query = q == std::string_view::npos ? "" : tail.substr(q + 1);
    }
    return u;
  }
};

// RFC 3986 unreserved characters pass through; everything else becomes %XX.
constexpr bool is_url_unreserved(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == '.' || c == '~';
}

// Returns bytes written, or 0 if `out` is too small (worst case 3 * in.size()).
inline std::size_t percent_encode(std::string_view in, std::span<char> out) noexcept {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::size_t n = 0;
  for (char c : in) {
    if (is_url_unreserved(c)) {
      if (n + 1 > out.size()) return 0;
      out[n++] = c;
    } else {
      if (n + 3 > out.size()) return 0;
      const auto b = static_cast<std::uint8_t>(c);
      out[n++] = '%';
      out[n++] = kHex[b >> 4];
      out[n++] = kHex[b & 0x0F];
    }
  }
  return n;
}

// Builds "k1=v1&k2=v2" into a fixed buffer, percent-encoding values. Overflow is sticky:
// `ok()` turns false and the view is truncated to the last complete pair, so a signed
// request can never be built from a partially-encoded query.
template <std::size_t Capacity>
class QueryBuilder {
 public:
  QueryBuilder& add(std::string_view key, std::string_view value) noexcept {
    if (!ok_) return *this;
    const std::size_t start = len_;
    if (len_ > 0 && !put('&')) return fail(start);
    for (char c : key) {
      if (!put(c)) return fail(start);
    }
    if (!put('=')) return fail(start);
    const std::size_t n =
        percent_encode(value, std::span<char>(buf_.data() + len_, Capacity - len_));
    if (n == 0 && !value.empty()) return fail(start);
    len_ += n;
    return *this;
  }

  QueryBuilder& add(std::string_view key, std::int64_t value) noexcept {
    char tmp[21];
    std::size_t n = 0;
    std::uint64_t mag =
        value < 0 ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    char digits[20];
    std::size_t d = 0;
    do {
      digits[d++] = static_cast<char>('0' + mag % 10);
      mag /= 10;
    } while (mag != 0);
    if (value < 0) tmp[n++] = '-';
    while (d > 0) tmp[n++] = digits[--d];
    return add(key, std::string_view(tmp, n));
  }

  // Appends raw pre-encoded text (e.g. "&signature=...").
  QueryBuilder& append_raw(std::string_view raw) noexcept {
    if (!ok_) return *this;
    const std::size_t start = len_;
    for (char c : raw) {
      if (!put(c)) return fail(start);
    }
    return *this;
  }

  std::string_view view() const noexcept { return {buf_.data(), len_}; }
  std::size_t size() const noexcept { return len_; }
  bool ok() const noexcept { return ok_; }
  bool empty() const noexcept { return len_ == 0; }
  void clear() noexcept {
    len_ = 0;
    ok_ = true;
  }

 private:
  bool put(char c) noexcept {
    if (len_ >= Capacity) return false;
    buf_[len_++] = c;
    return true;
  }
  QueryBuilder& fail(std::size_t rollback_to) noexcept {
    len_ = rollback_to;
    ok_ = false;
    return *this;
  }

  std::array<char, Capacity> buf_{};
  std::size_t len_ = 0;
  bool ok_ = true;
};

}  // namespace fastmm::net
