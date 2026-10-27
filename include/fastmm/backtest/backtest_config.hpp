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
//   latency_ack_us, latency_ack_jitter_us,
//   latency_md_us, latency_md_jitter_us, p_drop   volatile_mult, seed_levels, spread_bps,
//                                                 mid_step_bps, mid_step_ticks, depth_bps
//   latency_cancel_us, latency_cancel_jitter_us, order_service_us
//   md_arrival = "venue" | "recorded", reorder_window_ms = 1000
//   balances_from_journal = false, own_orders_in_feed = true, transfer_latency_ms = 0
//   initial_state = "state_file" | "journal" | "none", params_from_journal = false
//   [backtest.balances] <ASSET> = "<amount>"  (and [backtest.venues.<name>.balances])
//   [backtest.positions] <SYMBOL> = "<qty>" | "<qty>@<avg_px>"
//   start_at = <unix s> | "<UTC time>", latency_from_journal = false
//   equity_bar_s = 1, initial_capital = 0
//   markout_horizons_s = "1,10,60"
//   output_dir = "runs/backtest", journal_out = ""
#include "fastmm/config/config.hpp"
#include "fastmm/core/engine.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/treasury.hpp"
#include "fastmm/sim/market_generator.hpp"
#include "fastmm/sim/sim_transport.hpp"
#include "fastmm/strategies/params.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::bt {

// The backtest settings a sweep's point can vary besides the strategy's parameters
// (BacktestConfig::set_setting). The Python BacktestConfig has an attribute of each name.
inline constexpr std::array<std::string_view, 8> kSweepSettings = {"latency_fixed_us",
                                                                   "latency_jitter_us",
                                                                   "latency_md_us",
                                                                   "latency_md_jitter_us",
                                                                   "p_drop",
                                                                   "queue_conservatism",
                                                                   "maker_fee_bps",
                                                                   "taker_fee_bps"};

// Where the strategy's starting state comes from ([backtest] initial_state).
enum class InitialState : std::uint8_t {
  StateFile = 0,  // [strategy] state_file, when set
  Journal = 1,    // the data journal's (journal_state.hpp), with the session's positions
  None = 2,       // nothing: the strategy starts fresh
};

// The [sim] defaults of a configuration file: a liquid crypto pair. New limit orders leave 4 bps
// around the latent mid and rest 1 bps behind that on average; orders a mid move left behind rest
// inside it, so the book's spread is 1.7 to 3.4 bps (10th to 90th percentile), 2.7 at the median.
// Latent mid steps of 0.5 bps, 2 a second calm and 8 volatile, give 0.4 to 0.7 bps of volatility a
// square-root second.
inline constexpr double kDefaultSpreadBps = 4.0;
inline constexpr double kDefaultMidStepBps = 0.5;
inline constexpr double kDefaultDepthBps = 1.0;

