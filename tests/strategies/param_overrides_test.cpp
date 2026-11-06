// Per-instrument parameter values (strategies/param_overrides.hpp): an instrument's own value
// outlives an update for every instrument until a reset, the publisher's copy follows the same
// rule, and [strategy.groups] / [strategy.overrides] resolve to one list per instrument.
#include "fastmm/strategies/param_overrides.hpp"

#include "test_support.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/strategies/param_publisher.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <fmt/format.h>

#include <string>
#include <vector>

using namespace fastmm;

namespace {

struct OvParams {
  FASTMM_PARAMS(OvParams)
  FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels")
  FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 100_bps, "half spread")
};

struct PerInstrument : StrategyBase<OvParams, ParamScope::PerInstrument> {
  static constexpr std::string_view name() noexcept { return "per_instrument"; }
};

// A publisher whose updates the strategy applies at once.
struct Wired {
  PerInstrument s;
  std::vector<ParamUpdateMsg> sent;
  std::unique_ptr<ParamPublisher> pub;
  explicit Wired(std::size_t instruments) {
    REQUIRE_FALSE(s.configure({}).has_value());
    pub = std::make_unique<ParamPublisher>(ParamSink{this,
                                                     [](void* c, const ParamUpdateMsg& m) noexcept {
                                                       auto* w = static_cast<Wired*>(c);
                                                       w->sent.push_back(m);
                                                       w->s.apply_param_update(m);
                                                       return true;
                                                     }},
                                           s.params(),
                                           instruments);
  }
};

constexpr InstrumentId kA{0};
constexpr InstrumentId kB{1};

std::string config(std::string_view extra) {
  std::string out = "[venues.x]\nkind = \"sim\"\n[venues.y]\nkind = \"sim\"\n";
  for (const char* v : {"x", "y"}) {
    for (const char* sym : {"AAA", "BBB"}) {
      if (std::string_view(v) == "y" && std::string_view(sym) == "BBB") continue;
      out += fmt::format(
          "\n[[instruments]]\nvenue = \"{}\"\nsymbol = \"{}\"\ntick = \"0.01\"\n"
          "lot = \"0.001\"\n",
          v,
          sym);
    }
  }
  out += "\n[strategy]\nname = \"basic_mm\"\n";
  out += extra;
  return out;
}

std::vector<InstrumentParams> resolve(const Config& cfg) {
  const InstrumentTable table = load_instruments(cfg);
  std::vector<std::string> venues;
  for (const VenueSection& v : cfg.venues) venues.push_back(v.name);
  return resolve_overrides(cfg.strategy.groups, cfg.strategy.overrides, table, venues);
}

}  // namespace

TEST_CASE("strategies.overrides: an instrument's own value outlives updates for every instrument") {
  Wired w(3);
  REQUIRE(w.pub->publish({{"half_spread_bps", "9"}}, kA));
  CHECK(w.s.own_params(kA).count() == 1);
  CHECK(w.s.own_params(kB).none());

  // The shared value moves; instrument A keeps its own, the others follow.
  REQUIRE(w.pub->publish({{"half_spread_bps", "7"}, {"levels", "3"}}));
  CHECK(w.s.params().half_spread_bps == 7_bps);
  CHECK(w.s.params(kA).half_spread_bps == 9_bps);
  CHECK(w.s.params(kA).levels == 3);
  CHECK(w.s.params(kB).half_spread_bps == 7_bps);
  CHECK(w.pub->describe(kA).find("half_spread_bps=9") != std::string::npos);
  CHECK(w.pub->own(kA) == std::vector<std::string>{"half_spread_bps"});

  // A reset of one instrument gives it the shared value back.
  REQUIRE(w.pub->reset({"half_spread_bps"}, kA, ParamUpdateMsg::Origin::Control, "manual"));
  CHECK((w.sent.back().flags & ParamUpdateMsg::kReset) != 0);
  CHECK(w.s.params(kA).half_spread_bps == 7_bps);
  CHECK(w.s.own_params(kA).none());
  CHECK(w.pub->own(kA).empty());

  // A reset for every instrument drops each one's own value of the parameter.
  REQUIRE(w.pub->publish({{"levels", "5"}}, kA));
  REQUIRE(w.pub->publish({{"levels", "6"}}, kB));
  REQUIRE(w.pub->reset(
      {"levels"}, ParamPublisher::kAllInstruments, ParamUpdateMsg::Origin::Control, ""));
  CHECK(w.s.params(kA).levels == 3);
  CHECK(w.s.params(kB).levels == 3);
  CHECK(w.pub->own(kB).empty());
}

