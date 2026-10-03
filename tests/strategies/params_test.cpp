#include "fastmm/strategies/params.hpp"

#include "test_support.hpp"

#include "fastmm/strategies/strategy.hpp"

#include <sstream>

using namespace fastmm;
using namespace fastmm::literals;

namespace {
struct P {
  FASTMM_PARAMS(P)
  FASTMM_PARAM(double, gamma, 0.1, 0.0, 10.0, "risk aversion")
  FASTMM_PARAM(int, levels, 1, 1, 8, "levels")
  FASTMM_PARAM(bool, flag, false, 0, 1, "a flag")
};

struct Typed {
  FASTMM_PARAMS(Typed)
  FASTMM_PARAM(Qty, quote_qty, 0.01_qty, 0_qty, 1000_qty, "quantity")
  FASTMM_PARAM(Price, offset, Price{}, -5_px, 5_px, "price offset")
  FASTMM_PARAM(Notional, budget, Notional::from_int(1000), Notional{}, Notional::max(), "budget")
  FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 1000_bps, "half spread")
  FASTMM_PARAM_MS(stale_ms, milliseconds(2000), milliseconds(0), milliseconds(60000), "stale")
  FASTMM_PARAM(std::int64_t, big, 0, -1000, 1'000'000'000'000, "a 64-bit integer")
};

struct Validated {
  FASTMM_PARAMS(Validated)
  FASTMM_PARAM(Qty, quote_qty, 0.01_qty, 0_qty, 1000_qty, "quantity per side")
  FASTMM_PARAM(Qty, max_inventory, 0.1_qty, 0_qty, 1000_qty, "inventory cap (0 = none)")
  [[nodiscard]] std::optional<std::string> validate() const {
    if (max_inventory.is_positive() && quote_qty > max_inventory)
      return "quote_qty must not exceed max_inventory";
    return std::nullopt;
  }
};
class ValidatedStrategy : public StrategyBase<Validated> {};

