// ParamScope (strategies/strategy.hpp): a StrategyBase that keeps a parameter set per instrument
// takes updates naming one instrument, and its registered publisher accepts them.
#include "test_support.hpp"

#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/builtin.hpp"
#include "fastmm/strategies/param_publisher.hpp"
#include "fastmm/strategies/registry.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;

namespace {

struct ScopeParams {
  FASTMM_PARAMS(ScopeParams)
  FASTMM_PARAM(int, levels, 1, 1, 8, "quote levels")
  FASTMM_PARAM_BPS(half_spread_bps, 5_bps, 0_bps, 100_bps, "half spread")
};

struct PerInstrument : StrategyBase<ScopeParams, ParamScope::PerInstrument> {
  static constexpr std::string_view name() noexcept { return "per_instrument"; }
};
struct Shared : StrategyBase<ScopeParams> {
  static constexpr std::string_view name() noexcept { return "shared"; }
};

// The update `pub` would send for `values`.
ParamUpdateMsg update(const ParamPublisher& pub,
                      const std::vector<ParamPublisher::ParamValue>& values,
                      InstrumentId inst = ParamPublisher::kAllInstruments) {
  ParamUpdateMsg m{};
  REQUIRE_FALSE(pub.build(values, inst, m).has_value());
  return m;
}

}  // namespace

TEST_CASE("strategies.scope: PerInstrument keeps a set per instrument, Shared one for all") {
  PerInstrument s;
  REQUIRE_FALSE(s.configure({{"levels", "2"}}).has_value());
  CHECK(s.params(InstrumentId{3}).levels == 2);
  const ParamPublisher pub(ParamSink{}, s.params(), 4);
  CHECK(pub.per_instrument());

  s.apply_param_update(update(pub, {{"levels", "3"}}, InstrumentId{1}));
  CHECK(s.params(InstrumentId{1}).levels == 3);
  CHECK(s.params(InstrumentId{0}).levels == 2);
  CHECK(s.params().levels == 2);

  // An update for every instrument changes the fields it names in every set.
  s.apply_param_update(update(pub, {{"half_spread_bps", "7"}}));
  CHECK(s.params().half_spread_bps == 7_bps);
  CHECK(s.params(InstrumentId{1}).half_spread_bps == 7_bps);
  CHECK(s.params(InstrumentId{1}).levels == 3);
  CHECK(s.params(InstrumentId{2}).levels == 2);
  // An id past the table reads the shared set.
  CHECK(s.params(InstrumentId{kMaxInstruments}).levels == 2);

  Shared one;
  REQUIRE_FALSE(one.configure({}).has_value());
  one.apply_param_update(update(pub, {{"levels", "4"}}, InstrumentId{1}));
  CHECK(one.params(InstrumentId{0}).levels == 4);
  CHECK(&one.params(InstrumentId{2}) == &one.params());
}

TEST_CASE("strategies.scope: the registered publisher follows the strategy's scope") {
  register_builtin_strategies(StrategyRegistry::instance());
  const StrategyEntry* basic = StrategyRegistry::instance().find("basic_mm");
  REQUIRE(basic != nullptr);
  REQUIRE(basic->publisher != nullptr);
  const auto per = basic->publisher(ParamSink{}, {{"half_spread_bps", "5"}}, 3);
  CHECK(per->per_instrument());
  ParamUpdateMsg m{};
  CHECK_FALSE(per->build({{"half_spread_bps", "9"}}, InstrumentId{2}, m).has_value());
  const StrategyEntry* lead = StrategyRegistry::instance().find("lead_mm");
  REQUIRE(lead != nullptr);
  REQUIRE(lead->publisher != nullptr);
  CHECK_FALSE(lead->publisher(ParamSink{}, {}, 3)->per_instrument());
}

TEST_CASE("strategies.scope: basic_mm quotes each instrument with its own parameters") {
  BasicMM s;
  REQUIRE_FALSE(s.configure({{"half_spread_bps", "10"},
                             {"quote_qty", "0.01"},
                             {"max_inventory", "0.1"},
                             {"skew_bps_per_unit", "0"}})
                    .has_value());
  const ParamPublisher pub(ParamSink{}, s.params(), 2);
  s.apply_param_update(update(pub, {{"half_spread_bps", "20"}}, InstrumentId{1}));
  Instrument a{};
  a.id = InstrumentId{0};
  a.tick = Price::from_decimal("0.01").value();
  a.lot = Qty::from_decimal("0.001").value();
  Instrument b = a;
  b.id = InstrumentId{1};
  const Price mid = Price::from_decimal("100").value();
  const DesiredQuotes qa = s.compute_quotes(mid, Qty{}, a);
  const DesiredQuotes qb = s.compute_quotes(mid, Qty{}, b);
  REQUIRE(qa.bids.size() == 1);
  REQUIRE(qb.bids.size() == 1);
  CHECK(qa.bids[0].price == Price::from_decimal("99.9").value());
  CHECK(qb.bids[0].price == Price::from_decimal("99.8").value());
}