TEST_CASE("strategies.overrides: an [ab] update for every instrument is a reset") {
  Wired w(2);
  REQUIRE(w.pub->publish({{"levels", "4"}}, kA));
  REQUIRE(w.pub->publish_as({{"levels", "2"}},
                            ParamPublisher::kAllInstruments,
                            ParamUpdateMsg::Origin::Experiment,
                            "ab:x",
                            1));
  CHECK(w.s.params(kA).levels == 2);
  CHECK(w.s.own_params(kA).none());
}

TEST_CASE("strategies.overrides: the publisher checks every instrument an update reaches") {
  struct Checked {
    FASTMM_PARAMS(Checked)
    FASTMM_PARAM(int, lo, 1, 0, 10, "low")
    FASTMM_PARAM(int, hi, 5, 0, 10, "high")
    [[nodiscard]] std::optional<std::string> validate() const {
      if (lo > hi) return std::string("lo must not exceed hi");
      return std::nullopt;
    }
  };
  const Checked c{};
  ParamPublisher pub(
      ParamSink{nullptr, [](void*, const ParamUpdateMsg&) noexcept { return true; }}, c, 2);
  REQUIRE(pub.publish({{"hi", "2"}}, kA));
  // Instrument A keeps hi = 2, so lo = 3 fails there.
  ParamUpdateMsg m{};
  const auto err = pub.build({{"lo", "3"}}, ParamPublisher::kAllInstruments, m);
  REQUIRE(err.has_value());
  CHECK(err->find("instrument 0") != std::string::npos);
  CHECK_FALSE(pub.build({{"lo", "3"}}, kB, m).has_value());
}

TEST_CASE("strategies.overrides: groups and instruments resolve, an instrument's own key last") {
  const Config cfg = Config::parse(config(R"(
[strategy.groups]
majors = ["x:AAA", "BBB"]
[strategy.overrides.majors]
half_spread_bps = 6
levels = 2
[strategy.overrides."x:AAA"]
levels = 3
)"));
  const std::vector<InstrumentParams> r = resolve(cfg);
  REQUIRE(r.size() == 2);
  CHECK(r[0].instrument == InstrumentId{0});
  CHECK(r[0].values == std::vector<std::pair<std::string, std::string>>{{"half_spread_bps", "6"},
                                                                        {"levels", "3"}});
  CHECK(r[1].instrument == InstrumentId{1});
  CHECK(r[1].values.size() == 2);

  // The effective configuration carries them and parses back to the same.
  const Config again = Config::parse(cfg.effective_toml());
  CHECK(again.strategy.groups == cfg.strategy.groups);
  CHECK(again.strategy.overrides == cfg.strategy.overrides);
}

TEST_CASE(
    "strategies.overrides: a key that names nothing, or two groups that disagree, are refused") {
  CHECK_THROWS_WITH_AS(resolve(Config::parse(config("[strategy.overrides.ZZZ]\nlevels = 2\n"))),
                       doctest::Contains("no instrument or group"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(resolve(Config::parse(config("[strategy.overrides.AAA]\nlevels = 2\n"))),
                       doctest::Contains("more than one venue"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(resolve(Config::parse(config(R"(
[strategy.groups]
one = ["x:AAA"]
two = ["x:AAA", "BBB"]
[strategy.overrides.one]
levels = 2
[strategy.overrides.two]
levels = 3
)"))),
                       doctest::Contains("different values of levels"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(resolve(Config::parse(config("[strategy.groups]\nBBB = [\"x:AAA\"]\n"))),
                       doctest::Contains("an instrument has that symbol"),
                       ConfigError);
  CHECK_THROWS_WITH_AS(resolve(Config::parse(config("[strategy.groups]\ng = [\"QQQ\"]\n"))),
                       doctest::Contains("no instrument 'QQQ'"),
                       ConfigError);
  CHECK_THROWS_AS(Config::parse(config("[strategy.overrides]\nlevels = 2\n")), ConfigError);
}