struct SixtyFour {
  FASTMM_PARAMS(SixtyFour)
  FASTMM_PARAM(int, p00, 0, 0, 100, "p0")
  FASTMM_PARAM(int, p01, 0, 0, 100, "p1")
  FASTMM_PARAM(int, p02, 0, 0, 100, "p2")
  FASTMM_PARAM(int, p03, 0, 0, 100, "p3")
  FASTMM_PARAM(int, p04, 0, 0, 100, "p4")
  FASTMM_PARAM(int, p05, 0, 0, 100, "p5")
  FASTMM_PARAM(int, p06, 0, 0, 100, "p6")
  FASTMM_PARAM(int, p07, 0, 0, 100, "p7")
  FASTMM_PARAM(int, p08, 0, 0, 100, "p8")
  FASTMM_PARAM(int, p09, 0, 0, 100, "p9")
  FASTMM_PARAM(int, p10, 0, 0, 100, "p10")
  FASTMM_PARAM(int, p11, 0, 0, 100, "p11")
  FASTMM_PARAM(int, p12, 0, 0, 100, "p12")
  FASTMM_PARAM(int, p13, 0, 0, 100, "p13")
  FASTMM_PARAM(int, p14, 0, 0, 100, "p14")
  FASTMM_PARAM(int, p15, 0, 0, 100, "p15")
  FASTMM_PARAM(int, p16, 0, 0, 100, "p16")
  FASTMM_PARAM(int, p17, 0, 0, 100, "p17")
  FASTMM_PARAM(int, p18, 0, 0, 100, "p18")
  FASTMM_PARAM(int, p19, 0, 0, 100, "p19")
  FASTMM_PARAM(int, p20, 0, 0, 100, "p20")
  FASTMM_PARAM(int, p21, 0, 0, 100, "p21")
  FASTMM_PARAM(int, p22, 0, 0, 100, "p22")
  FASTMM_PARAM(int, p23, 0, 0, 100, "p23")
  FASTMM_PARAM(int, p24, 0, 0, 100, "p24")
  FASTMM_PARAM(int, p25, 0, 0, 100, "p25")
  FASTMM_PARAM(int, p26, 0, 0, 100, "p26")
  FASTMM_PARAM(int, p27, 0, 0, 100, "p27")
  FASTMM_PARAM(int, p28, 0, 0, 100, "p28")
  FASTMM_PARAM(int, p29, 0, 0, 100, "p29")
  FASTMM_PARAM(int, p30, 0, 0, 100, "p30")
  FASTMM_PARAM(int, p31, 0, 0, 100, "p31")
  FASTMM_PARAM(int, p32, 0, 0, 100, "p32")
  FASTMM_PARAM(int, p33, 0, 0, 100, "p33")
  FASTMM_PARAM(int, p34, 0, 0, 100, "p34")
  FASTMM_PARAM(int, p35, 0, 0, 100, "p35")
  FASTMM_PARAM(int, p36, 0, 0, 100, "p36")
  FASTMM_PARAM(int, p37, 0, 0, 100, "p37")
  FASTMM_PARAM(int, p38, 0, 0, 100, "p38")
  FASTMM_PARAM(int, p39, 0, 0, 100, "p39")
  FASTMM_PARAM(int, p40, 0, 0, 100, "p40")
  FASTMM_PARAM(int, p41, 0, 0, 100, "p41")
  FASTMM_PARAM(int, p42, 0, 0, 100, "p42")
  FASTMM_PARAM(int, p43, 0, 0, 100, "p43")
  FASTMM_PARAM(int, p44, 0, 0, 100, "p44")
  FASTMM_PARAM(int, p45, 0, 0, 100, "p45")
  FASTMM_PARAM(int, p46, 0, 0, 100, "p46")
  FASTMM_PARAM(int, p47, 0, 0, 100, "p47")
  FASTMM_PARAM(int, p48, 0, 0, 100, "p48")
  FASTMM_PARAM(int, p49, 0, 0, 100, "p49")
  FASTMM_PARAM(int, p50, 0, 0, 100, "p50")
  FASTMM_PARAM(int, p51, 0, 0, 100, "p51")
  FASTMM_PARAM(int, p52, 0, 0, 100, "p52")
  FASTMM_PARAM(int, p53, 0, 0, 100, "p53")
  FASTMM_PARAM(int, p54, 0, 0, 100, "p54")
  FASTMM_PARAM(int, p55, 0, 0, 100, "p55")
  FASTMM_PARAM(int, p56, 0, 0, 100, "p56")
  FASTMM_PARAM(int, p57, 0, 0, 100, "p57")
  FASTMM_PARAM(int, p58, 0, 0, 100, "p58")
  FASTMM_PARAM(int, p59, 0, 0, 100, "p59")
  FASTMM_PARAM(int, p60, 0, 0, 100, "p60")
  FASTMM_PARAM(int, p61, 0, 0, 100, "p61")
  FASTMM_PARAM(int, p62, 0, 0, 100, "p62")
  FASTMM_PARAM(int, p63, 0, 0, 100, "p63")
};
struct SixtyFive {
  FASTMM_PARAMS(SixtyFive)
  FASTMM_PARAM(int, p00, 0, 0, 100, "p0")
  FASTMM_PARAM(int, p01, 0, 0, 100, "p1")
  FASTMM_PARAM(int, p02, 0, 0, 100, "p2")
  FASTMM_PARAM(int, p03, 0, 0, 100, "p3")
  FASTMM_PARAM(int, p04, 0, 0, 100, "p4")
  FASTMM_PARAM(int, p05, 0, 0, 100, "p5")
  FASTMM_PARAM(int, p06, 0, 0, 100, "p6")
  FASTMM_PARAM(int, p07, 0, 0, 100, "p7")
  FASTMM_PARAM(int, p08, 0, 0, 100, "p8")
  FASTMM_PARAM(int, p09, 0, 0, 100, "p9")
  FASTMM_PARAM(int, p10, 0, 0, 100, "p10")
  FASTMM_PARAM(int, p11, 0, 0, 100, "p11")
  FASTMM_PARAM(int, p12, 0, 0, 100, "p12")
  FASTMM_PARAM(int, p13, 0, 0, 100, "p13")
  FASTMM_PARAM(int, p14, 0, 0, 100, "p14")
  FASTMM_PARAM(int, p15, 0, 0, 100, "p15")
  FASTMM_PARAM(int, p16, 0, 0, 100, "p16")
  FASTMM_PARAM(int, p17, 0, 0, 100, "p17")
  FASTMM_PARAM(int, p18, 0, 0, 100, "p18")
  FASTMM_PARAM(int, p19, 0, 0, 100, "p19")
  FASTMM_PARAM(int, p20, 0, 0, 100, "p20")
  FASTMM_PARAM(int, p21, 0, 0, 100, "p21")
  FASTMM_PARAM(int, p22, 0, 0, 100, "p22")
  FASTMM_PARAM(int, p23, 0, 0, 100, "p23")
  FASTMM_PARAM(int, p24, 0, 0, 100, "p24")
  FASTMM_PARAM(int, p25, 0, 0, 100, "p25")
  FASTMM_PARAM(int, p26, 0, 0, 100, "p26")
  FASTMM_PARAM(int, p27, 0, 0, 100, "p27")
  FASTMM_PARAM(int, p28, 0, 0, 100, "p28")
  FASTMM_PARAM(int, p29, 0, 0, 100, "p29")
  FASTMM_PARAM(int, p30, 0, 0, 100, "p30")
  FASTMM_PARAM(int, p31, 0, 0, 100, "p31")
  FASTMM_PARAM(int, p32, 0, 0, 100, "p32")
  FASTMM_PARAM(int, p33, 0, 0, 100, "p33")
  FASTMM_PARAM(int, p34, 0, 0, 100, "p34")
  FASTMM_PARAM(int, p35, 0, 0, 100, "p35")
  FASTMM_PARAM(int, p36, 0, 0, 100, "p36")
  FASTMM_PARAM(int, p37, 0, 0, 100, "p37")
  FASTMM_PARAM(int, p38, 0, 0, 100, "p38")
  FASTMM_PARAM(int, p39, 0, 0, 100, "p39")
  FASTMM_PARAM(int, p40, 0, 0, 100, "p40")
  FASTMM_PARAM(int, p41, 0, 0, 100, "p41")
  FASTMM_PARAM(int, p42, 0, 0, 100, "p42")
  FASTMM_PARAM(int, p43, 0, 0, 100, "p43")
  FASTMM_PARAM(int, p44, 0, 0, 100, "p44")
  FASTMM_PARAM(int, p45, 0, 0, 100, "p45")
  FASTMM_PARAM(int, p46, 0, 0, 100, "p46")
  FASTMM_PARAM(int, p47, 0, 0, 100, "p47")
  FASTMM_PARAM(int, p48, 0, 0, 100, "p48")
  FASTMM_PARAM(int, p49, 0, 0, 100, "p49")
  FASTMM_PARAM(int, p50, 0, 0, 100, "p50")
  FASTMM_PARAM(int, p51, 0, 0, 100, "p51")
  FASTMM_PARAM(int, p52, 0, 0, 100, "p52")
  FASTMM_PARAM(int, p53, 0, 0, 100, "p53")
  FASTMM_PARAM(int, p54, 0, 0, 100, "p54")
  FASTMM_PARAM(int, p55, 0, 0, 100, "p55")
  FASTMM_PARAM(int, p56, 0, 0, 100, "p56")
  FASTMM_PARAM(int, p57, 0, 0, 100, "p57")
  FASTMM_PARAM(int, p58, 0, 0, 100, "p58")
  FASTMM_PARAM(int, p59, 0, 0, 100, "p59")
  FASTMM_PARAM(int, p60, 0, 0, 100, "p60")
  FASTMM_PARAM(int, p61, 0, 0, 100, "p61")
  FASTMM_PARAM(int, p62, 0, 0, 100, "p62")
  FASTMM_PARAM(int, p63, 0, 0, 100, "p63")
  FASTMM_PARAM(int, p64, 0, 0, 100, "p64")
};

