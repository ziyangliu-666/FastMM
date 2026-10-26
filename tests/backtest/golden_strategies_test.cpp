// Golden outbound hashes for the built-in strategies on fixed seeds (ADR-0012, step 1).
//
// Each case runs one strategy through the template path run_backtest<S>() and pins the SHA-256 of
// the outbound stream. Strategy API changes must keep these hashes. A change that alters behaviour
// on purpose re-baselines them in a commit of its own that names the reason (the intended
// differences are listed in docs/adr/0012-strategy-developer-experience.md).
//
// FASTMM_PRINT_GOLDEN=1 prints the hashes, message and fill counts of the current code.
#include "backtest_test_util.hpp"
#include "options_market.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/core/options/black76.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/strategies/avellaneda_stoikov.hpp"
#include "fastmm/strategies/basic_mm.hpp"
#include "fastmm/strategies/options_mm.hpp"

#include <cmath>
#include <cstdlib>
#include <string>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

void check_golden(const std::string& name, const BacktestResult& r, const std::string& expected) {
  const char* print = std::getenv("FASTMM_PRINT_GOLDEN");
  if (print != nullptr && std::string(print) == "1") {
    MESSAGE(name << " outbound_sha256 " << r.outbound_sha256 << " messages " << r.outbound_messages
                 << " fills " << r.metrics.fills);
  }
  CAPTURE(name);
  CHECK(r.outbound_sha256 == expected);
}

}  // namespace

TEST_CASE("backtest.golden: BasicMM outbound hash, coupled matching market, two levels") {
  BacktestConfig cfg = synthetic_config(21, seconds(20));
  cfg.params["levels"] = "2";
  cfg.params["level_step_ticks"] = "2";
  const BacktestResult r = run_backtest<BasicMM>(cfg);
  REQUIRE(r.metrics.fills > 0);
  check_golden(
      "basic_mm/coupled", r, "a4c5af9990a2beeb5a83624670328153b468f8e17db75f8736a896e819644c2f");
}

TEST_CASE("backtest.golden: BasicMM outbound hash, L2 queue model with stale-book pulls") {
  BacktestConfig cfg = synthetic_config(22, seconds(15));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 5000;
  cfg.params["pull_on_stale_ms"] = "20";
  SyntheticSourceConfig sc;
  sc.generator = cfg.generator;
  sc.md = cfg.transport.md;
  sc.seed = 22;
  sc.duration = seconds(15);
  SyntheticSource src(sc);
  const BacktestResult r = run_backtest<BasicMM>(cfg, &src);
  REQUIRE(r.engine.timers_fired > 0);
  check_golden(
      "basic_mm/l2_queue", r, "fadbd3081e2ac55841ec246699c20e4ce559b5cdf4960479363c91a2ed42d57c");
}

TEST_CASE("backtest.golden: AvellanedaStoikov outbound hash on a fixed seed") {
  BacktestConfig cfg = synthetic_config(23, seconds(15));
  cfg.params = {{"gamma", "0.1"},
                {"kappa", "100"},
                {"estimate_kappa", "1"},
                {"infinite_horizon", "0"},
                {"horizon_s", "5"},
                {"quote_qty", "0.002"},
                {"max_inventory", "0.02"}};
  const BacktestResult r = run_backtest<AvellanedaStoikov>(cfg);
  REQUIRE(r.metrics.fills > 0);
  check_golden("avellaneda_stoikov/coupled",
               r,
               "baf2eb80251aed06d148c5a7f04f6a48928440f7c4e3a6fcef7e3f7a756b1cb8");
}

TEST_CASE("backtest.golden: OptionsMM outbound hash on a scripted options market") {
  BacktestConfig cfg;
  cfg.instruments = option_table();
  cfg.set_seed(24);
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.fees = sim::FeeModel::from_bps(0.0, 3.0);
  cfg.engine.quotes.min_requote_interval = milliseconds(50);
  cfg.engine.risk.max_open_orders = 8;
  cfg.params = {{"quote_qty", "0.1"},
                {"max_position", "0.5"},
                {"max_delta", "0.3"},
                {"delta_skew_ticks", "5"},
                {"inventory_skew_ticks", "1"},
                {"max_vega", "0"},
                {"pull_on_stale_ms", "1000"}};
  cfg.measure_wall_clock = false;
  ScriptedOptionsSource src(cfg.instruments);
  const BacktestResult r = run_backtest<OptionsMM>(cfg, &src);
  REQUIRE(r.metrics.fills > 0);
  REQUIRE(r.engine.timers_fired > 0);
  check_golden(
      "options_mm/scripted", r, "77d40f19c3ba5d58bb73a590cca73df7e0b7258b85c180aa69beeb12016fe0d7");
}

TEST_CASE("backtest.golden: BasicMM outbound hash, L2 queue model, three levels a tick apart") {
  // One print through several of our levels is shared by them in price priority (queue_model.hpp).
  BacktestConfig cfg = synthetic_config(22, seconds(15));
  cfg.transport.fill_model = sim::FillModel::L2Queue;
  cfg.transport.queue_conservatism_bps = 5000;
  cfg.params["levels"] = "3";
  cfg.params["level_step_ticks"] = "1";
  SyntheticSourceConfig sc;
  sc.generator = cfg.generator;
  sc.md = cfg.transport.md;
  sc.seed = 22;
  sc.duration = seconds(15);
  SyntheticSource src(sc);
  const BacktestResult r = run_backtest<BasicMM>(cfg, &src);
  REQUIRE(r.metrics.fills > 0);
  check_golden("basic_mm/l2_queue_levels",
               r,
               "a2b53f6a149947015c7594ee2c20403a5b5d419c088c0d775c2af545ba6e0195");
}
