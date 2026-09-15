// max_param_age (ADR-0013) through StrategyHarness: no quotes before the first parameter update,
// quotes pulled once the parameters are too old while book updates keep arriving and in a quiet
// market, quoting back on the next update.
#include "test_support.hpp"

#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/testing/strategy_harness.hpp"

#include <stdexcept>
#include <vector>

using namespace fastmm;

namespace {

// BasicMM, recording on_quoting and on_params.
struct AgedMM : BasicMM {
  std::vector<bool> quoting;
  int params_calls = 0;
  template <class Ctx>
  void on_quoting(Ctx& ctx, bool enabled) noexcept {
    quoting.push_back(enabled);
    BasicMM::on_quoting(ctx, enabled);
  }
  template <class Ctx>
  void on_params(Ctx& /*ctx*/) noexcept {
    ++params_calls;
  }
};

const ParamMap kParams{{"half_spread_bps", "5"},
                       {"quote_qty", "0.01"},
                       {"max_inventory", "0.1"},
                       {"pull_on_stale_ms", "0"}};

sim::HarnessOptions aged(Duration max_age) {
  sim::HarnessOptions o;
  o.engine.max_param_age = max_age;
  return o;
}

}  // namespace

TEST_CASE(
    "strategies.param_age: no quotes before the first update, pulled when stale, back on the "
    "next") {
  sim::StrategyHarness<AgedMM> h(kParams, aged(milliseconds(500)));
  const AgedMM& s = h.strategy();
  CHECK_FALSE(h.engine().quoting_enabled());
  h.book("100.00", "100.02");
  h.advance(milliseconds(1));
  CHECK(h.working_orders().empty());
  CHECK(s.quoting.empty());

  h.publish({{"half_spread_bps", "4"}});
  CHECK(s.params_calls == 1);
  CHECK(s.params().half_spread_bps == 4_bps);
  CHECK(s.quoting == std::vector<bool>{true});
  h.advance(milliseconds(1));
  CHECK(h.working_orders().size() == 2);

  // Book updates keep arriving and BasicMM keeps requoting, but no parameter update: 500 ms after
  // the last one the quotes are pulled and later books do not bring them back.
  for (int i = 0; i < 4; ++i) {
    h.advance(milliseconds(100));
    h.book(i % 2 == 0 ? "100.10" : "100.00", i % 2 == 0 ? "100.12" : "100.02");
  }
  h.advance(milliseconds(1));
  CHECK(h.working_orders().size() == 2);
  CHECK(s.quoting == std::vector<bool>{true});
  for (int i = 0; i < 3; ++i) {
    h.advance(milliseconds(100));
    h.book(i % 2 == 0 ? "100.20" : "100.30", i % 2 == 0 ? "100.22" : "100.32");
  }
  h.advance(milliseconds(1));
  CHECK(s.quoting == std::vector<bool>{true, false});
  CHECK(h.engine().params_stale());
  CHECK(h.engine().stats().param_expiries == 1);
  CHECK(h.working_orders().empty());

  // The next update: quoting is back and BasicMM requotes from on_quoting.
  h.publish({});
  CHECK(s.quoting == std::vector<bool>{true, false, true});
  h.advance(milliseconds(1));
  CHECK(h.working_orders().size() == 2);

  // A quiet market: the engine's own timer pulls the quotes.
  h.advance(milliseconds(600));
  CHECK(s.quoting == std::vector<bool>{true, false, true, false});
  CHECK(h.working_orders().empty());
  CHECK(h.engine().stats().param_expiries == 2);

  const auto out_of_range = [&h] { h.publish({{"half_spread_bps", "-1"}}); };
  CHECK_THROWS_AS(out_of_range(), std::invalid_argument);
}

TEST_CASE("strategies.param_age: off by default") {
  sim::StrategyHarness<AgedMM> h(kParams);
  CHECK(h.engine().quoting_enabled());
  h.book("100.00", "100.02");
  h.advance(seconds(5));
  CHECK(h.working_orders().size() == 2);
  CHECK(h.strategy().quoting.empty());
}
