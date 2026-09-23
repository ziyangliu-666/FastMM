#pragma once
// Fixed<Tag>: int64 fixed point with a global 1e-8 scale (ADR-0001).
//
// Price, Qty and Notional are distinct types: p * q does not compile, use mul(p, q) which
// widens to Int128. Exact decimal parsing/formatting (no double) is used for every venue
// string; from_double/to_double exist only for config and diagnostics.
// Range: +-92,233,720,368.54775807. Instruments outside this are rejected at load time.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace fastmm {

inline constexpr std::int64_t kFixedScale = 100'000'000;  // 1e-8
inline constexpr int kFixedDecimals = 8;
// sign + 11 integer digits + '.' + 8 fraction digits + NUL
inline constexpr std::size_t kMaxDecimalChars = 24;
// Ratio (below): 1.0 == kFixedScale raw, one basis point == kRatioPerBp raw.
inline constexpr std::int64_t kRatioPerBp = 10'000;
inline constexpr int kBpsDecimals = 4;

struct RatioTag;

namespace detail {

// a * b / d truncated toward zero, as through Int128. When a * b fits int64 (prices and
// quantities in practice) the division stays 64-bit: a constant divisor becomes a multiply,
// where an Int128 division is a call to __divti3 (~40 cycles). Same result either way.
[[nodiscard]] FASTMM_FORCE_INLINE constexpr std::int64_t mul_div(std::int64_t a,
                                                                 std::int64_t b,
                                                                 std::int64_t d) noexcept {
  std::int64_t p = 0;
  if (FASTMM_LIKELY(!__builtin_mul_overflow(a, b, &p) && d != -1)) return p / d;
  return static_cast<std::int64_t>(static_cast<Int128>(a) * b / d);
}
template <std::int64_t D>
[[nodiscard]] FASTMM_FORCE_INLINE constexpr std::int64_t mul_div(std::int64_t a,
                                                                 std::int64_t b) noexcept {
  std::int64_t p = 0;
  if (FASTMM_LIKELY(!__builtin_mul_overflow(a, b, &p))) return p / D;
  return static_cast<std::int64_t>(static_cast<Int128>(a) * b / D);
}

// Exact parse of "[+-]digits[.digits][(e|E)[+-]digits]" into value * 10^decimals. The result must
// be an integer that fits int64: digits left over after the scale must be zeros, so "2e-05" with 8
// decimals is 2000 and "1.5e-8" is rejected. With `literal` (numeric literal characters) no sign is
// accepted, digit separators (') are skipped and an octal-looking integer ("017") is rejected.
[[nodiscard]] constexpr std::optional<std::int64_t> parse_scaled_decimal(
    std::string_view s, int decimals, bool literal = false) noexcept {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  std::size_t i = 0;
  bool neg = false;
  if (!literal && !s.empty() && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    i = 1;
  }
  const std::size_t mant_begin = i;
  std::int64_t n_digits = 0;
  std::int64_t frac_digits = 0;
  bool dot = false;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      ++n_digits;
      if (dot) ++frac_digits;
    } else if (c == '.' && !dot) {
      dot = true;
    } else if (!literal || c != '\'') {
      break;
    }
  }
  const std::size_t mant_end = i;
  if (n_digits == 0) return std::nullopt;
  std::int64_t exp = 0;
  bool has_exp = false;
  if (i < s.size()) {
    if (s[i] != 'e' && s[i] != 'E') return std::nullopt;
    has_exp = true;
    ++i;
    bool exp_neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
      exp_neg = s[i] == '-';
      ++i;
    }
    std::size_t exp_digits = 0;
    for (; i < s.size(); ++i) {
      const char c = s[i];
      if (literal && c == '\'') continue;
      if (c < '0' || c > '9') return std::nullopt;
      if (exp < 100'000) exp = exp * 10 + (c - '0');  // larger exponents fail below anyway
      ++exp_digits;
    }
    if (exp_digits == 0) return std::nullopt;
    if (exp_neg) exp = -exp;
  }
  if (literal && !dot && !has_exp && n_digits > 1 && s[mant_begin] == '0') return std::nullopt;
  // value = mantissa digits * 10^(exp - frac_digits); result = digits * 10^shift
  const std::int64_t shift = exp - frac_digits + decimals;
  const std::int64_t keep = n_digits + (shift < 0 ? shift : 0);  // leading digits that survive
  std::int64_t r = 0;
  std::int64_t idx = 0;
  for (std::size_t k = mant_begin; k < mant_end; ++k) {
    const char c = s[k];
    if (c < '0' || c > '9') continue;
    const int d = c - '0';
    if (idx < keep) {
      if (r > (kMax - d) / 10) return std::nullopt;
      r = r * 10 + d;
    } else if (d != 0) {
      return std::nullopt;  // more decimals than the scale holds
    }
    ++idx;
  }
  for (std::int64_t k = 0; k < shift && r != 0; ++k) {
    if (r > kMax / 10) return std::nullopt;
    r *= 10;
  }
  return neg ? -r : r;
}

