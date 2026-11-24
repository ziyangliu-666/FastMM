// The compiled-in zones (core/time_zone.hpp) against IANA tzdata as Python's zoneinfo reads it, and
// the recurring windows built on them (core/windows.hpp), across both changes of daylight saving.
#include "fastmm/core/time_zone.hpp"

#include "test_support.hpp"

#include "fastmm/core/windows.hpp"

#include <string>

using namespace fastmm;

namespace {

TimeZone zone(const char* name) {
  std::string why;
  std::optional<TimeZone> z = TimeZone::find(name, &why);
  INFO(why);
  REQUIRE(z.has_value());
  return *z;
}

std::int32_t day(const char* date) {
  const std::optional<std::int32_t> d = parse_local_date(date);
  REQUIRE(d.has_value());
  return *d;
}

// Sunday 20:00 New York for 30 minutes: the weekly reopen.
WindowRule weekly_reopen() {
  WindowRule w;
  w.name = "reopen";
  w.zone = zone("America/New_York");
  w.weekdays = *parse_weekday("sun");
  w.start_s = 20 * 3600;
  w.duration_s = 30 * 60;
  return w;
}

constexpr std::int64_t kHour = 3600;

}  // namespace

TEST_CASE("core.time_zone: civil dates round-trip and weekdays count from Monday") {
  CHECK(civil_days(1970, 1, 1) == 0);
  CHECK(civil_days(2000, 3, 1) == 11017);
  for (std::int64_t d = -800000; d < 800000; d += 997) {
    const CivilDate c = civil_date(d);
    CHECK(civil_days(c.y, c.m, c.d) == d);
  }
  CHECK(civil_weekday(0) == 3);                         // 1970-01-01, a Thursday
  CHECK(civil_weekday(civil_days(2026, 11, 22)) == 6);  // a Sunday
  CHECK(civil_weekday(-1) == 2);
}

TEST_CASE("core.time_zone: offsets match tzdata at and around each change") {
  struct Case {
    const char* zone;
    std::int64_t utc;
    std::int32_t offset;
  };
  // Python zoneinfo over the same tzdata release (2026e), /usr/share/zoneinfo.
  const Case cases[] = {
      {"America/New_York", 1772953199, -18000},    // 2026-03-08T06:59:59Z
      {"America/New_York", 1772953200, -14400},    // 2026-03-08T07:00:00Z
      {"America/New_York", 1793512799, -14400},    // 2026-11-01T05:59:59Z
      {"America/New_York", 1793512800, -18000},    // 2026-11-01T06:00:00Z
      {"America/New_York", 1143961199, -18000},    // 2006-04-02T06:59:59Z, the old US rule
      {"America/New_York", 1143961200, -14400},    // 2006-04-02T07:00:00Z
      {"America/New_York", 1162101599, -14400},    // 2006-10-29T05:59:59Z
      {"America/New_York", 1162101600, -18000},    // 2006-10-29T06:00:00Z
      {"Europe/London", 1774745999, 0},            // 2026-03-29T00:59:59Z
      {"Europe/London", 1774746000, 3600},         // 2026-03-29T01:00:00Z
      {"Europe/London", 1792889999, 3600},         // 2026-10-25T00:59:59Z
      {"Europe/London", 1792890000, 0},            // 2026-10-25T01:00:00Z
      {"Australia/Sydney", 1775318399, 39600},     // 2026-04-04T15:59:59Z
      {"Australia/Sydney", 1775318400, 36000},     // 2026-04-04T16:00:00Z
      {"Australia/Sydney", 1791043199, 36000},     // 2026-10-03T15:59:59Z
      {"Australia/Sydney", 1791043200, 39600},     // 2026-10-03T16:00:00Z
      {"Australia/Lord_Howe", 1775314799, 39600},  // 2026-04-04T14:59:59Z, half-hour DST
      {"Australia/Lord_Howe", 1775314800, 37800},  // 2026-04-04T15:00:00Z
      {"Australia/Lord_Howe", 1791041399, 37800},  // 2026-10-03T15:29:59Z
      {"Australia/Lord_Howe", 1791041400, 39600},  // 2026-10-03T15:30:00Z
      {"Asia/Kolkata", 1780272000, 19800},         // 2026-06-01
      {"Asia/Tokyo", 991353600, 32400},            // 2001-06-01
      {"America/Sao_Paulo", 1541300399, -10800},   // 2018-11-04T02:59:59Z, DST until 2019
      {"America/Sao_Paulo", 1541300400, -7200},    // 2018-11-04T03:00:00Z
      {"America/Sao_Paulo", 1767225600, -10800},   // 2026-01-01, none since
      {"Asia/Tehran", 1622505600, 16200},          // 2021-06-01, DST until 2022
      {"Asia/Tehran", 1780272000, 12600},          // 2026-06-01
      {"America/Santiago", 1775357999, -10800},    // 2026-04-05T02:59:59Z
      {"America/Santiago", 1775358000, -14400},    // 2026-04-05T03:00:00Z
      {"America/Santiago", 1788667199, -14400},    // 2026-09-06T03:59:59Z
      {"America/Santiago", 1788667200, -10800},    // 2026-09-06T04:00:00Z
      {"Pacific/Chatham", 1775311199, 49500},      // 2026-04-04T13:59:59Z, +12:45/+13:45
      {"Pacific/Chatham", 1775311200, 45900},      // 2026-04-04T14:00:00Z
      {"Africa/Casablanca", 1771120799, 3600},     // 2026-02-15T01:59:59Z
      {"Africa/Casablanca", 1771120800, 0},        // 2026-02-15T02:00:00Z
      {"Africa/Casablanca", 1893456000, 0},        // 2030-01-01
      {"US/Eastern", 1814400000, -14400},          // 2027-07-01, a link
      {"Europe/Berlin", 2224713600, 7200},         // 2040-07-01, past the explicit data
      {"Europe/Berlin", 2237932800, 3600},         // 2040-12-01
  };
  for (const Case& c : cases) {
    INFO(c.zone << " at " << c.utc);
    CHECK(zone(c.zone).offset_at(c.utc) == c.offset);
  }
  CHECK(tzdata_version() == "2026e");
}

