// [ab] tests (strategies/ab_test.hpp): the configuration, the plan's three designs, and the
// controller that publishes each arm's parameters when an instrument changes arm.
#include "fastmm/strategies/ab_test.hpp"

#include "test_support.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/param_publisher.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

constexpr std::int64_t kSec = 1'000'000'000;

std::string config(std::string_view ab) {
  std::string out = R"(
[venues.sim]
kind = "sim"
)";
  for (const char* sym : {"AAA", "BBB", "CCC", "DDD", "EEE", "FFF"}) {
    out += "\n[[instruments]]\nvenue = \"sim\"\nsymbol = \"";
    out += sym;
    out += "\"\ntick = \"0.01\"\nlot = \"0.001\"\n";
  }
  out += "\n[strategy]\nname = \"basic_mm\"\n[strategy.params]\nhalf_spread_bps = 5\n";
  out += ab;
  return out;
}

constexpr std::string_view kCrossover = R"(
[ab]
name = "spread-1"
assignment = "crossover"
block_s = 3600
seed = 7

[ab.weights]
"sim:AAA" = 60
BBB = 50
CCC = 40
DDD = 30
EEE = 20
FFF = 10

[[ab.arms]]
name = "control"

[[ab.arms]]
name = "wide"
params = { half_spread_bps = 8, levels = 2 }
)";

Config parse(std::string_view ab) {
  return Config::parse(config(ab));
}

AbPlan plan_of(const Config& cfg) {
  const InstrumentTable table = load_instruments(cfg);
  std::vector<std::string> venues;
  for (const VenueSection& v : cfg.venues) venues.push_back(v.name);
  return AbPlan(cfg.ab, table, venues);
}

// A publisher whose updates land in a vector.
struct Captured {
  std::vector<ParamUpdateMsg> sent;
  bool refuse = false;
  ParamSink sink() {
    return {this, [](void* c, const ParamUpdateMsg& m) noexcept {
              auto* self = static_cast<Captured*>(c);
              if (self->refuse) return false;
              try {
                self->sent.push_back(m);
              } catch (...) {
                return false;
              }
              return true;
            }};
  }
};

std::int64_t field_value(const ParamUpdateMsg& m, std::string_view name) {
  const ParamDesc* d = BasicMMParams::schema().find(name);
  REQUIRE(d != nullptr);
  const auto index = static_cast<std::uint16_t>(d - BasicMMParams::schema().begin());
  for (std::uint32_t i = 0; i < m.count; ++i) {
    if (m.field[i] == index) return m.value[i];
  }
  FAIL("field not in the update");
  return 0;
}

}  // namespace

