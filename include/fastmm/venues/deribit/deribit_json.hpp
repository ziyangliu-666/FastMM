#pragma once
// Exact JSON-number -> fixed-point parsing for Deribit payloads.
//
// Deribit sends prices, amounts, strikes and tick sizes as JSON numbers rather than strings, and
// uses exponent notation freely: recorded testnet frames (tests/fixtures/deribit) contain
// "amount":1.0002e6, "strike":6.8e4 and "gamma":2.8e-4. Going through double would round large
// values (BTC-PERPETUAL open interest ~1e10 USD is 1e18 raw units), so fixed-point fields are
// parsed from the raw token text: optional '-', digits, optional fraction, optional decimal
// exponent. Values finer than the 1e-8 grid are rounded half away from zero. constexpr, noexcept,
// allocation-free; used on the hot path by the market-data and private-stream parsers.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/fixed_point.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace fastmm::venues::deribit {

// Raw 1e-8 units of a JSON number token (trailing whitespace allowed). nullopt for anything that
// is not a JSON number (including `null`) and for values outside the int64 range.
[[nodiscard]] constexpr std::optional<std::int64_t> json_number_to_raw(
    std::string_view s) noexcept {
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
    s.remove_suffix(1);
  if (s.empty()) return std::nullopt;
  std::size_t i = 0;
  const bool neg = s[0] == '-';
  if (neg) i = 1;
  constexpr int kMaxDigits = 36;  // significant digits that fit an unsigned 128-bit mantissa
  Uint128 mant = 0;
  int digits = 0;   // significant digits held in mant
  int dropped = 0;  // integer digits beyond kMaxDigits (each scales the value by 10)
  int frac = 0;     // fraction digits held in mant
  bool any_int = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    any_int = true;
    if (digits < kMaxDigits) {
      mant = mant * 10U + static_cast<unsigned>(s[i] - '0');
      if (mant != 0) ++digits;
    } else {
      ++dropped;
    }
  }
  if (!any_int) return std::nullopt;
  if (i < s.size() && s[i] == '.') {
    ++i;
    bool any_frac = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
      any_frac = true;
      if (digits < kMaxDigits) {
        mant = mant * 10U + static_cast<unsigned>(s[i] - '0');
        ++frac;
        if (mant != 0) ++digits;
      }
    }
    if (!any_frac) return std::nullopt;
  }
  int exp = 0;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    bool exp_neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      exp_neg = s[i] == '-';
      ++i;
    }
    bool any_exp = false;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
      any_exp = true;
      if (exp < 100'000) exp = exp * 10 + (s[i] - '0');
    }
    if (!any_exp) return std::nullopt;
    if (exp_neg) exp = -exp;
  }
  if (i != s.size()) return std::nullopt;
  if (mant == 0) return std::int64_t{0};
  constexpr auto kMax = static_cast<Uint128>(std::numeric_limits<std::int64_t>::max());
  // value = mant * 10^(exp + dropped - frac); raw = value * 10^kFixedDecimals.
  const int k = exp + dropped - frac + kFixedDecimals;
  Uint128 raw = mant;
  if (k >= 0) {
    for (int j = 0; j < k; ++j) {
      if (raw > kMax) return std::nullopt;
      raw *= 10U;
    }
  } else {
    if (-k > 38) return std::int64_t{0};  // mant < 1e36: the value is below half a raw unit
    Uint128 div = 1;
    for (int j = 0; j < -k; ++j) div *= 10U;
    const Uint128 rem = raw % div;
    raw /= div;
    if (rem >= div - rem) ++raw;  // rem * 2 >= div without overflow
  }
  if (raw > kMax) return std::nullopt;
  const auto r = static_cast<std::int64_t>(raw);
  return neg ? -r : r;
}

template <class F>
[[nodiscard]] constexpr std::optional<F> json_fixed(std::string_view token) noexcept {
  const auto r = json_number_to_raw(token);
  if (!r) return std::nullopt;
  return F::from_raw(*r);
}

}  // namespace fastmm::venues::deribit