TEST_CASE("core.time_zone: a POSIX rule is a zone, and an unknown name says why") {
  const TimeZone posix = zone("EST5EDT,M3.2.0,M11.1.0");
  const TimeZone ny = zone("America/New_York");
  for (std::int64_t t = 1767225600; t < 1798761600; t += 3 * kHour + 17)  // 2026
    REQUIRE(posix.offset_at(t) == ny.offset_at(t));
  CHECK(zone("UTC").offset_at(0) == 0);
  CHECK(zone("<+0545>-5:45").offset_at(1780272000) == 20700);

  std::string why;
  CHECK_FALSE(TimeZone::find("America/Gotham", &why).has_value());
  CHECK(why.find("tzdata 2026e") != std::string::npos);
  CHECK_FALSE(TimeZone::find("EST5EDT,M3.2", &why).has_value());
  CHECK(why.find("POSIX") != std::string::npos);
  CHECK_FALSE(TimeZone::find("", &why).has_value());
}

TEST_CASE("core.time_zone: a skipped local time is the jump, a repeated one its first instant") {
  const TimeZone ny = zone("America/New_York");
  const std::int64_t mar8 = std::int64_t{day("2026-03-08")} * 86400;
  const std::int64_t nov1 = std::int64_t{day("2026-11-01")} * 86400;
  CHECK(ny.to_utc(mar8 + 2 * kHour + 1800) == 1772953200);  // 02:30 does not exist: 07:00Z
  CHECK(ny.to_utc(mar8 + 2 * kHour) == 1772953200);
  CHECK(ny.to_utc(mar8 + 3 * kHour) == 1772953200);  // 03:00 EDT is the jump itself
  CHECK(ny.to_utc(mar8 + 1 * kHour) == 1772953200 - kHour);
  CHECK(ny.to_utc(nov1 + 1 * kHour + 1800) == 1793511000);  // 01:30 EDT, not 01:30 EST
  CHECK(ny.to_utc(nov1 + 2 * kHour) == 1793512800 + kHour);
  CHECK(ny.to_utc(nov1 + 20 * kHour) == 1793581200);
}

TEST_CASE("core.windows: the parsers") {
  CHECK(parse_local_date("2026-12-24") == civil_days(2026, 12, 24));
  CHECK_FALSE(parse_local_date("2026-02-30"));
  CHECK_FALSE(parse_local_date("2026-2-3"));
  CHECK(parse_local_time("20:00") == 72000);
  CHECK(parse_local_time("09:30:15") == 34215);
  CHECK_FALSE(parse_local_time("24:00"));
  CHECK_FALSE(parse_local_time("9:30"));
  CHECK(parse_duration("30m") == 1800);
  CHECK(parse_duration("1h30m") == 5400);
  CHECK(parse_duration("2d12h") == 216000);
  CHECK(parse_duration("90s") == 90);
  CHECK_FALSE(parse_duration("30"));
  CHECK_FALSE(parse_duration("30m1h"));
  CHECK_FALSE(parse_duration("1h1h"));
  CHECK_FALSE(parse_duration("1w"));
  CHECK(parse_weekday("sun") == 64);
  CHECK(parse_weekday("monday") == 1);
  CHECK_FALSE(parse_weekday("Sun"));
  CHECK(format_local_date(day("2026-03-08")) == "2026-03-08");
}