// d * scale rounded half away from zero, or nullopt when the result is not an int64: NaN and
// +-inf fail both comparisons, and the bounds are exclusive so the cast is always defined.
[[nodiscard]] inline std::optional<std::int64_t> scaled_round(double d, double scale) noexcept {
  constexpr double kTwoPow63 = 9223372036854775808.0;  // == -(double)INT64_MIN, exact
  const double scaled = d * scale;
  const double rounded = scaled < 0 ? scaled - 0.5 : scaled + 0.5;
  if (!(rounded > -kTwoPow63 && rounded < kTwoPow63)) return std::nullopt;
  return static_cast<std::int64_t>(rounded);
}
// What from_double() returns for a value scaled_round() refused: zero for NaN, the nearest bound
// otherwise.
template <class F>
[[nodiscard]] inline F saturate(double d) noexcept {
  if (d != d) return F{};  // NaN
  return d < 0 ? F::min() : F::max();
}

}  // namespace detail

template <class Tag>
struct Fixed {
  using rep_type = std::int64_t;
  std::int64_t raw = 0;

  constexpr Fixed() noexcept = default;
  [[nodiscard]] static constexpr Fixed from_raw(std::int64_t r) noexcept {
    Fixed f;
    f.raw = r;
    return f;
  }
  // Whole units, e.g. from_int(100) == 100.00000000.
  [[nodiscard]] static constexpr Fixed from_int(std::int64_t units) noexcept {
    return from_raw(units * kFixedScale);
  }
  [[nodiscard]] static constexpr Fixed zero() noexcept { return Fixed{}; }
  [[nodiscard]] static constexpr Fixed max() noexcept {
    return from_raw(std::numeric_limits<std::int64_t>::max());
  }
  [[nodiscard]] static constexpr Fixed min() noexcept {
    return from_raw(std::numeric_limits<std::int64_t>::min());
  }

