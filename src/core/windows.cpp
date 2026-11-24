#include "fastmm/core/windows.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>

namespace fastmm {

namespace {

constexpr std::int64_t kDay = 86'400;
// An occurrence starting on local day d starts no earlier than this in UTC (offsets stay within
// a day of UTC, and a skipped start only moves later).
constexpr std::int64_t earliest_start(std::int64_t d) noexcept {
  return d * kDay - 2 * kDay;
}

bool digits(std::string_view s) noexcept {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::int64_t to_int(std::string_view s) noexcept {
  std::int64_t v = 0;
  for (const char c : s) v = v * 10 + (c - '0');
  return v;
}

}  // namespace

bool WindowRule::on_day(std::int32_t day) const noexcept {
  if (day < from_day || day > until_day) return false;
  if (std::binary_search(except.begin(), except.end(), day)) return false;
  if (!dates.empty()) return std::binary_search(dates.begin(), dates.end(), day);
  return ((weekdays >> civil_weekday(day)) & 1U) != 0;
}

WindowRule::Span WindowRule::occurrence(std::int32_t day) const noexcept {
  const std::int64_t d = day;
  const std::int64_t local_start = d * kDay + start_s;
  const std::int64_t local_end =
      end_s >= 0 ? (d + (end_s <= start_s ? 1 : 0)) * kDay + end_s : local_start + duration_s;
  return {zone.to_utc(local_start), zone.to_utc(local_end)};
}

bool WindowRule::active(std::int64_t utc_s) const noexcept {
  const std::int64_t today = floor_div(utc_s + zone.offset_at(utc_s), kDay);
  const std::int64_t back = kMaxWindowSeconds / kDay + 2;
  for (std::int64_t d = today - back; d <= today + 1; ++d) {
    const auto day = static_cast<std::int32_t>(d);
    if (!on_day(day)) continue;
    const Span s = occurrence(day);
    if (s.start <= utc_s && utc_s < s.end) return true;
  }
  return false;
}

std::int64_t WindowRule::next_edge(std::int64_t utc_s) const noexcept {
  const std::int64_t today = floor_div(utc_s + zone.offset_at(utc_s), kDay);
  std::int64_t first = today - (kMaxWindowSeconds / kDay + 2);
  std::int64_t best = kNoEdge;
  const auto consider = [&](std::int64_t d) {
    const auto day = static_cast<std::int32_t>(d);
    if (!on_day(day)) return;
    const Span s = occurrence(day);
    if (s.start >= s.end) return;
    if (s.start > utc_s) best = std::min(best, s.start);
    if (s.end > utc_s) best = std::min(best, s.end);
  };
  if (!dates.empty()) {
    for (auto it = std::lower_bound(dates.begin(), dates.end(), static_cast<std::int32_t>(first));
         it != dates.end() && earliest_start(*it) <= best;
         ++it) {
      consider(*it);
    }
    return best;
  }
  if (weekdays == 0) return kNoEdge;
  first = std::max<std::int64_t>(first, from_day);
  // A weekday of the rule comes every week, and each exception removes at most one week's: past
  // the days still open now, one week per exception and one more holds an occurrence, or the
  // range has ended.
  const std::int64_t last =
      std::min<std::int64_t>(until_day,
                             first + 2 * (kMaxWindowSeconds / kDay + 2) +
                                 7 * (static_cast<std::int64_t>(except.size()) + 1));
  for (std::int64_t d = first; d <= last && earliest_start(d) <= best; ++d) consider(d);
  return best;
}

std::optional<std::int32_t> parse_local_date(std::string_view s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
  const std::string_view y = s.substr(0, 4);
  const std::string_view m = s.substr(5, 2);
  const std::string_view d = s.substr(8, 2);
  if (!digits(y) || !digits(m) || !digits(d)) return std::nullopt;
  const std::int64_t yy = to_int(y);
  const auto mm = static_cast<unsigned>(to_int(m));
  const auto dd = static_cast<unsigned>(to_int(d));
  if (yy < 1970 || mm < 1 || mm > 12 || dd < 1) return std::nullopt;
  const std::int64_t day = civil_days(yy, mm, dd);
  const CivilDate back = civil_date(day);
  if (back.m != mm || back.d != dd) return std::nullopt;  // 2026-02-30
  return static_cast<std::int32_t>(day);
}

std::optional<std::int32_t> parse_local_time(std::string_view s) {
  if ((s.size() != 5 && s.size() != 8) || s[2] != ':' || (s.size() == 8 && s[5] != ':'))
    return std::nullopt;
  const std::string_view h = s.substr(0, 2);
  const std::string_view m = s.substr(3, 2);
  const std::string_view sec = s.size() == 8 ? s.substr(6, 2) : std::string_view("00");
  if (!digits(h) || !digits(m) || !digits(sec)) return std::nullopt;
  const std::int64_t hh = to_int(h);
  const std::int64_t mm = to_int(m);
  const std::int64_t ss = to_int(sec);
  if (hh > 23 || mm > 59 || ss > 59) return std::nullopt;
  return static_cast<std::int32_t>(hh * 3600 + mm * 60 + ss);
}

std::optional<std::int64_t> parse_duration(std::string_view s) {
  static constexpr std::array<std::pair<char, std::int64_t>, 4> kUnits = {
      {{'d', kDay}, {'h', 3600}, {'m', 60}, {'s', 1}}};
  std::int64_t total = 0;
  std::size_t unit = 0;
  std::size_t i = 0;
  if (s.empty()) return std::nullopt;
  while (i < s.size()) {
    const std::size_t from = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == from || i == s.size() || i - from > 9) return std::nullopt;
    const std::int64_t n = to_int(s.substr(from, i - from));
    while (unit < kUnits.size() && kUnits[unit].first != s[i]) ++unit;
    if (unit == kUnits.size()) return std::nullopt;  // unknown, repeated or out of order
    total += n * kUnits[unit].second;
    ++unit;
    ++i;
  }
  return total;
}

std::optional<std::uint8_t> parse_weekday(std::string_view s) {
  static constexpr std::array<std::string_view, 7> kShort = {
      "mon", "tue", "wed", "thu", "fri", "sat", "sun"};
  static constexpr std::array<std::string_view, 7> kLong = {
      "monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
  for (std::size_t k = 0; k < kShort.size(); ++k) {
    if (s == kShort[k] || s == kLong[k]) return static_cast<std::uint8_t>(1U << k);
  }
  return std::nullopt;
}

std::string format_local_date(std::int32_t day) {
  const CivilDate c = civil_date(day);
  return fmt::format("{:04}-{:02}-{:02}", c.y, c.m, c.d);
}

}  // namespace fastmm
