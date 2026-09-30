#pragma once
// Coinbase Exchange wire forms shared by the parsers, the encoder and the connector: client order
// ids, order ids and times. Allocation free.
//
// client_oid "must be a variant 1 UUIDv4 ... all lowercase" (the venue accepts a malformed one over
// REST and does not process the order). FastMM's 48-bit ClientOrderId (epoch 16 bits, sequence 32
// bits) goes into the last 12 hex digits behind a fixed prefix whose first four digits spell "fm"
// in ASCII hex (0x66 0x6d):
//
//   666d0000-0000-4000-8000-EEEESSSSSSSS      version 4, variant 1 (the '8')
//
// An id without that prefix is not FastMM's.
//
// Order ids are UUIDs the venue assigns. OrderKey is the low 64 bits of the id, taken from its last
// 16 hex digits: those are random in a UUIDv4, so two live orders do not share one in practice.
//
// Times are RFC 3339 with up to nine fraction digits ("2026-09-30T01:41:50.644756Z").
//
// Balances carry up to 16 fraction digits on the Exchange ("0.0000000000000000"): parse_balance()
// truncates them to FastMM's 8, so a balance never reads as more than the venue holds.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/strong_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fastmm::venues::coinbase {

inline constexpr std::size_t kUuidChars = 36;
inline constexpr std::string_view kClientOidPrefix = "666d0000-0000-4000-8000-";

struct ClientOid {
  std::array<char, kUuidChars> text{};
  [[nodiscard]] std::string_view view() const noexcept { return {text.data(), text.size()}; }
};

[[nodiscard]] constexpr ClientOid encode_client_oid(ClientOrderId id) noexcept {
  ClientOid out;
  for (std::size_t i = 0; i < kClientOidPrefix.size(); ++i) out.text[i] = kClientOidPrefix[i];
  constexpr char kHex[] = "0123456789abcdef";
  std::uint64_t v = id.value & 0xFFFF'FFFF'FFFFULL;
  for (std::size_t i = kUuidChars; i > kClientOidPrefix.size(); --i) {
    out.text[i - 1] = kHex[v & 0xF];
    v >>= 4;
  }
  return out;
}

namespace detail {
[[nodiscard]] constexpr int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace detail

// nullopt for anything that is not a FastMM client_oid (another client's, or empty).
[[nodiscard]] constexpr std::optional<ClientOrderId> decode_client_oid(
    std::string_view s) noexcept {
  if (s.size() != kUuidChars || s.substr(0, kClientOidPrefix.size()) != kClientOidPrefix)
    return std::nullopt;
  std::uint64_t v = 0;
  for (std::size_t i = kClientOidPrefix.size(); i < kUuidChars; ++i) {
    const char c = s[i];
    if (c >= 'A' && c <= 'F') return std::nullopt;  // the venue's form is lowercase
    const int d = detail::hex_digit(c);
    if (d < 0) return std::nullopt;
    v = (v << 4) | static_cast<std::uint64_t>(d);
  }
  if (v == 0) return std::nullopt;
  return ClientOrderId{v};
}

struct OrderKey {
  std::uint64_t value = 0;
  friend constexpr bool operator==(OrderKey, OrderKey) noexcept = default;
};

// The key of a venue order id (a 36-character UUID); nullopt when it is not one.
[[nodiscard]] constexpr std::optional<OrderKey> order_key(std::string_view id) noexcept {
  if (id.size() != kUuidChars || id[8] != '-' || id[13] != '-' || id[18] != '-' || id[23] != '-')
    return std::nullopt;
  std::uint64_t v = 0;
  std::size_t digits = 0;
  for (std::size_t i = kUuidChars; i > 0 && digits < 16; --i) {
    const char c = id[i - 1];
    if (c == '-') continue;
    const int d = detail::hex_digit(c);
    if (d < 0) return std::nullopt;
    v |= static_cast<std::uint64_t>(d) << (4 * digits);
    ++digits;
  }
  if (v == 0) return std::nullopt;
  return OrderKey{v};
}

// RFC 3339 UTC ("YYYY-MM-DDTHH:MM:SS[.fraction]Z", or a space for the T) to Unix nanoseconds; -1
// when unreadable.
[[nodiscard]] constexpr std::int64_t parse_time_ns(std::string_view s) noexcept {
  if (s.size() < 20) return -1;
  auto num = [&](std::size_t at, std::size_t w) -> std::int64_t {
    std::int64_t v = 0;
    for (std::size_t i = at; i < at + w; ++i) {
      if (s[i] < '0' || s[i] > '9') return -1;
      v = v * 10 + (s[i] - '0');
    }
    return v;
  };
  if (s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':')
    return -1;
  const std::int64_t y = num(0, 4);
  const std::int64_t mo = num(5, 2);
  const std::int64_t d = num(8, 2);
  const std::int64_t h = num(11, 2);
  const std::int64_t mi = num(14, 2);
  const std::int64_t se = num(17, 2);
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
      se < 0 || se > 60)
    return -1;
  std::int64_t frac_ns = 0;
  std::size_t i = 19;
  if (i < s.size() && s[i] == '.') {
    ++i;
    std::int64_t scale = 100'000'000;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
      frac_ns += (s[i] - '0') * scale;
      scale /= 10;
    }
  }
  if (i >= s.size() || s[i] != 'Z') return -1;
  // days_from_civil (H. Hinnant).
  const std::int64_t yy = mo <= 2 ? y - 1 : y;
  const std::int64_t era = yy / 400;
  const std::int64_t yoe = yy - era * 400;
  const std::int64_t mp = mo > 2 ? mo - 3 : mo + 9;
  const std::int64_t doy = (153 * mp + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days = era * 146097 + doe - 719468;
  return ((days * 86400 + h * 3600 + mi * 60 + se) * 1'000'000'000) + frac_ns;
}

// "YYYY-MM-DDTHH:MM:SS.mmmZ" of a Unix time in milliseconds (24 characters).
struct TimeText {
  std::array<char, 24> text{};
  [[nodiscard]] std::string_view view() const noexcept { return {text.data(), text.size()}; }
};

[[nodiscard]] constexpr TimeText format_time_ms(std::int64_t unix_ms) noexcept {
  TimeText out;
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

// A balance amount truncated to 8 decimals; nullopt when it is not a plain decimal.
[[nodiscard]] constexpr std::optional<Notional> parse_balance(std::string_view s) noexcept {
  const std::size_t dot = s.find('.');
  constexpr auto kKeep = static_cast<std::size_t>(kFixedDecimals);
  if (dot != std::string_view::npos && s.size() - dot - 1 > kKeep) {
    for (std::size_t i = dot + 1 + kKeep; i < s.size(); ++i) {
      if (s[i] < '0' || s[i] > '9') return std::nullopt;
    }
    s = s.substr(0, dot + 1 + kKeep);
  }
  return Notional::from_decimal(s);
}

}  // namespace fastmm::venues::coinbase