  // Exact parse of "[+-]digits[.digits]". Rejects: empty, >8 significant fraction digits,
  // integer overflow, exponents, whitespace. Trailing zeros beyond 8 places are accepted.
  [[nodiscard]] static constexpr std::optional<Fixed> from_decimal(std::string_view s) noexcept {
    if (s.empty()) return std::nullopt;
    std::size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') {
      neg = s[0] == '-';
      i = 1;
    }
    constexpr std::int64_t kMaxInt =
        std::numeric_limits<std::int64_t>::max() / kFixedScale;  // 92233720368
    std::int64_t ip = 0;
    std::size_t int_digits = 0;
    for (; i < s.size() && s[i] != '.'; ++i) {
      const char c = s[i];
      if (c < '0' || c > '9') return std::nullopt;
      if (ip > (kMaxInt - (c - '0')) / 10) return std::nullopt;
      ip = ip * 10 + (c - '0');
      ++int_digits;
    }
    std::int64_t fp = 0;
    std::size_t frac_digits = 0;
    if (i < s.size()) {  // s[i] == '.'
      ++i;
      for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return std::nullopt;
        if (frac_digits < static_cast<std::size_t>(kFixedDecimals)) {
          fp = fp * 10 + (c - '0');
          ++frac_digits;
        } else if (c != '0') {
          return std::nullopt;  // would lose precision
        }
      }
    }
    if (int_digits == 0 && frac_digits == 0) return std::nullopt;
    for (std::size_t k = frac_digits; k < static_cast<std::size_t>(kFixedDecimals); ++k) fp *= 10;
    const std::int64_t mag = ip * kFixedScale;
    if (mag > std::numeric_limits<std::int64_t>::max() - fp) return std::nullopt;
    const std::int64_t r = mag + fp;
    return from_raw(neg ? -r : r);
  }

  // Exact parse of a decimal in plain or exponent notation: "0.1", "2e-05", "1.5E3", "-3". Used for
  // configuration values (TOML floats arrive formatted by fmt, Python floats by repr). Rejects a
  // value only if more than 8 decimals remain after applying the exponent, or it is out of range.
  // Venue strings use the stricter from_decimal.
  [[nodiscard]] static constexpr std::optional<Fixed> parse(std::string_view s) noexcept {
    const std::optional<std::int64_t> r = detail::parse_scaled_decimal(s, kFixedDecimals);
    if (!r) return std::nullopt;
    return from_raw(*r);
  }

  // Ratio only. Startup only (double): Ratio::from_bps(2.5) == 2.5 bps, rounded to 0.0001 bp.
  // Saturates like from_double; from_bps_checked() reports the loss instead.
  [[nodiscard]] static Fixed from_bps(double bps) noexcept
    requires std::is_same_v<Tag, RatioTag>
  {
    return from_bps_checked(bps).value_or(detail::saturate<Fixed>(bps));
  }
  [[nodiscard]] static std::optional<Fixed> from_bps_checked(double bps) noexcept
    requires std::is_same_v<Tag, RatioTag>
  {
    const std::optional<std::int64_t> r =
        detail::scaled_round(bps, static_cast<double>(kRatioPerBp));
    if (!r) return std::nullopt;
    return from_raw(*r);
  }
  // Ratio only. Diagnostics only.
  [[nodiscard]] double to_bps() const noexcept
    requires std::is_same_v<Tag, RatioTag>
  {
    return static_cast<double>(raw) / static_cast<double>(kRatioPerBp);
  }

  // Formats into buf (>= kMaxDecimalChars), trims trailing zeros ("1.5", "100", "0.00000001").
  // Returns the number of chars written; no NUL is appended.
  std::size_t to_decimal(char* buf) const noexcept {
    char* p = buf;
    std::uint64_t mag = 0;
    if (raw < 0) {
      *p++ = '-';
      mag = raw == std::numeric_limits<std::int64_t>::min()
                ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U
                : static_cast<std::uint64_t>(-raw);
    } else {
      mag = static_cast<std::uint64_t>(raw);
    }
    const std::uint64_t ip = mag / static_cast<std::uint64_t>(kFixedScale);
    std::uint64_t fp = mag % static_cast<std::uint64_t>(kFixedScale);
    // integer part
    char tmp[20];
    int n = 0;
    std::uint64_t v = ip;
    do {
      tmp[n++] = static_cast<char>('0' + (v % 10));
      v /= 10;
    } while (v != 0);
    while (n > 0) *p++ = tmp[--n];
    if (fp != 0) {
      *p++ = '.';
      char frac[kFixedDecimals];
      for (int k = kFixedDecimals - 1; k >= 0; --k) {
        frac[k] = static_cast<char>('0' + (fp % 10));
        fp /= 10;
      }
      int len = kFixedDecimals;
      while (len > 0 && frac[len - 1] == '0') --len;
      for (int k = 0; k < len; ++k) *p++ = frac[k];
    }
    return static_cast<std::size_t>(p - buf);
  }

  // Rounds to the 1e-8 grid. NaN becomes zero and a value outside +-92,233,720,368.54775807
  // saturates at max()/min(); the unchecked cast this replaced made both INT64_MIN.
  [[nodiscard]] static Fixed from_double(double d) noexcept {
    return from_double_checked(d).value_or(detail::saturate<Fixed>(d));
  }
  // nullopt for NaN, +-inf and anything outside the representable range; two compares more.
  [[nodiscard]] static std::optional<Fixed> from_double_checked(double d) noexcept {
    const std::optional<std::int64_t> r = detail::scaled_round(d, static_cast<double>(kFixedScale));
    if (!r) return std::nullopt;
    return from_raw(*r);
  }
  [[nodiscard]] double to_double() const noexcept {
    return static_cast<double>(raw) / static_cast<double>(kFixedScale);
  }

  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw == 0; }
  [[nodiscard]] constexpr bool is_positive() const noexcept { return raw > 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return raw < 0; }
  [[nodiscard]] constexpr Fixed abs() const noexcept { return from_raw(raw < 0 ? -raw : raw); }
  [[nodiscard]] constexpr std::int64_t units() const noexcept { return raw / kFixedScale; }

  constexpr auto operator<=>(const Fixed&) const noexcept = default;

  constexpr Fixed operator-() const noexcept { return from_raw(-raw); }
  friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept { return from_raw(a.raw + b.raw); }
  friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept { return from_raw(a.raw - b.raw); }
  friend constexpr Fixed operator*(Fixed a, std::int64_t k) noexcept { return from_raw(a.raw * k); }
  friend constexpr Fixed operator*(std::int64_t k, Fixed a) noexcept { return from_raw(a.raw * k); }
  friend constexpr Fixed operator/(Fixed a, std::int64_t k) noexcept { return from_raw(a.raw / k); }
  // Number of whole ticks in a (truncating); result is a plain integer.
  friend constexpr std::int64_t operator/(Fixed a, Fixed b) noexcept { return a.raw / b.raw; }
  friend constexpr Fixed operator%(Fixed a, Fixed b) noexcept { return from_raw(a.raw % b.raw); }
  constexpr Fixed& operator+=(Fixed o) noexcept {
    raw += o.raw;
    return *this;
  }
  constexpr Fixed& operator-=(Fixed o) noexcept {
    raw -= o.raw;
    return *this;
  }
  constexpr Fixed& operator*=(std::int64_t k) noexcept {
    raw *= k;
    return *this;
  }
};