ParamMap parse_describe(const std::string& s) {
  ParamMap m;
  std::istringstream in(s);
  std::string kv;
  while (in >> kv) {
    const auto eq = kv.find('=');
    REQUIRE(eq != std::string::npos);
    m[kv.substr(0, eq)] = kv.substr(eq + 1);
  }
  return m;
}
}  // namespace

// Notional has no literal: the default is built with from_int.
static_assert(std::is_same_v<decltype(Typed{}.budget), Notional>);

TEST_CASE("strategies.params: schema collection, apply, describe") {
  const ParamSchema& s = P::schema();
  REQUIRE(s.size() == 3);
  const ParamDesc* gamma = s.find("gamma");
  const ParamDesc* levels = s.find("levels");
  const ParamDesc* flag = s.find("flag");
  REQUIRE(gamma != nullptr);
  REQUIRE(levels != nullptr);
  REQUIRE(flag != nullptr);
  CHECK(std::string(gamma->doc) == "risk aversion");
  CHECK(gamma->type == ParamType::Double);
  CHECK(levels->type == ParamType::Int);
  CHECK(flag->type == ParamType::Bool);
  CHECK(s.find("nope") == nullptr);
  P p;
  CHECK(p.gamma == doctest::Approx(0.1));
  CHECK_FALSE(p.apply({{"gamma", "0.5"}, {"levels", "3"}, {"flag", "true"}}));
  CHECK(p.gamma == doctest::Approx(0.5));
  CHECK(p.levels == 3);
  CHECK(p.flag);
  CHECK(p.apply({{"gamma", "11"}}).value().find("outside [0, 10]") != std::string::npos);
  CHECK(p.apply({{"levels", "2.5"}}).value().find("cannot parse") != std::string::npos);
  CHECK(p.apply({{"flag", "maybe"}}).value().find("cannot parse") != std::string::npos);
  CHECK(p.apply({{"gamma", "nan"}}).value().find("cannot parse") != std::string::npos);
  CHECK(p.apply({{"unknown", "1"}}).value().find("unknown parameter") != std::string::npos);
  CHECK(p.gamma == doctest::Approx(0.5));  // failed values leave the field unchanged
  CHECK(p.describe() == "gamma=0.5 levels=3 flag=true");
  CHECK(gamma->format(&p) == "0.5");
  // Integers accept the forms TOML and Python produce for whole numbers.
  CHECK_FALSE(p.apply({{"levels", "4.0"}}));
  CHECK(p.levels == 4);
  CHECK_FALSE(p.apply({{"levels", "8e0"}}));
  CHECK(p.levels == 8);
  CHECK_FALSE(p.apply({{"gamma", "2e-05"}}));
  CHECK(p.gamma == doctest::Approx(2e-05));
  static_assert(sizeof(P) ==
                sizeof(double) + sizeof(int) + sizeof(bool) + 3);  // registrars take no space
}

