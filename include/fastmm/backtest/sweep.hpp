#pragma once
// Parameter sweep (8.4/8.5): a list of points, or the cartesian grid that expands to one, over
// strategy parameters and backtest settings, run on a std::jthread pool; every worker owns its own
// data cursor (source_factory) and backend, results come back in the points' order (a grid's
// first parameter varies slowest).
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
// The points of a sweep, in order: a grid expanded (expand_grid) or any list.
using SweepPoints = std::vector<ParamMap>;
using SourceFactory = std::function<std::unique_ptr<MdSource>()>;  // may return nullptr
using RunFn = std::function<BacktestResult(const BacktestConfig&, MdSource*)>;

struct SweepPoint {
  ParamMap params;  // the grid values applied for this point
  BacktestResult result;
};

// Called with each point's result as it is done (index in the list of points or configurations),
// one call at a time, before it is kept: a caller that needs less of it (the CLI prints the
// metrics) can drop the rest there, so a sweep of thousands of points holds no more than that.
using ResultHook = std::function<void(std::size_t index, BacktestResult& result)>;

// Expands the grid in order: [{k1: v11, k2: v21}, {k1: v11, k2: v22}, ...].
[[nodiscard]] SweepPoints expand_grid(const ParamGrid& grid);

// The configuration a point runs: `start` (shared_start() of the sweep's base) with each key of
// `point` set, a key of kSweepSettings as that backtest setting (BacktestConfig::set_setting) and
// any other as a strategy parameter; no output directory, journal or order rows.
[[nodiscard]] BacktestConfig point_config(const BacktestConfig& start, const ParamMap& point);

// Generic driver; `run` is called once per point on a worker thread. threads <= 0 uses
// hardware_concurrency.
std::vector<SweepPoint> sweep(const BacktestConfig& base,
                              const SweepPoints& points,
                              const SourceFactory& source_factory,
                              const RunFn& run,
                              int threads = 0,
                              const ResultHook& done = {});
std::vector<SweepPoint> sweep(const BacktestConfig& base,
                              const ParamGrid& grid,
                              const SourceFactory& source_factory,
                              const RunFn& run,
                              int threads = 0,
                              const ResultHook& done = {});

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

// A registered strategy's sweep. Under the queue fill models (l2_queue, l3_queue) the points share
// their passes over the data (sweep_multiverse(), multiverse.hpp) with the results each gives
// alone; otherwise, or with `separately`, every point is a backtest of its own.
std::vector<SweepPoint> sweep_by_name(const BacktestConfig& base,
                                      std::string strategy,
                                      const SweepPoints& points,
                                      const SourceFactory& source_factory = {},
                                      int threads = 0,
                                      bool separately = false,
                                      const ResultHook& done = {});
std::vector<SweepPoint> sweep_by_name(const BacktestConfig& base,
                                      std::string strategy,
                                      const ParamGrid& grid,
                                      const SourceFactory& source_factory = {},
                                      int threads = 0,
                                      bool separately = false,
                                      const ResultHook& done = {});

// ---- walk-forward ------------------------------------------------------------------------------

// Ranking metric of a run, higher is better. score_metric() names four, "net_pnl",
// "realized_pnl", "sharpe_bar" and "spread_captured_bps", and throws std::invalid_argument for any
// other name.
using ScoreFn = std::function<double(const BacktestResult&)>;
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
  std::vector<std::string> param_names;  // grid order; of a list of points, every key they set
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

// The k + 1 edges of `folds` equal slices of the data's event-time range: fold i is
// [edges[i], edges[i + 1]). Reads one cursor from `source_factory` to the end; an empty factory
// (or one returning nullptr) reads the synthetic market as a SyntheticSource stream, which needs
// fill_model l2_queue (std::invalid_argument otherwise, and for data with no events or folds < 2).
[[nodiscard]] std::vector<Timestamp> fold_edges(const BacktestConfig& base,
                                                const SourceFactory& source_factory,
                                                int folds);

// The cursor a walk-forward fold reads: `inner` from `t0` to `t1`, owning it. A null `inner` is the
// synthetic market of `base` as a stream (fill_model l2_queue; std::invalid_argument otherwise).
[[nodiscard]] std::unique_ptr<MdSource> fold_source(const BacktestConfig& base,
                                                    std::unique_ptr<MdSource> inner,
                                                    Timestamp t0,
                                                    Timestamp t1);

// Scores the points of every fold of `rep` by `score_fn`, or by the metric named `rep.metric` when
// `score_fn` is empty, and fills in best, chosen, the in-sample / out-of-sample / hindsight scores,
// the means and the choice changes. With one fold, its start_ts / end_ts are set from its results.
// walk_forward() ends with it; a caller that ran the points itself (the Python package, for
// strategies written in Python) or ranks by a score of its own calls it directly. Throws
// std::invalid_argument for an unknown metric.
void score_walk_forward(WalkForwardReport& rep, const ScoreFn& score_fn = {});

// Splits the data's event-time range into `folds` equal slices and runs the whole grid on each
// one (a fresh backtest per point and fold, all on one pool of `threads` workers). Each fold
// reads a TimeSliceSource over the worker's cursor, so it starts on the book as of its first
// instant with a flat engine. folds == 1 is exactly sweep(). The synthetic market
// (source_factory empty or returning nullptr) is sliced as a SyntheticSource stream, which needs
// fill_model l2_queue: the coupled matching market cannot be cut and throws
// std::invalid_argument.
WalkForwardReport walk_forward(const BacktestConfig& base,
                               const SweepPoints& points,
                               const SourceFactory& source_factory,
                               const RunFn& run,
                               int folds,
                               std::string_view metric = "net_pnl",
                               int threads = 0);
WalkForwardReport walk_forward(const BacktestConfig& base,
                               const ParamGrid& grid,
                               const SourceFactory& source_factory,
                               const RunFn& run,
                               int folds,
                               std::string_view metric = "net_pnl",
                               int threads = 0);

// walk_forward() of a registered strategy. Under the queue fill models the points of each fold
// share their passes over the fold's data (sweep_multiverse(), multiverse.hpp), with the same
// report. `done` sees each result, with index fold * points + point (ResultHook).
WalkForwardReport walk_forward_by_name(const BacktestConfig& base,
                                       std::string strategy,
                                       const SweepPoints& points,
                                       const SourceFactory& source_factory,
                                       int folds,
                                       std::string_view metric = "net_pnl",
                                       int threads = 0,
                                       const ResultHook& done = {});
WalkForwardReport walk_forward_by_name(const BacktestConfig& base,
                                       std::string strategy,
                                       const ParamGrid& grid,
                                       const SourceFactory& source_factory,
                                       int folds,
                                       std::string_view metric = "net_pnl",
                                       int threads = 0,
                                       const ResultHook& done = {});

}  // namespace fastmm::bt