using Price = Fixed<struct PriceTag>;
using Qty = Fixed<struct QtyTag>;
using Notional = Fixed<struct NotionalTag>;
// A dimensionless factor: 1.0 == 1e8 raw, 1 bp == 10'000 raw, so 0.0001 bp is the smallest step.
using Ratio = Fixed<RatioTag>;

static_assert(std::is_trivially_copyable_v<Price> && sizeof(Price) == 8);

// value * ratio in the value's domain, through Int128 with one truncation toward zero:
// mid * 5_bps, qty * ratio(filled, total).
template <class T>
[[nodiscard]] constexpr Fixed<T> operator*(Fixed<T> v, Ratio r) noexcept {
  return Fixed<T>::from_raw(detail::mul_div<kFixedScale>(v.raw, r.raw));
}
template <class T>
[[nodiscard]] constexpr Fixed<T> operator*(Ratio r, Fixed<T> v) noexcept {
  return v * r;
}
[[nodiscard]] constexpr Ratio operator*(Ratio a, Ratio b) noexcept {
  return Ratio::from_raw(detail::mul_div<kFixedScale>(a.raw, b.raw));
}
// num / den as a Ratio, truncated toward zero; zero when den is zero. The quotient must fit
// +-92,233,720,368.
template <class T>
[[nodiscard]] constexpr Ratio ratio(Fixed<T> num, Fixed<T> den) noexcept {
  if (den.raw == 0) return Ratio{};
  return Ratio::from_raw(detail::mul_div(num.raw, kFixedScale, den.raw));
}

