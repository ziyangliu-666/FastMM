#include "fastmm/core/time_zone.hpp"

#include "tzdata_table.hpp"

#include <algorithm>
#include <cstring>

namespace fastmm {

namespace {

constexpr std::int64_t kDay = 86'400;

bool leap_year(std::int64_t y) noexcept {
  return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
}

// The day (since 1970-01-01) a rule's change falls on in year y.
std::int64_t change_day(const TzRule::Change& c, std::int64_t y) noexcept {
  const std::int64_t jan1 = civil_days(y, 1, 1);
  if (c.kind == 'J') return jan1 + c.a - 1 + (leap_year(y) && c.a >= 60 ? 1 : 0);
  if (c.kind == 'N') return jan1 + c.a;
  const auto m = static_cast<unsigned>(c.a);
  const std::int64_t first = civil_days(y, m, 1);
  // civil_weekday counts from Monday; the rule's d from Sunday.
  const auto first_wd = static_cast<std::int64_t>((civil_weekday(first) + 1) % 7);
  std::int64_t d = first + ((c.c - first_wd) % 7 + 7) % 7 + std::int64_t{7} * (c.b - 1);
  const std::int64_t next = m == 12 ? civil_days(y + 1, 1, 1) : civil_days(y, m + 1, 1);
  while (d >= next) d -= 7;
  return d;
}

// The reader of a POSIX TZ string.
struct Reader {
  std::string_view s;
  std::size_t i = 0;
  std::string error;

