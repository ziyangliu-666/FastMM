#pragma once
// Multiverse: many backtests over the same recorded data in one pass, each giving the result it
// gives alone.
//
// Under the queue fill models (l2_queue, l3_queue) the recorded market does not depend on the
// orders of a run: every run over the same data reads the same events, puts them in the same venue
// order and keeps the same venue books and, under l3_queue, the same record of every order. Only a
// run's own orders, their places in the queues, its engine and its strategy differ. A multiverse
// keeps what the runs share once:
//
//   * one cursor over the data and one sim::SimWorld, which takes each recorded event for every
//     run (the venue's books, its tickers and trades, the recorded orders and their levels);
//   * an index of the orders the runs rest at the venue, by instrument, side and price: a recorded
//     event goes to the runs whose orders it can reach (a cancel ahead of them, an execution that
//     meets them, a print or a cross at or through their price) and to no other;
//   * the market data forwarded to the engines of the runs whose feed is the same: one copy in
//     recorded order with its arrival, one set of engine books and one core/market_view.hpp
//     MarketView they all follow (Engine::follow_market). A message goes only to the engines
//     whose MdInterest asks for it (what their strategy watches, the band their max_loss can
//     trip in, the settings that act on every message); the others catch up with it, exactly,
//     before they next take anything. Runs whose feed shows their own orders (own_orders_in_feed)
//     or whose market data arrives with jitter forward their own, as alone.
//
// Each run keeps its own engine, strategy, simulated venue links, accounts, latency draws and
// seed. The runs are stepped in lockstep on the order of the events they share: before a recorded
// event every run takes its own events (order arrivals, acks and fills, timers, parameter updates,
// equity bars and markouts) that come before it, before market data the runs that read it do, and
// at one instant the kinds go in the order a run alone takes them (sim::SimKey). So every run sees
// its events in the order and at the times it sees them alone, and its result is the one
// run_backtest() gives for its configuration: fills, orders, equity, markouts, cash, positions and
// outbound hash (tests/backtest/multiverse_exact_test.cpp, bt::result_difference).
//
// The runs of a configuration list go in passes of at most 128 runs (a run holds its engine and
// venue, about 6 MB), `threads` passes at a time (<= 0: hardware concurrency), as many waves of
// them as that takes. A wave reads the data once: one more thread reads a cursor from
// `source_factory`, puts its events in venue order and hands them to every pass of the wave, each
// of which keeps its own world and market data.
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/sweep.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::bt {

// A run of a multiverse: its session (with an [ab] publisher, a parameter schedule or a slow tier
// set as needed) and its engine's runner and hooks, which begin() is called with.
struct MultiverseRun {
  std::unique_ptr<BacktestSession> session;
  std::unique_ptr<IEngineRunner> runner;
  sim::EngineHooks hooks{};
  std::string strategy_name;
  // Called once the run's result is taken, before its engine goes (may be empty).
  std::function<void()> on_end;
};
// Builds the run of cfgs[index] over `source` (the pass's cursor, which the run must not read).
using MultiverseFactory =
    std::function<MultiverseRun(std::size_t index, const BacktestConfig& cfg, MdSource& source)>;

// Throws std::invalid_argument when the configurations cannot share a pass: fill_model must be
// l2_queue or l3_queue, the data recorded (not synthetic), and the instrument tables and reorder
// windows the same.
// `done` sees each run's result as its pass ends (sweep.hpp, ResultHook).
[[nodiscard]] std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                                         const MultiverseFactory& make,
                                                         const SourceFactory& source_factory,
                                                         int threads = 0,
                                                         const ResultHook& done = {});

// The runs of a registered strategy; also throws for an unknown strategy.
[[nodiscard]] std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                                         std::string_view strategy,
                                                         const SourceFactory& source_factory,
                                                         int threads = 0,
                                                         const ResultHook& done = {});

// Every run in one pass over `source`, read from its current position (reset() it first to
// reuse one), on the calling thread.
[[nodiscard]] std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                                         std::string_view strategy,
                                                         MdSource& source);

// Empty when the configurations can share a pass, else why not.
[[nodiscard]] std::string multiverse_refusal(const std::vector<BacktestConfig>& cfgs);

// sweep_by_name() in multiverses: the same points, in grid order, with the same results.
[[nodiscard]] std::vector<SweepPoint> sweep_multiverse(const BacktestConfig& base,
                                                       std::string_view strategy,
                                                       const ParamGrid& grid,
                                                       const SourceFactory& source_factory,
                                                       int threads = 0,
                                                       const ResultHook& done = {});

}  // namespace fastmm::bt