// One [backtest.positions] row: the engine starts with this position in the instrument.
struct StartPosition {
  InstrumentId instrument;
  Qty qty;
  Price avg_px;
};

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
  // [risk.underlying]: from_config() builds engine.underlying from it for `instruments`; replay
  // builds it again for the journal's table.
  UnderlyingSpec underlying;
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
  // The result keeps a row for every order sent (BacktestResult::orders). A sweep's points do not:
  // their metrics count the orders as they go (OrderStats), and the outbound hash covers them.
  bool keep_orders = true;
  // Post-fill markout horizons in simulated time; empty turns markouts off. The run stops the
  // simulated clock at every fill time + horizon to read the venue mid there, so the shortest
  // horizon also bounds how often the run loop is entered.
  std::vector<Duration> markout_horizons = {seconds(1), seconds(10), seconds(60)};
  // Start each venue's account (transport.accounts) from the first balance snapshot the journal
  // source recorded for it; a venue it has none for keeps the configured balances.
  bool balances_from_journal = false;
  // The pool treasuries ([venues.<primary>.treasury] with enabled): each runs on simulated time
  // against its pool's simulated accounts (sim/sim_treasury.hpp), without a ledger file. Empty:
  // none, and the run is what it was without them.
  std::vector<TreasuryConfig> treasuries;
  // [backtest] transfer_latency_ms: a simulated transfer is carried out this long after it is sent.
  Duration transfer_latency{};
  // The strategy's starting state; Journal also starts the engine's positions where the recorded
  // session had them (sim::SessionStart).
  InitialState initial_state = InitialState::StateFile;
  // [backtest.positions]: the engine's positions at the start, by instrument. They replace the
  // journal's (initial_state = "journal") for the instruments listed: a backtest that starts at a
  // part from the middle of a session, whose records do not repeat the positions, starts where the
  // session was.
  std::vector<StartPosition> positions;
  // [backtest] start_at: the run reads the data from its start (the books and the strategy's view
  // of the market warm up), but trading starts later: with initial_state = "journal" at the last
  // checkpoint of the data journal at or before this time, whose state the strategy restores as a
  // warm standby's take-over does, else at this time. Until then every venue an order can go to
  // has not reconciled, so the engine refuses orders (NotReconciled); at the start each one
  // reconciles with the start positions. Invalid: trading starts with the data.
  Timestamp start_at{};
  // [backtest] latency_from_journal: the simulated venues take a message's latency from the data
  // journal's own messages around the same time where that is longer than the model's draw
  // (sim/recorded_latency.hpp); the data source must be a journal.
  bool latency_from_journal = false;
  // The data journal's parameter updates reach the strategy at the times the session received
  // them (sim::RecordedParams), mapped by name and type onto its schema.
  bool params_from_journal = false;
  // [ab]: an A/B test of the strategy's parameters (strategies/ab_test.hpp); no arms: none.
  AbSection ab;
  // [ab] assignment = "branch": the arm this run is (an index into ab.arms); fastmm-backtest runs
  // each one in turn.
  int ab_arm = -1;
  // Recorded data reaches the simulated venue in venue-time order (sim/venue_order.hpp), assuming
  // no event was received more than this after its venue time; zero keeps the recorded order.
  Duration reorder_window = milliseconds(1000);

  // Throws ConfigError on invalid values.
  [[nodiscard]] static BacktestConfig from_config(const Config& cfg);
  // One instrument (venue 0) with the given tick/lot; generator tick/lot follow (examples).
  [[nodiscard]] static BacktestConfig single_instrument(std::string_view symbol,
                                                        Price tick,
                                                        Qty lot);

  // The FX plan for `table` under `accounting` (throws ConfigError when a limit reads the totals
  // and the table is not covered; otherwise a warning goes to `warnings` and nothing converts).
  [[nodiscard]] FxPlan fx_plan(const InstrumentTable& table);
  // The [risk.underlying] plan for `table` (throws ConfigError when the spec does not fit it).
  [[nodiscard]] UnderlyingPlan underlying_plan(const InstrumentTable& table) const;

  // The synthetic market in basis points of generator.start_mid, turned into ticks of
  // generator.tick: the spread from touch to touch (base_spread_ticks, half of it a side), the
  // size of a latent mid step (mid_step_ticks) and the mean distance of a new limit order behind
  // the touch (depth_ticks). Each comes to at least one tick. from_config() applies the [sim]
  // defaults below unless the tick keys are given; single_instrument() keeps the one-tick market
  // of MarketGeneratorParams.
  void set_market_bps(double spread_bps, double mid_step_bps, double depth_bps);

  // Sets one of kSweepSettings from its decimal value: latency_fixed_us and latency_jitter_us the
  // order and ack paths of the venues without a [backtest.venues.<name>] entry, latency_md_us and
  // latency_md_jitter_us their market data, p_drop the share of orders lost, queue_conservatism
  // the queue model's, maker_fee_bps and taker_fee_bps one schedule for every instrument.
  // Returns false for any other key; throws std::invalid_argument for a value out of range.
  bool set_setting(std::string_view key, std::string_view value);
  // "key=value" strategy parameter override (CLI --param).
  void set_param(const std::string& key, const std::string& value) { params[key] = value; }
  // Seeds the synthetic market and the latency model.
  void set_seed(std::uint64_t s) noexcept {
    seed = s;
    transport.seed = s;
  }
};

}  // namespace fastmm::bt
