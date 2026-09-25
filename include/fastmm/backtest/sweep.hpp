#pragma once
// Parameter sweep (8.4/8.5): cartesian grid over strategy parameters run on a std::jthread
// pool; every worker owns its own data cursor (source_factory) and backend, results come
// back in grid order (first parameter varies slowest).
//
// walk_forward() runs the same grid on K consecutive time slices (folds) of the data and scores
// the in-sample winner of fold i-1 on fold i: an out-of-sample check of the sweep's best row.
#include "fastmm/backtest/backtest_runner.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fastmm::bt {

using ParamGrid = std::vector<std::pair<std::string, std::vector<std::string>>>;
using SourceFactory = std::function<std::unique_ptr<MdSource>()>;  // may return nullptr
using RunFn = std::function<BacktestResult(const BacktestConfig&, MdSource*)>;

struct SweepPoint {
  ParamMap params;  // the grid values applied for this point
  BacktestResult result;
};

// Expands the grid in order: [{k1: v11, k2: v21}, {k1: v11, k2: v22}, ...].
[[nodiscard]] std::vector<ParamMap> expand_grid(const ParamGrid& grid);

// Generic driver; `run` is called once per point on a worker thread. threads <= 0 uses
// hardware_concurrency.
std::vector<SweepPoint> sweep(const BacktestConfig& base,
                              const ParamGrid& grid,
                              const SourceFactory& source_factory,
                              const RunFn& run,
                              int threads = 0);

template <StrategyLike S>
std::vector<SweepPoint> sweep(const BacktestConfig& base,
                              const ParamGrid& grid,
                              const SourceFactory& source_factory = {},
                              int threads = 0) {
  return sweep(
      base,
      grid,
      source_factory,
      [](const BacktestConfig& c, MdSource* s) { return run_backtest<S>(c, s); },
      threads);
}

inline std::vector<SweepPoint> sweep_by_name(const BacktestConfig& base,
                                             std::string strategy,
                                             const ParamGrid& grid,
                                             const SourceFactory& source_factory = {},
                                             int threads = 0) {
  return sweep(
      base,
      grid,
      source_factory,
      [strategy = std::move(strategy)](const BacktestConfig& c, MdSource* s) {
        return run_backtest(c, strategy, s);
      },
      threads);
}

// ---- walk-forward ------------------------------------------------------------------------------

// Ranking metric of a run, higher is better: "net_pnl", "realized_pnl", "sharpe_bar" or
// "spread_captured_bps". Throws std::invalid_argument for any other name.
using ScoreFn = double (*)(const BacktestResult&);
[[nodiscard]] ScoreFn score_metric(std::string_view name);

struct WalkForwardFold {
  std::int64_t start_ts = 0;  // [start_ts, end_ts) of event time, ns
  std::int64_t end_ts = 0;
  std::vector<SweepPoint> points;  // every grid point run on this fold alone, grid order
  std::vector<double> scores;      // the metric of each point (NaN ranks last)
  std::size_t best = 0;            // in-sample best of this fold (lowest grid index on ties)
  // Folds >= 1: the point chosen on the previous fold, its score there (in-sample), its score
  // here (out-of-sample) and the best score here (hindsight). NaN on fold 0.
  std::size_t chosen = 0;
  double in_sample = std::numeric_limits<double>::quiet_NaN();
  double out_of_sample = std::numeric_limits<double>::quiet_NaN();
  double hindsight = std::numeric_limits<double>::quiet_NaN();
};

struct WalkForwardReport {
  std::string metric;
  std::vector<std::string> param_names;  // grid order
  std::vector<WalkForwardFold> folds;
  // Means over folds 1..K-1 (NaN with one fold).
  double mean_in_sample = std::numeric_limits<double>::quiet_NaN();
  double mean_out_of_sample = std::numeric_limits<double>::quiet_NaN();
  double mean_hindsight = std::numeric_limits<double>::quiet_NaN();
  std::size_t choice_changes = 0;  // folds i >= 2 whose chosen point differs from fold i-1's

  // "k1=v1 k2=v2" of one grid point, in grid order.
  [[nodiscard]] std::string label(const ParamMap& params) const;
  // One line per fold plus the summary.
  [[nodiscard]] std::string table() const;
};

// Splits the data's event-time range into `folds` equal slices and runs the whole grid on each
// one (a fresh backtest per point and fold, all on one pool of `threads` workers). Each fold
// reads a TimeSliceSource over the worker's cursor, so it starts on the book as of its first
// instant with a flat engine. folds == 1 is exactly sweep(). The synthetic market
// (source_factory empty or returning nullptr) is sliced as a SyntheticSource stream, which needs
// fill_model l2_queue: the coupled matching market cannot be cut and throws
// std::invalid_argument.
WalkForwardReport walk_forward(const BacktestConfig& base,
                               const ParamGrid& grid,
                               const SourceFactory& source_factory,
                               const RunFn& run,
                               int folds,
                               std::string_view metric = "net_pnl",
                               int threads = 0);

inline WalkForwardReport walk_forward_by_name(const BacktestConfig& base,
                                              std::string strategy,
                                              const ParamGrid& grid,
                                              const SourceFactory& source_factory,
                                              int folds,
                                              std::string_view metric = "net_pnl",
                                              int threads = 0) {
  return walk_forward(
      base,
      grid,
      source_factory,
      [strategy = std::move(strategy)](const BacktestConfig& c, MdSource* s) {
        return run_backtest(c, strategy, s);
      },
      folds,
      metric,
      threads);
}

}  // namespace fastmm::bt
