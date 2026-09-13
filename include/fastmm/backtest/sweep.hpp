#pragma once
// Parameter sweep (8.4/8.5): cartesian grid over strategy parameters run on a std::jthread
// pool; every worker owns its own data cursor (source_factory) and backend, results come
// back in grid order (first parameter varies slowest).
#include "fastmm/backtest/backtest_runner.hpp"

#include <functional>
#include <memory>
#include <string>
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

}  // namespace fastmm::bt
