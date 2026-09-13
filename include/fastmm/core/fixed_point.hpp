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

  // Config / diagnostics only: never on the hot path.
  [[nodiscard]] static Fixed from_double(double d) noexcept {
    const double scaled = d * static_cast<double>(kFixedScale);
    const double rounded = scaled < 0 ? scaled - 0.5 : scaled + 0.5;
    return from_raw(static_cast<std::int64_t>(rounded));
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

static_assert(std::is_trivially_copyable_v<Price> && sizeof(Price) == 8);

// price * qty with a 128-bit intermediate, truncated toward zero to 1e-8.
[[nodiscard]] constexpr Notional mul(Price p, Qty q) noexcept {
  const Int128 r = static_cast<Int128>(p.raw) * static_cast<Int128>(q.raw) / kFixedScale;
  return Notional::from_raw(static_cast<std::int64_t>(r));
}
// Generic scaled multiply for fixed*fixed in one domain (e.g. qty * multiplier).
template <class A, class B>
[[nodiscard]] constexpr std::int64_t mul_raw(Fixed<A> a, Fixed<B> b) noexcept {
  return static_cast<std::int64_t>(static_cast<Int128>(a.raw) * static_cast<Int128>(b.raw) /
                                   kFixedScale);
}
// notional / qty -> price (used for average cost). Truncates toward zero.
[[nodiscard]] constexpr Price div(Notional n, Qty q) noexcept {
  if (q.raw == 0) return Price{};
  const Int128 r = static_cast<Int128>(n.raw) * kFixedScale / static_cast<Int128>(q.raw);
  return Price::from_raw(static_cast<std::int64_t>(r));
}
// value * bps / 10000 in one domain.
template <class T>
[[nodiscard]] constexpr Fixed<T> apply_bps(Fixed<T> v, std::int64_t bps) noexcept {
  return Fixed<T>::from_raw(static_cast<std::int64_t>(static_cast<Int128>(v.raw) * bps / 10'000));
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

namespace literals {
// 100_px == Price::from_int(100). Only whole units; use from_decimal for fractions.
constexpr Price operator""_px(unsigned long long v) noexcept {
  return Price::from_int(static_cast<std::int64_t>(v));
}
constexpr Qty operator""_qty(unsigned long long v) noexcept {
  return Qty::from_int(static_cast<std::int64_t>(v));
}
}  // namespace literals

}  // namespace fastmm
