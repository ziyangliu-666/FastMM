#include "fastmm/backtest/sweep.hpp"

#include "fastmm/backtest/registrations.hpp"

#include <atomic>
#include <exception>
#include <thread>

namespace fastmm::bt {

std::vector<ParamMap> expand_grid(const ParamGrid& grid) {
  std::vector<ParamMap> out{ParamMap{}};
  for (const auto& [name, values] : grid) {
    if (values.empty()) continue;
    std::vector<ParamMap> next;
    next.reserve(out.size() * values.size());
    for (const ParamMap& base : out) {
      for (const std::string& v : values) {
        ParamMap m = base;
        m[name] = v;
        next.push_back(std::move(m));
      }
    }
    out = std::move(next);
  }
  return out;
}

std::vector<SweepPoint> sweep(const BacktestConfig& base,
                              const ParamGrid& grid,
                              const SourceFactory& source_factory,
                              const RunFn& run,
                              int threads) {
  // The registry is not thread-safe for writers: register before any worker can look it up
  // (run_backtest by name registers too, which would otherwise race on the first sweep).
  register_builtin_strategies();
  const std::vector<ParamMap> points = expand_grid(grid);
  std::vector<SweepPoint> results(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) results[i].params = points[i];
  if (points.empty()) return results;
  unsigned n = threads > 0 ? static_cast<unsigned>(threads) : std::thread::hardware_concurrency();
  if (n == 0) n = 1;
  if (n > points.size()) n = static_cast<unsigned>(points.size());
  if (n > 32) n = 32;  // Logger supports a bounded number of threads
  std::atomic<std::size_t> next{0};
  std::exception_ptr error;
  std::atomic<bool> failed{false};
  {
    std::vector<std::jthread> pool;
    pool.reserve(n);
    for (unsigned w = 0; w < n; ++w) {
      pool.emplace_back([&] {
        std::unique_ptr<MdSource> source = source_factory ? source_factory() : nullptr;
        for (;;) {
          const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= points.size() || failed.load(std::memory_order_relaxed)) return;
          try {
            BacktestConfig cfg = base;
            for (const auto& [k, v] : points[i]) cfg.params[k] = v;
            cfg.output_dir.clear();
            cfg.journal_out.clear();
            if (source) source->reset();
            results[i].result = run(cfg, source.get());
          } catch (...) {
            if (!failed.exchange(true)) error = std::current_exception();
            return;
          }
        }
      });
    }
  }
  if (error) std::rethrow_exception(error);
  return results;
}

}  // namespace fastmm::bt
