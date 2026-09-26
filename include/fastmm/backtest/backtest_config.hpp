#pragma once
// BacktestConfig: everything a run needs, assembled from the TOML sections
// [engine] [[instruments]] [strategy] [strategy.params] [risk] [venues.<x>.fees] [sim]
// [backtest] (8.4, 8.7) or built by hand (examples, tests, Python).
//
//   [backtest]                                    [sim]   (synthetic market)
//   source = "synthetic" | "journal" | "csv"      seed, duration_s, start_mid,
//   path = "data.fmj" | "data.csv"                depth_update_ms, book_ticker,
//   seed = 1                                      limit_rate_per_s, market_rate_per_s,
//   duration_s = 60          (synthetic horizon)  mid_step_rate_per_s,
//   fill_model = "matching" | "l2_queue"          cancel_rate_per_order_s, offset_p,
//   queue_conservatism = 0..1                     base_spread_ticks, limit_qty_median_lots,
//   latency_fixed_us, latency_jitter_us,          market_qty_median_lots, regimes,
//   latency_md_us, latency_md_jitter_us, p_drop   volatile_mult, seed_levels
//   equity_bar_s = 1, initial_capital = 0
//   markout_horizons_s = "1,10,60"
//   output_dir = "runs/backtest", journal_out = ""
#include "fastmm/config/config.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/params.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::bt {

struct BacktestConfig {
  EngineConfig engine;
  InstrumentTable instruments;
  std::string strategy;  // registry name (apps / Python); ignored by run_backtest<S>()
  ParamMap params;
  sim::SimTransportConfig transport;
  sim::MarketGeneratorParams generator;
  std::uint64_t seed = 1;  // synthetic market + latency model (engine.rng_seed is separate)
  Timestamp start{seconds(1'700'000'000).ns};  // synthetic start; data sources use their own
  Duration duration = seconds(60);             // synthetic horizon
  Duration equity_bar = seconds(1);
  double initial_capital = 0.0;  // reporting only (drawdown %)
  // [accounting] and the venue names its sources name (Config::venues order). from_config() builds
  // engine.fx from them for `instruments`; replay builds it again for the journal's table.
  AccountingSpec accounting;
  std::vector<std::string> venue_names;
  // from_config: Config::warnings plus unknown [backtest] keys, each naming the key and its line.
  std::vector<std::string> warnings;
  int generator_seed_levels = 20;
  std::string source;  // "synthetic" | "journal" | "csv" | "" (caller supplies the source)
  std::string path;
  std::string output_dir;
  std::string journal_out;  // record the run to this .fmj (empty = no journal)
  // Effective configuration embedded in journal_out (Config::effective_toml(); from_config() sets
  // it, hand-built configs leave it empty). Callers that override fields after from_config()
  // must refresh or clear it.
  std::string config_toml;
  bool measure_wall_clock = true;
  // Post-fill markout horizons in simulated time; empty turns markouts off. The run stops the
  // simulated clock at every fill time + horizon to read the venue mid there, so the shortest
  // horizon also bounds how often the run loop is entered.
  std::vector<Duration> markout_horizons = {seconds(1), seconds(10), seconds(60)};

  // Throws ConfigError on invalid values.
  [[nodiscard]] static BacktestConfig from_config(const Config& cfg);
  // One instrument (venue 0) with the given tick/lot; generator tick/lot follow (examples).
  [[nodiscard]] static BacktestConfig single_instrument(std::string_view symbol,
                                                        Price tick,
                                                        Qty lot);

  // The FX plan for `table` under `accounting` (throws ConfigError when a limit reads the totals
  // and the table is not covered; otherwise a warning goes to `warnings` and nothing converts).
  [[nodiscard]] FxPlan fx_plan(const InstrumentTable& table);

  // "key=value" strategy parameter override (CLI --param).
  void set_param(const std::string& key, const std::string& value) { params[key] = value; }
  // Seeds the synthetic market and the latency model.
  void set_seed(std::uint64_t s) noexcept {
    seed = s;
    transport.seed = s;
  }
};

}  // namespace fastmm::bt
