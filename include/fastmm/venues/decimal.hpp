#pragma once
// Exact decimal helpers for venue payloads (6.2). Every price/quantity string a venue sends
// is parsed with Fixed::from_decimal (integer loop, no double) and returned as a Result so
// the caller can count malformed messages instead of throwing. Timestamps arrive as
// millisecond integers (Binance `E`/`T`, Bybit `ts`/`T`) or strings (Bybit `execTime`).
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace fastmm::venues {

enum class DecimalError : std::uint8_t {
  Empty = 1,
  Malformed = 2,  // non-digit, exponent, whitespace, > 8 significant fraction digits
  Overflow = 3,   // does not fit the int64 1e-8 range
};
[[nodiscard]] constexpr std::string_view to_string(DecimalError e) noexcept {
  switch (e) {
    case DecimalError::Empty:
      return "empty";
    case DecimalError::Malformed:
      return "malformed";
    case DecimalError::Overflow:
      return "overflow";
  }
  return "?";
}

namespace detail {
// Classifies why from_decimal rejected `s` (only called on the failure path).
[[nodiscard]] constexpr DecimalError classify(std::string_view s) noexcept {
  if (s.empty()) return DecimalError::Empty;
  std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  std::size_t int_digits = 0;
  for (; i < s.size() && s[i] != '.'; ++i) {
    if (s[i] < '0' || s[i] > '9') return DecimalError::Malformed;
    ++int_digits;
  }
  if (i < s.size()) {
    for (++i; i < s.size(); ++i) {
      if (s[i] < '0' || s[i] > '9') return DecimalError::Malformed;
    }
  }
  // 92233720368 has 11 digits: anything longer is an overflow, anything shorter that still
  // failed must have carried too many fraction digits.
  return int_digits >= 11 ? DecimalError::Overflow : DecimalError::Malformed;
}
}  // namespace detail

template <class F>
[[nodiscard]] inline Result<F, DecimalError> parse_fixed(std::string_view s) noexcept {
  if (const auto v = F::from_decimal(s)) return *v;
  return fail(detail::classify(s));
}
[[nodiscard]] inline Result<Price, DecimalError> parse_price(std::string_view s) noexcept {
  return parse_fixed<Price>(s);
}
[[nodiscard]] inline Result<Qty, DecimalError> parse_qty(std::string_view s) noexcept {
  return parse_fixed<Qty>(s);
}
[[nodiscard]] inline Result<Notional, DecimalError> parse_notional(std::string_view s) noexcept {
  return parse_fixed<Notional>(s);
}

// "[-]digits" -> int64; rejects empty, signs other than a leading '-', overflow.
[[nodiscard]] inline Result<std::int64_t, DecimalError> parse_int64(std::string_view s) noexcept {
  if (s.empty()) return fail(DecimalError::Empty);
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '-') {
    neg = true;
    i = 1;
    if (s.size() == 1) return fail(DecimalError::Malformed);
  }
  std::uint64_t v = 0;
  constexpr std::uint64_t kMax =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return fail(DecimalError::Malformed);
    const auto d = static_cast<std::uint64_t>(c - '0');
    if (v > (kMax - d) / 10) return fail(DecimalError::Overflow);
    v = v * 10 + d;
  }
  return neg ? -static_cast<std::int64_t>(v) : static_cast<std::int64_t>(v);
}

// Milliseconds since the Unix epoch -> Timestamp (ns).
[[nodiscard]] constexpr Timestamp ts_from_ms(std::int64_t ms) noexcept {
  return Timestamp{ms * 1'000'000};
}
[[nodiscard]] inline Result<Timestamp, DecimalError> parse_ts_ms(std::string_view s) noexcept {
  const auto v = parse_int64(s);
  if (!v) return fail(v.error());
  return ts_from_ms(*v);
}

// Stack formatting buffer: `DecimalText t(px); send(t.view());` - no heap, no locale.
class DecimalText {
 public:
  template <class Tag>
  explicit DecimalText(Fixed<Tag> v) noexcept : len_(v.to_decimal(buf_)) {
    buf_[len_] = '\0';
  }
  [[nodiscard]] std::string_view view() const noexcept { return {buf_, len_}; }
  [[nodiscard]] const char* c_str() const noexcept { return buf_; }
  [[nodiscard]] std::size_t size() const noexcept { return len_; }

 private:
  char buf_[kMaxDecimalChars + 1];
  std::size_t len_;
};

// Formats an int64 into a caller buffer (>= 21 chars); returns the length.
[[nodiscard]] constexpr std::size_t format_int64(std::int64_t v, char* out) noexcept {
  char tmp[20];
  std::size_t n = 0;
  std::uint64_t mag = v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
  do {
    tmp[n++] = static_cast<char>('0' + mag % 10);
    mag /= 10;
  } while (mag != 0);
  std::size_t w = 0;
  if (v < 0) out[w++] = '-';
  while (n > 0) out[w++] = tmp[--n];
  return w;
}

}  // namespace fastmm::venues