TEST_CASE("core.ab: UTC times parse and print") {
  CHECK(parse_utc_time("1970-01-01") == 0);
  CHECK(parse_utc_time("2026-10-22T15:00:00Z") == std::int64_t{1'792'681'200} * kSec);
  CHECK(parse_utc_time("2026-10-22 15:00") == std::int64_t{1'792'681'200} * kSec);
  CHECK(parse_utc_time("2024-02-29").has_value());
  CHECK_FALSE(parse_utc_time("2026-02-29").has_value());
  CHECK_FALSE(parse_utc_time("2026-10-22T25:00").has_value());
  CHECK_FALSE(parse_utc_time("tomorrow").has_value());
  CHECK_FALSE(parse_utc_time("2026-10-22T15:00:00+02").has_value());
  CHECK(format_utc_time(std::int64_t{1'792'681'200} * kSec) == "2026-10-22 15:00:00 UTC");
}

TEST_CASE("core.ab: the [ab] table parses, and its effective form parses to the same") {
  const Config cfg = parse(kCrossover);
  REQUIRE(cfg.ab.enabled());
  CHECK(cfg.ab.name == "spread-1");
  CHECK(cfg.ab.assignment == "crossover");
  CHECK(cfg.ab.block_s == 3600);
  CHECK(cfg.ab.seed == 7);
  CHECK(cfg.ab.weights.size() == 6);
  REQUIRE(cfg.ab.arms.size() == 2);
  CHECK(cfg.ab.arms[0].name == "control");
  CHECK(cfg.ab.arms[0].params.empty());
  CHECK(cfg.ab.arms[1].params.at("half_spread_bps") == "8");
  CHECK(cfg.ab.arms[1].params.at("levels") == "2");

  const Config again = Config::parse(cfg.effective_toml());
  CHECK(again.ab.name == cfg.ab.name);
  CHECK(again.ab.assignment == cfg.ab.assignment);
  CHECK(again.ab.block_s == cfg.ab.block_s);
  CHECK(again.ab.seed == cfg.ab.seed);
  CHECK(again.ab.weights == cfg.ab.weights);
  REQUIRE(again.ab.arms.size() == 2);
  CHECK(again.ab.arms[1].params == cfg.ab.arms[1].params);
  CHECK(again.effective_toml() == cfg.effective_toml());
  CHECK(cfg.redacted().find("[[ab.arms]]") != std::string::npos);
  // Without [ab] the effective text is what it was.
  CHECK(Config::parse(config("")).effective_toml().find("[ab") == std::string::npos);
}

TEST_CASE("core.ab: a malformed [ab] table is refused with its line") {
  const auto refused = [](std::string_view ab, std::string_view what) {
    CAPTURE(ab);
    try {
      static_cast<void>(parse(ab));
      FAIL("accepted");
    } catch (const ConfigError& e) {
      CHECK(std::string(e.what()).find(what) != std::string::npos);
    }
  };
  refused("[ab]\nassignment = \"time\"\nblock_s = 60\n", "missing required key 'ab.name'");
  refused("[ab]\nname = \"x\"\nassignment = \"random\"\n[[ab.arms]]\nname = \"a\"\n",
          "ab.assignment must be instrument, time, crossover or branch");
  refused("[ab]\nname = \"x\"\nassignment = \"time\"\n[[ab.arms]]\nname = \"a\"\n",
          "needs block_s");
  refused(
      "[ab]\nname = \"x\"\nassignment = \"instrument\"\nblock_s = 60\n[[ab.arms]]\nname = \"a\"\n",
      "has no blocks");
  refused("[ab]\nname = \"x\"\nassignment = \"time\"\nblock_s = 60\n[[ab.arms]]\nname = \"a\"\n",
          "needs 2 to 16 [[ab.arms]], got 1");
  refused(
      "[ab]\nname = \"x\"\nassignment = \"time\"\nblock_s = 60\n[[ab.arms]]\nname = "
      "\"a\"\n[[ab.arms]]\nname = \"a\"\n",
      "two arms are named 'a'");
  refused(
      "[ab]\nname = \"x\"\nassignment = \"time\"\nblock_s = 60\n[[ab.arms]]\nname = "
      "\"a\"\nparam = { levels = 2 }\n[[ab.arms]]\nname = \"b\"\n",
      "unknown key 'ab.arms[].param'");
  refused(
      "[ab]\nname = \"x y\"\nassignment = \"time\"\nblock_s = 60\n[[ab.arms]]\nname = "
      "\"a\"\n[[ab.arms]]\nname = \"b\"\n",
      "ab.name must be 1 to 32");
  refused(
      "[ab]\nname = \"x\"\nassignment = \"time\"\nblock_s = 60\nstart = \"2026-10-02\"\nend = "
      "\"2026-10-01\"\n[[ab.arms]]\nname = \"a\"\n[[ab.arms]]\nname = \"b\"\n",
      "ab.end must be after ab.start");
  refused(
      "[ab]\nname = \"x\"\nassignment = \"time\"\nblock_s = 60\nstart = "
      "\"soon\"\n[[ab.arms]]\nname "
      "= \"a\"\n[[ab.arms]]\nname = \"b\"\n",
      "is not a UTC time");
  refused(
      "[ab]\nname = \"x\"\nassignment = \"time\"\nblock_s = 60\n[ab.weights]\nAAA = 1\n[[ab.arms]]"
      "\nname = \"a\"\n[[ab.arms]]\nname = \"b\"\n",
      "puts every instrument in the same arm");
  refused(
      "[ab]\nname = \"x\"\nassignment = \"instrument\"\n[ab.weights]\nAAA = -1\n[[ab.arms]]\nname "
      "= \"a\"\n[[ab.arms]]\nname = \"b\"\n",
      "must be a number >= 0");
  std::string many = "[ab]\nname = \"x\"\nassignment = \"time\"\nblock_s = 60\n";
  for (int a = 0; a < 2; ++a) {
    many += "[[ab.arms]]\nname = \"a" + std::to_string(a) + "\"\n[ab.arms.params]\n";
    for (int p = 0; p < 17; ++p)
      many += "p" + std::to_string(a) + "_" + std::to_string(p) + " = 1\n";
  }
  refused(many, "the arms name 34 parameters; at most 32");
}

TEST_CASE("core.ab: the instrument split gives each stratum every arm, the same for a seed") {
  Config cfg = parse(kCrossover);
  cfg.ab.assignment = "instrument";
  cfg.ab.block_s = 0;
  const AbPlan plan = plan_of(cfg);
  CHECK(plan.assignment() == AbAssignment::Instrument);
  CHECK(plan.instrument_name(0) == "sim:AAA");
  // Weights 60..10 in instrument order: strata {AAA, BBB}, {CCC, DDD}, {EEE, FFF}.
  for (std::size_t s = 0; s < 3; ++s) {
    CHECK(plan.stratum(2 * s) == s);
    CHECK(plan.stratum(2 * s + 1) == s);
    CHECK(plan.arm_at(2 * s, 0) != plan.arm_at(2 * s + 1, 0));
  }
  for (std::size_t i = 0; i < plan.instruments(); ++i) {
    CHECK(plan.arm_at(i, 0) == plan.arm_at(i, std::int64_t{400'000} * kSec));
    CHECK(plan.arm_at(i, 0) == static_cast<int>(plan.slot(i)));
  }
  CHECK(plan.next_change(0) == std::numeric_limits<std::int64_t>::max());
  const AbPlan again = plan_of(cfg);
  for (std::size_t i = 0; i < plan.instruments(); ++i)
    CHECK(again.arm_at(i, 0) == plan.arm_at(i, 0));
  // Some seed among a few draws another split.
  bool differs = false;
  for (std::uint64_t seed = 1; seed < 8 && !differs; ++seed) {
    Config other = cfg;
    other.ab.seed = seed;
    const AbPlan p = plan_of(other);
    for (std::size_t i = 0; i < p.instruments(); ++i)
      differs = differs || p.arm_at(i, 0) != plan.arm_at(i, 0);
  }
  CHECK(differs);
}

TEST_CASE("core.ab: weights must name every instrument, and the split needs one per arm") {
  Config cfg = parse(kCrossover);
  cfg.ab.weights.erase("FFF");
  CHECK_THROWS_WITH_AS(plan_of(cfg), doctest::Contains("has no weight for sim:FFF"), ConfigError);
  cfg.ab.weights["ZZZ"] = 1;
  cfg.ab.weights["FFF"] = 1;
  CHECK_THROWS_WITH_AS(plan_of(cfg), doctest::Contains("no instrument 'ZZZ'"), ConfigError);
  cfg.ab.weights.erase("ZZZ");
  for (int a = 2; a < 8; ++a) cfg.ab.arms.push_back({"x" + std::to_string(a), {}});
  CHECK_THROWS_WITH_AS(
      plan_of(cfg), doctest::Contains("needs at least one instrument per arm"), ConfigError);
}

TEST_CASE("core.ab: time blocks give every arm one block per run, crossover each instrument too") {
  const Config cfg = parse(kCrossover);
  const AbPlan cross = plan_of(cfg);
  Config time_cfg = cfg;
  time_cfg.ab.assignment = "time";
  time_cfg.ab.weights.clear();
  const AbPlan time = plan_of(time_cfg);
  const std::int64_t block = 3600 * kSec;
  const std::int64_t t0 = std::int64_t{1'792'681'200} * kSec;  // a block boundary
  CHECK(time.block_of(t0) * block == t0);
  CHECK(time.next_change(t0) == t0 + block);
  CHECK(time.next_change(t0 + 1) == t0 + block);
  std::map<int, int> arms_seen;
  for (std::int64_t round = 0; round < 50; ++round) {
    const std::int64_t first = (time.round_of(t0) + round) * 2 * block;
    std::set<int> in_round;
    for (int k = 0; k < 2; ++k) {
      const std::int64_t t = first + k * block + block / 2;
      const int arm = time.arm_at(0, t);
      for (std::size_t i = 1; i < time.instruments(); ++i) CHECK(time.arm_at(i, t) == arm);
      in_round.insert(arm);
      ++arms_seen[k * 10 + arm];
      // Crossover: half the instruments in each arm, and each instrument in both over the run.
      int wide = 0;
      for (std::size_t i = 0; i < cross.instruments(); ++i) wide += cross.arm_at(i, t);
      CHECK(wide == 3);
    }
    CHECK(in_round == std::set<int>{0, 1});
    for (std::size_t i = 0; i < cross.instruments(); ++i) {
      CHECK(cross.arm_at(i, first) != cross.arm_at(i, first + block));
      CHECK(cross.round_of(first) == cross.round_of(first + block));
    }
  }
  // The order within a run is drawn: both orders occur.
  CHECK(arms_seen[0] > 0);
  CHECK(arms_seen[1] > 0);
}

TEST_CASE("core.ab: start and end bound the test") {
  Config cfg = parse(kCrossover);
  cfg.ab.start = "2026-10-22T15:30:00Z";
  cfg.ab.end = "2026-10-22T18:00";
  const AbPlan plan = plan_of(cfg);
  const std::int64_t start = *parse_utc_time(cfg.ab.start);
  const std::int64_t end = *parse_utc_time(cfg.ab.end);
  CHECK(plan.arm_at(0, start - 1) == AbPlan::kNoArm);
  CHECK(plan.arm_at(0, start) != AbPlan::kNoArm);
  CHECK(plan.arm_at(0, end - 1) != AbPlan::kNoArm);
  CHECK(plan.arm_at(0, end) == AbPlan::kNoArm);
  CHECK(plan.next_change(start - 10 * kSec) == start);
  CHECK(plan.next_change(start) == *parse_utc_time("2026-10-22T16:00"));
  CHECK(plan.next_change(end - 1) == end);
  CHECK(plan.next_change(end) == std::numeric_limits<std::int64_t>::max());
}

TEST_CASE("core.ab: the controller publishes each arm's parameters as instruments change arm") {
  Config cfg = parse(kCrossover);
  const AbPlan plan = plan_of(cfg);
  BasicMM strategy;
  REQUIRE_FALSE(strategy.configure(cfg.strategy.params).has_value());
  Captured out;
  ParamPublisher pub(out.sink(), strategy.params(), plan.instruments());
  AbController ab(plan, pub);
  CHECK(ab.owns("half_spread_bps"));
  CHECK(ab.owns("levels"));
  CHECK_FALSE(ab.owns("quote_qty"));
  CHECK(ab.arm("wide") == 1);
  CHECK(ab.arm("nope") == AbPlan::kNoArm);
  // The control arm carries the configured values of the parameters the arms name.
  REQUIRE(ab.values(0).size() == 2);
  CHECK(ab.values(0)[0] == ParamPublisher::ParamValue{"half_spread_bps", "5"});
  CHECK(ab.values(0)[1] == ParamPublisher::ParamValue{"levels", "1"});
  CHECK(ab.values(1)[0].second == "8");

  const Timestamp t0{std::int64_t{1'792'684'800} * kSec};  // the first block of a run of two
  REQUIRE(ab.poll(t0));
  // Crossover: one update per instrument, each with its arm and that arm's values.
  REQUIRE(out.sent.size() == plan.instruments());
  for (std::size_t i = 0; i < out.sent.size(); ++i) {
    const ParamUpdateMsg& m = out.sent[i];
    CHECK(m.hdr.instrument.value == i);
    CHECK(m.origin == ParamUpdateMsg::Origin::Experiment);
    CHECK(m.source_view() == "ab:spread-1");
    const int arm = plan.arm_at(i, t0.ns);
    CHECK(m.arm == arm + 1);
    CHECK(field_value(m, "half_spread_bps") == (arm == 1 ? 80'000 : 50'000));
    CHECK(field_value(m, "levels") == (arm == 1 ? 2 : 1));
    CHECK(ab.current(i) == arm);
  }
  CHECK(pub.describe(InstrumentId{0}).find(plan.arm_at(0, t0.ns) == 1 ? "levels=2" : "levels=1") !=
        std::string::npos);
  // Nothing moves within the block.
  out.sent.clear();
  REQUIRE(ab.poll(Timestamp{t0.ns + 100 * kSec}));
  CHECK(out.sent.empty());
  CHECK(ab.next_change(t0) == Timestamp{t0.ns + 3600 * kSec});
  // The next block moves every instrument; a refused update leaves the rest for the next poll.
  out.refuse = true;
  CHECK_FALSE(ab.poll(Timestamp{t0.ns + 3600 * kSec}));
  out.refuse = false;
  REQUIRE(ab.poll(Timestamp{t0.ns + 3600 * kSec}));
  CHECK(out.sent.size() == plan.instruments());
  CHECK(ab.state(t0) == AbController::State::Running);
  const std::string status = ab.status(Timestamp{t0.ns + 3700 * kSec});
  CHECK(
      status.starts_with("spread-1: running, assignment crossover, 2 arms, seed 7, block 3600 s"));
  CHECK(status.find("arm        1 wide: half_spread_bps=8 levels=2; 3 instrument(s) now") !=
        std::string::npos);
  CHECK(status.find("block      ") != std::string::npos);
  CHECK(status.find("instrument sim:AAA ") != std::string::npos);

  // Promote: every instrument on the arm's values, out of the test, once.
  out.sent.clear();
  ab.stop(1);
  REQUIRE(ab.poll(Timestamp{t0.ns + 3800 * kSec}));
  REQUIRE(out.sent.size() == 1);
  CHECK(out.sent[0].all_instruments());
  CHECK(out.sent[0].arm == ParamUpdateMsg::kArmNone);
  CHECK(field_value(out.sent[0], "half_spread_bps") == 80'000);
  CHECK(ab.state(t0) == AbController::State::Ended);
  CHECK(ab.next_change(t0) == Timestamp::max());
  REQUIRE(ab.poll(Timestamp{t0.ns + 9000 * kSec}));
  CHECK(out.sent.size() == 1);
  CHECK(ab.status(t0).find("stopped    every instrument on arm wide's parameters (promoted)") !=
        std::string::npos);
}

TEST_CASE("core.ab: a time test publishes one update for every instrument, stop the configured") {
  Config cfg = parse(kCrossover);
  cfg.ab.assignment = "time";
  cfg.ab.weights.clear();
  const AbPlan plan = plan_of(cfg);
  BasicMMParams configured;
  REQUIRE_FALSE(configured.apply(cfg.strategy.params).has_value());
  Captured out;
  // A strategy with one parameter set takes a time test.
  ParamPublisher pub(out.sink(), configured);
  AbController ab(plan, pub);
  REQUIRE(ab.poll(Timestamp{std::int64_t{1'792'681'200} * kSec}));
  REQUIRE(out.sent.size() == 1);
  CHECK(out.sent[0].all_instruments());
  ab.stop();
  REQUIRE(ab.poll(Timestamp{std::int64_t{1'792'681'201} * kSec}));
  REQUIRE(out.sent.size() == 2);
  CHECK(out.sent[1].arm == ParamUpdateMsg::kArmNone);
  CHECK(field_value(out.sent[1], "half_spread_bps") == 50'000);
}

TEST_CASE("core.ab: the controller refuses arms the strategy cannot run") {
  Config cfg = parse(kCrossover);
  const AbPlan plan = plan_of(cfg);
  BasicMMParams configured;
  REQUIRE_FALSE(configured.apply(cfg.strategy.params).has_value());
  Captured out;
  ParamPublisher shared(out.sink(), configured);
  CHECK_THROWS_WITH_AS(AbController(plan, shared),
                       doctest::Contains("keeps one set for all of them"),
                       std::invalid_argument);
  ParamPublisher per(out.sink(), configured, plan.instruments());
  Config unknown = cfg;
  unknown.ab.arms[1].params["spread"] = "3";
  CHECK_THROWS_WITH_AS(AbController(plan_of(unknown), per),
                       "[ab] arm 'wide': unknown parameter 'spread'",
                       std::invalid_argument);
  Config range = cfg;
  range.ab.arms[1].params["levels"] = "9";
  CHECK_THROWS_WITH_AS(AbController(plan_of(range), per),
                       "[ab] arm 'wide': parameter 'levels': value 9 outside [1, 8]",
                       std::invalid_argument);
  CHECK(out.sent.empty());
}
