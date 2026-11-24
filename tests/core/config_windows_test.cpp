// [[windows]] in the configuration: what it reads, what it refuses at load and the effective text.
#include "test_support.hpp"

#include "fastmm/config/config.hpp"

#include <string>
#include <vector>

using namespace fastmm;

namespace {

const std::string kBase = R"(
[venues.a]
kind = "sim"
[venues.b]
kind = "sim"
[[instruments]]
venue = "a"
symbol = "X"
tick = "0.01"
lot = "0.001"
[[instruments]]
venue = "b"
symbol = "Y"
tick = "0.01"
lot = "0.001"
[strategy]
name = "basic_mm"
)";

Config with_window(const std::string& w) {
  return Config::parse(kBase + "[[windows]]\n" + w);
}

const std::string kOk = "name = \"w\"\nzone = \"UTC\"\ndays = [\"mon\"]\nstart = \"10:00\"\n";

}  // namespace

TEST_CASE("core.config: [[windows]] read, resolved and written back") {
  const Config none = Config::parse(kBase);
  CHECK(none.windows.empty());
  CHECK(none.effective_toml().find("windows") == std::string::npos);

  const Config on = Config::parse(kBase + R"(
[[windows]]
name = "reopen"
zone = "America/New_York"
days = ["sun"]
start = "20:00"
duration = "30m"
action = "reduce_only"
except = ["2026-12-27"]
until = "2027-06-30"

[[windows]]
name = "cpi"
zone = "America/New_York"
dates = ["2027-01-13", "2026-12-10"]
start = "08:25"
end = "08:45:30"
venues = ["b"]
instruments = ["X"]
)");
  REQUIRE(on.windows.size() == 2);
  const std::vector<std::string> venues{"a", "b"};
  const Config back = Config::parse(on.effective_toml());
  CHECK(back.effective_toml() == on.effective_toml());
  for (const Config* c : {&on, &back}) {
    const InstrumentTable table = load_instruments(*c);
    const WindowsConfig w = window_config(*c, table, venues);
    REQUIRE(w.rules.size() == 2);
    const WindowRule& r = w.rules[0];
    CHECK(r.name == "reopen");
    CHECK(r.zone.name() == "America/New_York");
    CHECK(r.weekdays == 64);
    CHECK(r.start_s == 72000);
    CHECK(r.duration_s == 1800);
    CHECK(r.end_s == -1);
    CHECK(r.action == WindowAction::ReduceOnly);
    CHECK(r.whole_session());
    CHECK(r.except == std::vector<std::int32_t>{*parse_local_date("2026-12-27")});
    CHECK(r.until_day == *parse_local_date("2027-06-30"));
    const WindowRule& e = w.rules[1];
    CHECK(e.dates == std::vector<std::int32_t>{*parse_local_date("2026-12-10"),
                                               *parse_local_date("2027-01-13")});
    CHECK(e.end_s == 8 * 3600 + 45 * 60 + 30);
    CHECK(e.action == WindowAction::Pull);
    CHECK(e.venues == 2U);
    CHECK(e.instruments == std::vector<InstrumentId>{InstrumentId{0}});
    CHECK_FALSE(e.whole_session());
  }
  CHECK(on.redacted().find("[[windows]]") != std::string::npos);
  CHECK(on.redacted().find("zone = \"America/New_York\"") != std::string::npos);
}

TEST_CASE("core.config: [[windows]] mistakes fail the load and say what is wrong") {
  CHECK_NOTHROW(with_window(kOk + "duration = \"1h\"\n"));
  CHECK_THROWS_WITH_AS(with_window("name = \"w\"\nzone = \"Mars/Olympus\"\ndays = [\"mon\"]\n"
                                   "start = \"10:00\"\nduration = \"1h\"\n"),
                       doctest::Contains("not in the tzdata"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(with_window(kOk), doctest::Contains("give end or duration"), ConfigError);
  CHECK_THROWS_WITH_AS(with_window(kOk + "end = \"11:00\"\nduration = \"1h\"\n"),
                       doctest::Contains("one of them"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(
      with_window(kOk + "duration = \"8d\"\n"), doctest::Contains("at most 7d"), ConfigError);
  CHECK_THROWS_WITH_AS(with_window(kOk + "duration = \"an hour\"\n"),
                       doctest::Contains("not a duration"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(with_window(kOk + "duration = \"1h\"\ndates = [\"2026-12-10\"]\n"),
                       doctest::Contains("give days or dates"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(with_window("name = \"w\"\nzone = \"UTC\"\ndays = [\"funday\"]\n"
                                   "start = \"10:00\"\nduration = \"1h\"\n"),
                       doctest::Contains("'funday' is not a weekday"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(with_window("name = \"w\"\nzone = \"UTC\"\ndays = [\"mon\"]\n"
                                   "start = \"7pm\"\nduration = \"1h\"\n"),
                       doctest::Contains("is not a time"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(with_window(kOk + "duration = \"1h\"\nexcept = [\"2026-02-30\"]\n"),
                       doctest::Contains("is not a date"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(
      with_window(kOk + "duration = \"1h\"\nfrom = \"2027-01-01\"\nuntil = \"2026-01-01\"\n"),
      doctest::Contains("before from"),
      ConfigError);
  CHECK_THROWS_WITH_AS(with_window(kOk + "duration = \"1h\"\naction = \"cancel\"\n"),
                       doctest::Contains("\"pull\" or \"reduce_only\""),
                       ConfigError);
  CHECK_THROWS_WITH_AS(with_window(kOk + "duration = \"1h\"\nvenues = [\"c\"]\n"),
                       doctest::Contains("no venue named 'c'"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(with_window(kOk + "duration = \"1h\"\nlead = \"5m\"\n"),
                       doctest::Contains("unknown key 'windows[].lead'"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(
      with_window(kOk + "duration = \"1h\"\n[[windows]]\n" + kOk + "duration = \"1h\"\n"),
      doctest::Contains("two windows are named 'w'"),
      ConfigError);
  // A mistake carries its line.
  CHECK_THROWS_WITH_AS(with_window(kOk + "duration = \"1h\"\naction = \"cancel\"\n"),
                       doctest::Contains("(line "),
                       ConfigError);

  const Config unknown = with_window(kOk + "duration = \"1h\"\ninstruments = [\"Q\"]\n");
  const InstrumentTable table = load_instruments(unknown);
  const std::vector<std::string> venues{"a", "b"};
  CHECK_THROWS_WITH_AS(window_config(unknown, table, venues),
                       doctest::Contains("window 'w': no instrument 'Q'"),
                       ConfigError);
}
