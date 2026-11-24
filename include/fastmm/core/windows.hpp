#pragma once
// Recurring time windows ([[windows]]): on given weekdays or dates, from a local time in a time
// zone, for a duration or up to a local end time, the engine pulls the quotes of a scope or holds
// it reduce-only. The engine evaluates them on its event clock (core/engine.hpp), so a window
// opens at the same event live, in a replay and in a backtest, and daylight saving time and the
// calendar need no edits: the zone data is compiled in (core/time_zone.hpp).
//
// An occurrence starts on a local date D at the first instant the zone's clock reads D + start
// (a start the clock skips opens at the jump; one it shows twice opens at the first), and ends at
// the first instant it reads D + start + duration (or the end time: on D when later than start,
// otherwise on D + 1). The duration is wall-clock time: a window from Friday 17:00 lasting 48h
// ends Sunday 17:00 local across a change of offset. Occurrences may overlap; the window is open
// while any is.
//
// Seconds since 1970-01-01 throughout (UTC for instants, local for wall-clock times). active()
// and next_edge() do not allocate or throw.
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time_zone.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

enum class WindowAction : std::uint8_t {
  Pull = 0,        // no quotes in the scope (as fastmm-ctl pull)
  ReduceOnly = 1,  // refuse orders that add to a position (as fastmm-ctl reduce-only on)
};
[[nodiscard]] constexpr std::string_view to_string(WindowAction a) noexcept {
  return a == WindowAction::Pull ? "pull" : "reduce_only";
}

inline constexpr std::size_t kMaxWindows = 32;
inline constexpr std::int64_t kMaxWindowSeconds = std::int64_t{7} * 86'400;
inline constexpr std::int64_t kNoEdge = std::numeric_limits<std::int64_t>::max();

struct WindowRule {
  std::string name;
  TimeZone zone;
  std::uint8_t weekdays = 0;        // bit 0 Monday ... bit 6 Sunday; 0 with `dates`
  std::vector<std::int32_t> dates;  // local days since 1970-01-01, sorted: these only
  std::int32_t from_day = std::numeric_limits<std::int32_t>::min();   // inclusive
  std::int32_t until_day = std::numeric_limits<std::int32_t>::max();  // inclusive
  std::vector<std::int32_t> except;  // sorted: no occurrence starts on these
  std::int32_t start_s = 0;          // local time of day
  std::int32_t end_s = -1;           // local time of day; -1: start + duration_s
  std::int64_t duration_s = 0;
  WindowAction action = WindowAction::Pull;
  // The scope: every instrument of these venues (bit per venue id) and these instruments; both
  // empty: every instrument of the session.
  std::uint32_t venues = 0;
  std::vector<InstrumentId> instruments;

  [[nodiscard]] bool whole_session() const noexcept { return venues == 0 && instruments.empty(); }
  // An occurrence starts on local day `day`.
  [[nodiscard]] bool on_day(std::int32_t day) const noexcept;
  // That occurrence's [start, end) in UTC seconds (start >= end: empty, a gap swallowed it).
  struct Span {
    std::int64_t start;
    std::int64_t end;
  };
  [[nodiscard]] Span occurrence(std::int32_t day) const noexcept;
  // Open at that UTC second.
  [[nodiscard]] bool active(std::int64_t utc_s) const noexcept;
  // The first UTC second after `utc_s` at which an occurrence starts or ends (where active() may
  // change); kNoEdge: none.
  [[nodiscard]] std::int64_t next_edge(std::int64_t utc_s) const noexcept;
};

struct WindowsConfig {
  std::vector<WindowRule> rules;  // kMaxWindows at most
  [[nodiscard]] bool any() const noexcept { return !rules.empty(); }
};

// The pieces of a [[windows]] table, for the configuration loader. nullopt: not one.
[[nodiscard]] std::optional<std::int32_t> parse_local_date(std::string_view text);  // YYYY-MM-DD
[[nodiscard]] std::optional<std::int32_t> parse_local_time(std::string_view text);  // HH:MM[:SS]
// "90s", "45m", "2h", "1h30m", "2d12h": the units d, h, m, s, each at most once, in that order.
[[nodiscard]] std::optional<std::int64_t> parse_duration(std::string_view text);
// "mon" ... "sun" (or the full name); the bit of WindowRule::weekdays.
[[nodiscard]] std::optional<std::uint8_t> parse_weekday(std::string_view text);
// "YYYY-MM-DD" of a local day.
[[nodiscard]] std::string format_local_date(std::int32_t day);

}  // namespace fastmm
