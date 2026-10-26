#pragma once
// Multiverse: many backtests of one strategy over the same recorded data in one pass.
//
// The recorded market does not depend on the orders of a run: under fill_model = "l2_queue" every
// run reads the same events, puts them in the same venue order and keeps the same venue books, and
// only its own orders, queue positions, engine and strategy differ. A sweep or the arms of an A/B
// test run as separate backtests each read and order the data and rebuild the books again; a
// multiverse does that once (sim::SimWorld) and steps every run through each event in turn, so a
// run costs what its own orders, engine and strategy cost.
//
// Each run's result is the one run_backtest() gives for its configuration: the same events reach
// it at the same times in the same order, its latency draws are its own (each run keeps its own
// SimTransport and seed), and the venue books it reads are the books it would have built. The
// outbound hash, fills and every metric are identical (tests/backtest/multiverse_test.cpp).
//
// Runs go in groups of `threads` (<= 0: hardware_concurrency); each group reads its own cursor
// from `source_factory` and keeps its own SimWorld, its runs in lockstep on one thread.
#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/sweep.hpp"

#include <string_view>
#include <vector>

namespace fastmm::bt {

// Throws std::invalid_argument when the configurations cannot share a pass: fill_model must be
// l2_queue, the data recorded (not synthetic), and the instrument tables and reorder windows the
// same; or for an unknown strategy. `source_factory` must open the data the configurations name.
[[nodiscard]] std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                                         std::string_view strategy,
                                                         const SourceFactory& source_factory,
                                                         int threads = 0);

// Every run in one multiverse over `source`, read from its current position (reset() it first to
// reuse one), on the calling thread.
[[nodiscard]] std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                                         std::string_view strategy,
                                                         MdSource& source);

// sweep_by_name() in multiverses: the same points, in grid order, with the same results.
[[nodiscard]] std::vector<SweepPoint> sweep_multiverse(const BacktestConfig& base,
                                                       std::string_view strategy,
                                                       const ParamGrid& grid,
                                                       const SourceFactory& source_factory,
                                                       int threads = 0);

}  // namespace fastmm::bt