  [[nodiscard]] bool done() const noexcept { return i >= s.size(); }
  [[nodiscard]] char peek() const noexcept { return done() ? '\0' : s[i]; }
  bool fail(std::string_view why) {
    if (error.empty()) error = std::string(why);
    return false;
  }
  bool name() {
    if (peek() == '<') {
      const std::size_t close = s.find('>', i);
      if (close == std::string_view::npos || close - i < 4) return fail("a <name> is not closed");
      i = close + 1;
      return true;
    }
    const std::size_t from = i;
    while (!done() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) ++i;
    return i - from >= 3 ? true : fail("a zone abbreviation has fewer than 3 letters");
  }
  bool number(std::int64_t& out, std::int64_t max) {
    if (done() || s[i] < '0' || s[i] > '9') return fail("a number is missing");
    out = 0;
    while (!done() && s[i] >= '0' && s[i] <= '9') {
      out = out * 10 + (s[i] - '0');
      if (out > max) return fail("a number is out of range");
      ++i;
    }
    return true;
  }
  // [+-]hh[:mm[:ss]] in seconds, hours up to max_h.
  bool clock(std::int64_t& out, std::int64_t max_h) {
    std::int64_t sign = 1;
    if (peek() == '+' || peek() == '-') sign = s[i++] == '-' ? -1 : 1;
    std::int64_t h = 0;
    std::int64_t m = 0;
    std::int64_t sec = 0;
    if (!number(h, max_h)) return false;
    if (peek() == ':') {
      ++i;
      if (!number(m, 59)) return false;
      if (peek() == ':') {
        ++i;
        if (!number(sec, 59)) return false;
      }
    }
    out = sign * (h * 3600 + m * 60 + sec);
    return true;
  }
  bool change(TzRule::Change& c) {
    std::int64_t a = 0;
    std::int64_t b = 0;
    std::int64_t d = 0;
    if (peek() == 'J') {
      ++i;
      if (!number(a, 365) || a < 1) return fail("Jn takes a day from 1 to 365");
      c.kind = 'J';
    } else if (peek() == 'M') {
      ++i;
      if (!number(a, 12) || a < 1) return fail("Mm.w.d takes a month from 1 to 12");
      if (peek() != '.') return fail("Mm.w.d is missing a '.'");
      ++i;
      if (!number(b, 5) || b < 1) return fail("Mm.w.d takes a week from 1 to 5");
      if (peek() != '.') return fail("Mm.w.d is missing a '.'");
      ++i;
      if (!number(d, 6)) return fail("Mm.w.d takes a weekday from 0 to 6");
      c.kind = 'M';
    } else {
      if (!number(a, 365)) return fail("a day number takes 0 to 365");
      c.kind = 'N';
    }
    c.a = static_cast<std::int16_t>(a);
    c.b = static_cast<std::int16_t>(b);
    c.c = static_cast<std::int16_t>(d);
    c.time_s = 7200;
    if (peek() == '/') {
      ++i;
      std::int64_t t = 0;
      if (!clock(t, 167)) return false;
      c.time_s = static_cast<std::int32_t>(t);
    }
    return true;
  }
};

}  // namespace

std::int32_t TzRule::offset_at(std::int64_t utc_s) const noexcept {
  if (!has_dst) return std_offset;
  const std::int64_t y = civil_date(floor_div(utc_s + std_offset, kDay)).y;
  const std::int64_t on = change_day(start, y) * kDay + start.time_s - std_offset;
  const std::int64_t off = change_day(end, y) * kDay + end.time_s - dst_offset;
  const bool dst = on < off ? (utc_s >= on && utc_s < off) : !(utc_s >= off && utc_s < on);
  return dst ? dst_offset : std_offset;
}

std::string TzRule::parse(std::string_view text, TzRule& out) {
  Reader r{text, 0, {}};
  out = TzRule{};
  std::int64_t v = 0;
  if (!r.name() || !r.clock(v, 24)) return r.error;
  out.std_offset = static_cast<std::int32_t>(-v);
  if (r.done()) return {};
  if (!r.name()) return r.error;
  out.has_dst = true;
  out.dst_offset = out.std_offset + 3600;
  if (!r.done() && r.peek() != ',') {
    if (!r.clock(v, 24)) return r.error;
    out.dst_offset = static_cast<std::int32_t>(-v);
  }
  if (r.done()) {
    // No rule: the United States' since 2007, as glibc reads it.
    out.start = {'M', 3, 2, 0, 7200};
    out.end = {'M', 11, 1, 0, 7200};
    return {};
  }
  if (r.peek() != ',') return "expected ',' before the start of daylight saving time";
  ++r.i;
  if (!r.change(out.start)) return r.error;
  if (r.peek() != ',') return "expected ',' before the end of daylight saving time";
  ++r.i;
  if (!r.change(out.end)) return r.error;
  if (!r.done()) return "unexpected text after the rule";
  return {};
}

std::optional<TimeZone> TimeZone::find(std::string_view name, std::string* error) {
  const auto fail = [&](std::string why) -> std::optional<TimeZone> {
    if (error != nullptr) *error = std::move(why);
    return std::nullopt;
  };
  const tzdata::Zone* begin = tzdata::kZones;
  const tzdata::Zone* end = tzdata::kZones + tzdata::kZoneCount;
  const tzdata::Zone* z =
      std::lower_bound(begin, end, name, [](const tzdata::Zone& a, std::string_view n) {
        return std::string_view(a.name) < n;
      });
  TimeZone out;
  out.name_ = std::string(name);
  if (z != end && std::string_view(z->name) == name) {
    out.transitions_ = tzdata::kTransitions + z->first;
    out.count_ = z->count;
    out.offset0_ = z->offset0;
    out.rule_from_ = z->rule_from;
    if (z->rule[0] != '\0') {
      if (std::string why = TzRule::parse(z->rule, out.rule_); !why.empty())
        return fail("time zone '" + out.name_ + "': its rule does not read: " + why);
    } else {
      out.rule_from_ = kNoRule;
    }
    return out;
  }
  // A POSIX TZ string has an offset: a digit somewhere.
  if (name.find_first_of("0123456789") != std::string_view::npos) {
    if (std::string why = TzRule::parse(name, out.rule_); !why.empty())
      return fail("time zone '" + out.name_ + "' is not a POSIX TZ rule: " + why);
    out.offset0_ = out.rule_.std_offset;
    out.rule_from_ = std::numeric_limits<std::int64_t>::min();
    return out;
  }
  return fail("time zone '" + out.name_ + "' is not in the tzdata " +
              std::string(tzdata_version()) +
              " this build carries (an IANA name such as America/New_York, or a POSIX TZ rule)");
}

std::int32_t TimeZone::offset_at(std::int64_t utc_s) const noexcept {
  if (utc_s >= rule_from_) return rule_.offset_at(utc_s);
  const TzTransition* b = transitions_;
  const TzTransition* e = transitions_ + count_;
  const TzTransition* after =
      std::upper_bound(b, e, utc_s, [](std::int64_t t, const TzTransition& x) { return t < x.at; });
  return after == b ? offset0_ : (after - 1)->offset;
}

std::int64_t TimeZone::to_utc(std::int64_t local_s) const noexcept {
  // Offsets stay within a day of UTC: the offsets a day either side bound the answer.
  const std::int32_t before = offset_at(local_s - kDay - 3600);
  const std::int32_t after = offset_at(local_s + kDay + 3600);
  const std::int64_t u_before = local_s - before;
  const std::int64_t u_after = local_s - after;
  const bool ok_before = offset_at(u_before) == before;
  const bool ok_after = offset_at(u_after) == after;
  if (ok_before && ok_after) return std::min(u_before, u_after);  // shown twice: the first
  if (ok_before) return u_before;
  if (ok_after) return u_after;
  // Skipped: the instant of the jump, the first at which the clock reads past it. It lies in
  // (u_after, u_before]: offset_at is `before` up to it and `after` from it.
  std::int64_t lo = std::min(u_before, u_after);
  std::int64_t hi = std::max(u_before, u_after);
  while (hi - lo > 1) {
    const std::int64_t mid = lo + (hi - lo) / 2;
    if (offset_at(mid) == before) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

std::string_view tzdata_version() noexcept {
  return tzdata::kVersion;
}

}  // namespace fastmm