// price * qty with a 128-bit intermediate, truncated toward zero to 1e-8.
[[nodiscard]] constexpr Notional mul(Price p, Qty q) noexcept {
  return Notional::from_raw(detail::mul_div<kFixedScale>(p.raw, q.raw));
}
// Generic scaled multiply for fixed*fixed in one domain (e.g. qty * multiplier).
template <class A, class B>
[[nodiscard]] constexpr std::int64_t mul_raw(Fixed<A> a, Fixed<B> b) noexcept {
  return detail::mul_div<kFixedScale>(a.raw, b.raw);
}
// notional / qty -> price (used for average cost). Truncates toward zero.
[[nodiscard]] constexpr Price div(Notional n, Qty q) noexcept {
  if (q.raw == 0) return Price{};
  return Price::from_raw(detail::mul_div(n.raw, kFixedScale, q.raw));
}
// value * bps / 10000 in one domain.
template <class T>
[[nodiscard]] constexpr Fixed<T> apply_bps(Fixed<T> v, std::int64_t bps) noexcept {
  return Fixed<T>::from_raw(detail::mul_div<10'000>(v.raw, bps));
}

// Floor division for possibly-negative numerators (prices are >= 0, but be safe).
[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
  const std::int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Round to the tick grid in the passive direction: bids down, asks up. tick > 0.
[[nodiscard]] constexpr Price round_to_tick(Price p, Price tick, Side side) noexcept {
  FASTMM_ASSERT(tick.raw > 0);
  const std::int64_t q = floor_div(p.raw, tick.raw);
  const std::int64_t floored = q * tick.raw;
  if (side == Side::Buy || floored == p.raw) return Price::from_raw(floored);
  return Price::from_raw(floored + tick.raw);
}
[[nodiscard]] constexpr Price round_to_tick_nearest(Price p, Price tick) noexcept {
  FASTMM_ASSERT(tick.raw > 0);
  const std::int64_t half = tick.raw / 2;
  const std::int64_t shifted = p.raw >= 0 ? p.raw + half : p.raw - half;
  return Price::from_raw(floor_div(shifted, tick.raw) * tick.raw);
}
[[nodiscard]] constexpr bool on_tick(Price p, Price tick) noexcept {
  return tick.raw > 0 && p.raw % tick.raw == 0;
}
// Quantities always round down (never send more than intended).
[[nodiscard]] constexpr Qty round_to_lot(Qty q, Qty lot) noexcept {
  FASTMM_ASSERT(lot.raw > 0);
  return Qty::from_raw(floor_div(q.raw, lot.raw) * lot.raw);
}
[[nodiscard]] constexpr bool on_lot(Qty q, Qty lot) noexcept {
  return lot.raw > 0 && q.raw % lot.raw == 0;
}

template <class T>
[[nodiscard]] constexpr Fixed<T> min(Fixed<T> a, Fixed<T> b) noexcept {
  return a.raw < b.raw ? a : b;
}
template <class T>
[[nodiscard]] constexpr Fixed<T> max(Fixed<T> a, Fixed<T> b) noexcept {
  return a.raw < b.raw ? b : a;
}

// Checked arithmetic: nullopt instead of a wrapped int64. The operators stay unchecked; these are
// for the places that combine a value from outside the engine with a configured one.
template <class T>
[[nodiscard]] constexpr std::optional<Fixed<T>> checked_add(Fixed<T> a, Fixed<T> b) noexcept {
  std::int64_t r = 0;
  if (__builtin_add_overflow(a.raw, b.raw, &r)) return std::nullopt;
  return Fixed<T>::from_raw(r);
}
template <class T>
[[nodiscard]] constexpr std::optional<Fixed<T>> checked_sub(Fixed<T> a, Fixed<T> b) noexcept {
  std::int64_t r = 0;
  if (__builtin_sub_overflow(a.raw, b.raw, &r)) return std::nullopt;
  return Fixed<T>::from_raw(r);
}
template <class T>
[[nodiscard]] constexpr std::optional<Fixed<T>> checked_mul(Fixed<T> a, std::int64_t k) noexcept {
  std::int64_t r = 0;
  if (__builtin_mul_overflow(a.raw, k, &r)) return std::nullopt;
  return Fixed<T>::from_raw(r);
}

namespace detail {
template <char... Cs>
inline constexpr char kLiteralChars[sizeof...(Cs)] = {Cs...};
template <char... Cs>
[[nodiscard]] constexpr std::optional<std::int64_t> literal_raw(int decimals) noexcept {
  return parse_scaled_decimal(
      std::string_view(kLiteralChars<Cs...>, sizeof...(Cs)), decimals, /*literal=*/true);
}
}  // namespace detail

// Exact literals, parsed at compile time: 100.25_px, 0.01_qty (up to 8 decimals), 5_bps, 0.25_bps
// (up to 4 decimals). More decimals, or a value out of range, is a compile error. An inline
// namespace, so `using namespace fastmm;` brings them in too (like std::literals).
inline namespace literals {
// 100_px == Price::from_int(100).
constexpr Price operator""_px(unsigned long long v) noexcept {
  return Price::from_int(static_cast<std::int64_t>(v));
}
constexpr Qty operator""_qty(unsigned long long v) noexcept {
  return Qty::from_int(static_cast<std::int64_t>(v));
}
template <char... Cs>
[[nodiscard]] constexpr Price operator""_px() noexcept {
  static_assert(detail::literal_raw<Cs...>(kFixedDecimals).has_value(),
                "fastmm: _px literal needs more than 8 decimals or is out of range");
  return Price::from_raw(detail::literal_raw<Cs...>(kFixedDecimals)
                             .value_or(0));  // checked by the static_assert above
}
template <char... Cs>
[[nodiscard]] constexpr Qty operator""_qty() noexcept {
  static_assert(detail::literal_raw<Cs...>(kFixedDecimals).has_value(),
                "fastmm: _qty literal needs more than 8 decimals or is out of range");
  return Qty::from_raw(detail::literal_raw<Cs...>(kFixedDecimals)
                           .value_or(0));  // checked by the static_assert above
}
template <char... Cs>
[[nodiscard]] constexpr Ratio operator""_bps() noexcept {
  static_assert(detail::literal_raw<Cs...>(kBpsDecimals).has_value(),
                "fastmm: _bps literal needs more than 4 decimals or is out of range");
  return Ratio::from_raw(
      detail::literal_raw<Cs...>(kBpsDecimals).value_or(0));  // checked by the static_assert above
}
}  // namespace literals

}  // namespace fastmm