TEST_CASE("core.windows: Sunday 20:00 New York keeps its local time across both changes") {
  const WindowRule w = weekly_reopen();
  // EST before, EDT after the change of 2026-03-08 02:00, which falls on the window's own day.
  CHECK(w.next_edge(1772413200 - 1) == 1772413200);  // 03-01 20:00 EST = 03-02 01:00Z
  CHECK(w.active(1772413200));
  CHECK(w.next_edge(1772413200) == 1772413200 + 1800);
  CHECK_FALSE(w.active(1772413200 + 1800));
  CHECK(w.next_edge(1772413200 + 1800) == 1773014400);  // 03-08 20:00 EDT = 03-09 00:00Z
  CHECK_FALSE(w.active(1773014400 - 1));
  CHECK(w.active(1773014400));
  CHECK_FALSE(w.active(1773014400 + 3600));  // where an EST clock would have it
  // Back to EST on 2026-11-01, also a Sunday.
  CHECK(w.next_edge(1792972800 - 10) == 1792972800);    // 10-25 20:00 EDT = 10-26 00:00Z
  CHECK(w.next_edge(1792972800 + 1800) == 1793581200);  // 11-01 20:00 EST = 11-02 01:00Z
  CHECK(w.active(1793581200));
  CHECK_FALSE(w.active(1793581200 - 1));
}

TEST_CASE("core.windows: a duration is wall-clock time, and a skipped start opens at the jump") {
  WindowRule w = weekly_reopen();
  w.weekdays = *parse_weekday("fri");
  w.start_s = 17 * 3600;
  w.duration_s = 48 * kHour;
  // Friday 03-06 17:00 EST (22:00Z) to Sunday 03-08 17:00 EDT (21:00Z): 47 hours.
  const WindowRule::Span s = w.occurrence(day("2026-03-06"));
  CHECK(s.start == 1772834400);
  CHECK(s.end == 1773003600);
  CHECK(w.active(1772884800));  // Saturday noon
  CHECK(w.next_edge(1772884800) == 1773003600);

  WindowRule daily = weekly_reopen();
  daily.weekdays = 0x7F;
  daily.start_s = 2 * 3600 + 1800;  // 02:30, which 2026-03-08 never shows
  daily.duration_s = 3600;
  const WindowRule::Span gap = daily.occurrence(day("2026-03-08"));
  CHECK(gap.start == 1772953200);       // 03:00 EDT, the jump
  CHECK(gap.end == 1772953200 + 1800);  // 03:30 EDT, local 02:30 + 1h
  daily.start_s = 1 * 3600 + 1800;      // 01:30, which 2026-11-01 shows twice
  const WindowRule::Span twice = daily.occurrence(day("2026-11-01"));
  CHECK(twice.start == 1793511000);  // the first, EDT
  CHECK(twice.end == 1793518200);    // 02:30 EST, two hours on
}

TEST_CASE("core.windows: dates, ranges, exceptions and an end past midnight") {
  WindowRule w = weekly_reopen();
  w.weekdays = 0;
  w.dates = {day("2026-12-10")};
  w.zone = zone("America/New_York");
  w.start_s = 8 * 3600 + 25 * 60;  // 08:25 EST = 13:25Z
  w.duration_s = 5 * 60;
  CHECK(w.next_edge(1795392000) == 1796909100);
  CHECK(w.active(1796909100));
  CHECK(w.next_edge(1796909100) == 1796909400);
  CHECK(w.next_edge(1796909400) == kNoEdge);
  CHECK_FALSE(w.active(1796909400));

  WindowRule r = weekly_reopen();
  r.until_day = day("2026-03-01");
  CHECK(r.next_edge(1772413200 + 1800) == kNoEdge);  // the last one has closed
  r.until_day = std::numeric_limits<std::int32_t>::max();
  r.except = {day("2026-03-08"), day("2026-03-15")};
  CHECK(r.next_edge(1772413200 + 1800) == 1773014400 + 14 * 86400);  // 03-22
  r.except.clear();
  r.from_day = day("2026-10-26");
  CHECK(r.next_edge(1772413200) == 1793581200);

  WindowRule night = weekly_reopen();
  night.weekdays = *parse_weekday("mon");
  night.zone = TimeZone{};  // UTC
  night.start_s = 22 * 3600;
  night.end_s = 2 * 3600;  // the next day
  const std::int64_t mon = std::int64_t{day("2026-11-23")} * 86400;
  CHECK(night.occurrence(day("2026-11-23")).start == mon + 22 * kHour);
  CHECK(night.occurrence(day("2026-11-23")).end == mon + 26 * kHour);
  CHECK(night.active(mon + 25 * kHour));
  CHECK_FALSE(night.active(mon + 26 * kHour));
}

TEST_CASE("core.windows: overlapping occurrences keep the window open") {
  WindowRule w;
  w.name = "long";
  w.weekdays = 0x7F;  // every day, 36 hours from midnight UTC
  w.duration_s = 36 * kHour;
  const std::int64_t d0 = std::int64_t{day("2026-11-23")} * 86400;
  for (std::int64_t t = d0; t < d0 + 7 * 86400; t += 600) REQUIRE(w.active(t));
  // Every edge lies inside another occurrence: the next one is still an edge.
  CHECK(w.next_edge(d0) == d0 + 12 * kHour);
}
