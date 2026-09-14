#pragma once
// FIX UTCTimestamp <-> ns since the Unix epoch, without the C library (no timegm/gmtime_r on
// the hot path, no locale, no allocation).
//
// FIX 4.4 defines UTCTimestamp as "YYYYMMDD-HH:MM:SS" (whole seconds) or
// "YYYYMMDD-HH:MM:SS.sss" (milliseconds); see the FIX 4.4 data types
// (https://www.b2bits.com/fixopaedia/fixdic44/data_types.html). The parser also accepts 6 or 9
// fraction digits, which later FIX versions (and many FIX 4.4 venues) send; the writer emits
// milliseconds only, as FIX 4.4 specifies. A leap second (SS = 60) folds into the next second.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fastmm::codecs::fix {

inline constexpr std::size_t kUtcTimestampMillisChars = 21;  // YYYYMMDD-HH:MM:SS.sss

namespace detail {
// Howard Hinnant's days_from_civil / civil_from_days (proleptic Gregorian, public domain).
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y,
                                                     unsigned m,
                                                     unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
struct CivilDate {
  std::int64_t y;
  unsigned m;
  unsigned d;
};
[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {y + (m <= 2 ? 1 : 0), m, d};
}
[[nodiscard]] constexpr bool read_digits(std::string_view s,
                                         std::size_t pos,
                                         std::size_t n,
                                         unsigned& out) noexcept {
  unsigned v = 0;
  for (std::size_t i = pos; i < pos + n; ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  out = v;
  return true;
}
constexpr void put2(char* p, unsigned v) noexcept {
  p[0] = static_cast<char>('0' + v / 10);
  p[1] = static_cast<char>('0' + v % 10);
}
}  // namespace detail

// "YYYYMMDD-HH:MM:SS[.sss|.ssssss|.sssssssss]" -> ns since epoch; nullopt when malformed.
[[nodiscard]] constexpr std::optional<std::int64_t> parse_utc_timestamp(
    std::string_view s) noexcept {
  if (s.size() != 17 && s.size() != 21 && s.size() != 24 && s.size() != 27) return std::nullopt;
  unsigned year = 0;
  unsigned mon = 0;
  unsigned day = 0;
  unsigned hh = 0;
  unsigned mm = 0;
  unsigned ss = 0;
  if (!detail::read_digits(s, 0, 4, year) || !detail::read_digits(s, 4, 2, mon) ||
      !detail::read_digits(s, 6, 2, day) || s[8] != '-' || !detail::read_digits(s, 9, 2, hh) ||
      s[11] != ':' || !detail::read_digits(s, 12, 2, mm) || s[14] != ':' ||
      !detail::read_digits(s, 15, 2, ss))
    return std::nullopt;
  if (mon < 1 || mon > 12 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60)
    return std::nullopt;
  std::int64_t frac_ns = 0;
  if (s.size() > 17) {
    if (s[17] != '.') return std::nullopt;
    const std::size_t n = s.size() - 18;
    unsigned f = 0;
    if (!detail::read_digits(s, 18, n, f)) return std::nullopt;
    if (n == 3) {
      frac_ns = static_cast<std::int64_t>(f) * 1'000'000;
    } else if (n == 6) {
      frac_ns = static_cast<std::int64_t>(f) * 1'000;
    } else {
      frac_ns = static_cast<std::int64_t>(f);
    }
  }
  const std::int64_t days = detail::days_from_civil(year, mon, day);
  const std::int64_t secs = days * 86'400 + static_cast<std::int64_t>(hh) * 3'600 +
                            static_cast<std::int64_t>(mm) * 60 + static_cast<std::int64_t>(ss);
  return secs * 1'000'000'000 + frac_ns;
}

// Writes "YYYYMMDD-HH:MM:SS.sss" (21 chars, no NUL) for ns >= 0; returns chars written.
constexpr std::size_t format_utc_timestamp(std::int64_t ns, char* out) noexcept {
  if (ns < 0) ns = 0;
  const std::int64_t secs = ns / 1'000'000'000;
  const auto ms = static_cast<unsigned>((ns / 1'000'000) % 1'000);
  const std::int64_t days = secs / 86'400;
  auto sod = static_cast<unsigned>(secs % 86'400);
  const detail::CivilDate c = detail::civil_from_days(days);
  const auto y = static_cast<unsigned>(c.y);
  out[0] = static_cast<char>('0' + (y / 1000) % 10);
  out[1] = static_cast<char>('0' + (y / 100) % 10);
  out[2] = static_cast<char>('0' + (y / 10) % 10);
  out[3] = static_cast<char>('0' + y % 10);
  detail::put2(out + 4, c.m);
  detail::put2(out + 6, c.d);
  out[8] = '-';
  detail::put2(out + 9, sod / 3600);
  sod %= 3600;
  out[11] = ':';
  detail::put2(out + 12, sod / 60);
  out[14] = ':';
  detail::put2(out + 15, sod % 60);
  out[17] = '.';
  out[18] = static_cast<char>('0' + ms / 100);
  out[19] = static_cast<char>('0' + (ms / 10) % 10);
  out[20] = static_cast<char>('0' + ms % 10);
  return kUtcTimestampMillisChars;
}

}  // namespace fastmm::codecs::fix