TEST_CASE("strategies.params: kMaxParams fit the schema, one more is refused") {
  CHECK(SixtyFour::schema().size() == kMaxParams);
  CHECK(SixtyFour::schema().find("p63") != nullptr);
  // The 65th would otherwise be dropped and stay at its default whatever the configuration says.
  CHECK_THROWS_WITH_AS(static_cast<void>(SixtyFive::schema()),
                       doctest::Contains("at most 64 parameters"),
                       std::invalid_argument);
}

TEST_CASE("strategies.params: Price, Qty, Notional, bps and ms fields parse exactly") {
  const ParamSchema& s = Typed::schema();
  REQUIRE(s.size() == 6);
  CHECK(s.find("quote_qty")->type == ParamType::Decimal);
  CHECK(s.find("offset")->type == ParamType::Decimal);
  CHECK(s.find("budget")->type == ParamType::Decimal);
  CHECK(s.find("half_spread_bps")->type == ParamType::Bps);
  CHECK(s.find("stale_ms")->type == ParamType::Millis);
  CHECK(s.find("big")->type == ParamType::Int);
  CHECK(to_string(ParamType::Decimal) == "decimal");
  CHECK(to_string(ParamType::Bps) == "bps");
  CHECK(to_string(ParamType::Millis) == "ms");
  // Display doubles.
  CHECK(s.find("quote_qty")->def == doctest::Approx(0.01));
  CHECK(s.find("quote_qty")->max == doctest::Approx(1000.0));
  CHECK(s.find("offset")->min == doctest::Approx(-5.0));
  CHECK(s.find("half_spread_bps")->def == doctest::Approx(5.0));
  CHECK(s.find("half_spread_bps")->max == doctest::Approx(1000.0));
  CHECK(s.find("stale_ms")->def == doctest::Approx(2000.0));

  Typed t;
  CHECK(t.budget == Notional::from_int(1000));
  CHECK_FALSE(t.apply({{"quote_qty", "0.1"},
                       {"offset", "-0.25"},
                       {"budget", "12345.67891234"},
                       {"half_spread_bps", "0.25"},
                       {"stale_ms", "1500"},
                       {"big", "1e12"}}));
  CHECK(t.quote_qty.raw == 10'000'000);
  CHECK(t.offset.raw == -25'000'000);
  CHECK(t.budget.raw == 1'234'567'891'234);
  CHECK(t.half_spread_bps.raw == 2'500);
  CHECK(t.stale_ms == milliseconds(1500));
  CHECK(t.big == 1'000'000'000'000);

  SUBCASE("exponent notation") {
    CHECK_FALSE(t.apply({{"quote_qty", "2e-05"}}));  // how fmt formats a TOML 0.00002
    CHECK(t.quote_qty.raw == 2'000);
    CHECK_FALSE(t.apply({{"quote_qty", "1e-08"}}));
    CHECK(t.quote_qty.raw == 1);
    CHECK_FALSE(t.apply({{"half_spread_bps", "1.5e-3"}}));
    CHECK(t.half_spread_bps.raw == 15);
    CHECK_FALSE(t.apply({{"half_spread_bps", "2E2"}}));
    CHECK(t.half_spread_bps == 200_bps);
    CHECK_FALSE(t.apply({{"stale_ms", "2e3"}}));
    CHECK(t.stale_ms == milliseconds(2000));
  }
  SUBCASE("too many decimals") {
    const auto e = t.apply({{"quote_qty", "0.000000001"}});
    REQUIRE(e.has_value());
    CHECK(*e ==
          "parameter 'quote_qty': cannot parse '0.000000001' as decimal (at most 8 decimals)");
    CHECK(t.apply({{"quote_qty", "1.5e-8"}}).has_value());
    const auto b = t.apply({{"half_spread_bps", "0.00001"}});
    REQUIRE(b.has_value());
    CHECK(b->find("as bps (basis points, at most 4 decimals)") != std::string::npos);
    CHECK(t.apply({{"half_spread_bps", "2e-05"}}).has_value());
    CHECK(t.apply({{"stale_ms", "2.5"}}).value().find("as ms (whole milliseconds)") !=
          std::string::npos);
    CHECK(t.apply({{"big", "1.5"}}).has_value());
    CHECK(t.quote_qty.raw == 10'000'000);  // unchanged
  }
  SUBCASE("typed range checks") {
    CHECK(t.apply({{"quote_qty", "1000.00000001"}}).value() ==
          "parameter 'quote_qty': value 1000.00000001 outside [0, 1000]");
    CHECK_FALSE(t.apply({{"quote_qty", "1000"}}));
    CHECK(t.apply({{"quote_qty", "-0.00000001"}}).has_value());
    CHECK(t.apply({{"offset", "-5.00000001"}}).value().find("outside [-5, 5]") !=
          std::string::npos);
    CHECK(t.apply({{"half_spread_bps", "1000.0001"}}).value() ==
          "parameter 'half_spread_bps': value 1000.0001 outside [0, 1000]");
    CHECK(t.apply({{"half_spread_bps", "-0.0001"}}).has_value());
    CHECK(t.apply({{"stale_ms", "60001"}}).value() ==
          "parameter 'stale_ms': value 60001 outside [0, 60000]");
    CHECK(t.apply({{"stale_ms", "-1"}}).has_value());
    CHECK(t.apply({{"big", "-1001"}}).has_value());
    CHECK(t.apply({{"big", "1e19"}}).has_value());  // does not fit int64
    CHECK(t.apply({{"budget", "92233720368.54775808"}}).has_value());
  }
  SUBCASE("describe round trip") {
    const std::string d = t.describe();
    CHECK(d ==
          "quote_qty=0.1 offset=-0.25 budget=12345.67891234 half_spread_bps=0.25 stale_ms=1500 "
          "big=1000000000000");
    Typed u;
    CHECK_FALSE(u.apply(parse_describe(d)));
    CHECK(u.describe() == d);
    CHECK(u.quote_qty == t.quote_qty);
    CHECK(u.half_spread_bps == t.half_spread_bps);
    CHECK(u.stale_ms == t.stale_ms);
    // Every ParamDesc::format output parses back to the same value.
    for (const ParamDesc& desc : Typed::schema()) {
      CAPTURE(desc.name);
      Typed w;
      CHECK_FALSE(desc.parse(&w, desc.format(&t)));
      CHECK(desc.format(&w) == desc.format(&t));
    }
  }
}

TEST_CASE("strategies.params: describe round trip of the default struct and doubles") {
  P p;
  CHECK_FALSE(p.apply({{"gamma", "0.30000000000000004"}}));
  P q;
  CHECK_FALSE(q.apply(parse_describe(p.describe())));
  CHECK(q.gamma == p.gamma);  // shortest round-trip form keeps the exact double
  Typed defaults;
  CHECK(defaults.describe() ==
        "quote_qty=0.01 offset=0 budget=1000 half_spread_bps=5 stale_ms=2000 big=0");
}

TEST_CASE("strategies.params: validate() runs after all keys and keeps the old values on error") {
  ValidatedStrategy s;
  CHECK_FALSE(s.configure({{"quote_qty", "0.05"}, {"max_inventory", "0.2"}}));
  CHECK(s.params().quote_qty == 0.05_qty);
  // Applied one key at a time this would fail at quote_qty; validate() sees the final values.
  CHECK_FALSE(s.configure({{"quote_qty", "0.5"}, {"max_inventory", "1"}}));
  CHECK(s.params().quote_qty == 0.5_qty);
  CHECK(s.params().max_inventory == 1_qty);

  const auto err = s.configure({{"quote_qty", "2"}});
  REQUIRE(err.has_value());
  CHECK(*err == "quote_qty must not exceed max_inventory");
  CHECK(s.params().quote_qty == 0.5_qty);  // unchanged

  // A parse error in one key leaves every other key of the call unapplied too.
  CHECK(s.configure({{"max_inventory", "5"}, {"quote_qty", "x"}}).has_value());
  CHECK(s.params().max_inventory == 1_qty);

  CHECK_FALSE(s.configure({{"max_inventory", "0"}, {"quote_qty", "2"}}));  // 0 = no cap
  CHECK(s.describe_params() == "quote_qty=2 max_inventory=0");
}
