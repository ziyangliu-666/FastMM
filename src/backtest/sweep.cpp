#include "fastmm/backtest/sweep.hpp"

#include "fastmm/backtest/registrations.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>
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

namespace {

// Runs task(i, source) for i in [0, n) on a pool; every worker owns one source from `factory`
// (may be null). The first exception stops the pool and is rethrown.
template <class Task>
void run_pool(std::size_t n, int threads, const SourceFactory& factory, const Task& task) {
  if (n == 0) return;
  unsigned w = threads > 0 ? static_cast<unsigned>(threads) : std::thread::hardware_concurrency();
  if (w == 0) w = 1;
  if (w > n) w = static_cast<unsigned>(n);
  if (w > 32) w = 32;  // Logger supports a bounded number of threads
  std::atomic<std::size_t> next{0};
  std::exception_ptr error;
  std::atomic<bool> failed{false};
  {
    std::vector<std::jthread> pool;
    pool.reserve(w);
    for (unsigned k = 0; k < w; ++k) {
      pool.emplace_back([&] {
        std::unique_ptr<MdSource> source = factory ? factory() : nullptr;
        for (;;) {
          const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= n || failed.load(std::memory_order_relaxed)) return;
          try {
            task(i, source.get());
          } catch (...) {
            if (!failed.exchange(true)) error = std::current_exception();
            return;
          }
        }
      });
    }
  }
  if (error) std::rethrow_exception(error);
}

BacktestConfig point_config(const BacktestConfig& base, const ParamMap& point) {
  BacktestConfig cfg = base;
  for (const auto& [k, v] : point) cfg.params[k] = v;
  cfg.output_dir.clear();
  cfg.journal_out.clear();
  return cfg;
}

// Higher is better; NaN loses to everything, ties go to the lower index.
std::size_t argmax(const std::vector<double>& scores) {
  std::size_t best = 0;
  for (std::size_t i = 1; i < scores.size(); ++i) {
    if (std::isnan(scores[i])) continue;
    if (std::isnan(scores[best]) || scores[i] > scores[best]) best = i;
  }
  return best;
}

Timestamp event_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

}  // namespace

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
  run_pool(points.size(), threads, source_factory, [&](std::size_t i, MdSource* source) {
    const BacktestConfig cfg = point_config(base, points[i]);
    if (source) source->reset();
    results[i].result = run(cfg, source);
  });
  return results;
}

ScoreFn score_metric(std::string_view name) {
  if (name == "net_pnl") return [](const BacktestResult& r) { return r.metrics.net_pnl; };
  if (name == "realized_pnl") return [](const BacktestResult& r) { return r.metrics.realized_pnl; };
  if (name == "sharpe_bar") return [](const BacktestResult& r) { return r.metrics.sharpe_bar; };
  if (name == "spread_captured_bps")
    return [](const BacktestResult& r) { return r.metrics.spread_captured_bps; };
  throw std::invalid_argument("walk_forward: unknown metric '" + std::string(name) +
                              "' (net_pnl, realized_pnl, sharpe_bar, spread_captured_bps)");
}

std::string WalkForwardReport::label(const ParamMap& params) const {
  std::string out;
  for (const std::string& k : param_names) {
    const auto it = params.find(k);
    if (it == params.end()) continue;
    out.append(out.empty() ? "" : " ").append(k).append("=").append(it->second);
  }
  return out;
}

std::string WalkForwardReport::table() const {
  auto num = [](double v) {
    if (std::isnan(v)) return std::string("-");
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return std::string(buf);
  };
  const std::int64_t t0 = folds.empty() ? 0 : folds.front().start_ts;
  auto secs = [&](std::int64_t ts) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(ts - t0) * 1e-9);
    return std::string(buf);
  };
  std::string out = "walk-forward: " + std::to_string(folds.size()) + " folds, " +
                    std::to_string(folds.empty() ? 0 : folds.front().points.size()) +
                    " points, metric " + metric + ", chosen on the previous fold\n";
  char line[256];
  std::snprintf(line,
                sizeof line,
                "%4s  %19s  %6s  %12s  %13s  %6s  %12s\n",
                "fold",
                "time_s",
                "chosen",
                "in_sample",
                "out_of_sample",
                "best",
                "hindsight");
  out += line;
  std::vector<bool> shown;
  for (std::size_t i = 0; i < folds.size(); ++i) {
    const WalkForwardFold& f = folds[i];
    if (shown.size() < f.points.size()) shown.resize(f.points.size());
    const std::string span = secs(f.start_ts) + "-" + secs(f.end_ts);
    const std::string chosen = i == 0 ? "-" : "#" + std::to_string(f.chosen);
    std::snprintf(line,
                  sizeof line,
                  "%4zu  %19s  %6s  %12s  %13s  %6s  %12s\n",
                  i,
                  span.c_str(),
                  chosen.c_str(),
                  num(f.in_sample).c_str(),
                  num(f.out_of_sample).c_str(),
                  ("#" + std::to_string(f.best)).c_str(),
                  num(f.scores.empty() ? f.hindsight : f.scores[f.best]).c_str());
    out += line;
    shown[f.best] = true;
    if (i > 0) shown[f.chosen] = true;
  }
  const WalkForwardFold* any = folds.empty() ? nullptr : &folds.front();
  for (std::size_t p = 0; any != nullptr && p < shown.size(); ++p) {
    if (shown[p]) out += "  #" + std::to_string(p) + "  " + label(any->points[p].params) + "\n";
  }
  out += "mean in-sample best   " + num(mean_in_sample) + "\n";
  out += "mean out-of-sample    " + num(mean_out_of_sample) + "\n";
  out += "mean hindsight best   " + num(mean_hindsight) + "\n";
  const std::size_t transitions = folds.size() > 2 ? folds.size() - 2 : 0;
  out += "choice changes        " + std::to_string(choice_changes) + " of " +
         std::to_string(transitions) + "\n";
  return out;
}

WalkForwardReport walk_forward(const BacktestConfig& base,
                               const ParamGrid& grid,
                               const SourceFactory& source_factory,
                               const RunFn& run,
                               int folds,
                               std::string_view metric,
                               int threads) {
  if (folds < 1) throw std::invalid_argument("walk_forward: folds must be >= 1");
  const ScoreFn score = score_metric(metric);
  WalkForwardReport rep;
  rep.metric = std::string(metric);
  for (const auto& [k, values] : grid) {
    if (!values.empty()) rep.param_names.push_back(k);
  }
  const auto k = static_cast<std::size_t>(folds);
  rep.folds.resize(k);

  if (k == 1) {
    WalkForwardFold& f = rep.folds[0];
    f.points = sweep(base, grid, source_factory, run, threads);
    for (const SweepPoint& p : f.points) {
      if (f.start_ts == 0 || p.result.start_ts < f.start_ts) f.start_ts = p.result.start_ts;
      if (p.result.end_ts > f.end_ts) f.end_ts = p.result.end_ts;
    }
  } else {
    register_builtin_strategies();
    // The per-worker cursor that the folds slice: the caller's source, or the synthetic
    // market as a stream.
    std::unique_ptr<MdSource> probe = source_factory ? source_factory() : nullptr;
    SourceFactory factory = source_factory;
    if (probe == nullptr) {
      if (base.transport.fill_model != sim::FillModel::L2Queue) {
        throw std::invalid_argument(
            "walk_forward: folds > 1 on the synthetic market need fill_model l2_queue (the "
            "coupled matching market cannot be cut in time)");
      }
      factory = [&base]() -> std::unique_ptr<MdSource> {
        return std::make_unique<SyntheticSource>(synthetic_source_config(base));
      };
      probe = factory();
    }
    // Event-time range of the data, cut into k equal slices.
    Timestamp first{};
    Timestamp last{};
    for (const EventHeader* h = probe->next(); h != nullptr; h = probe->next()) {
      if (!first.valid()) first = event_time(*h);
      last = event_time(*h);
    }
    probe.reset();
    if (!first.valid()) throw std::invalid_argument("walk_forward: the data has no events");
    std::vector<Timestamp> edges(k + 1);
    const std::int64_t span = last.ns - first.ns + 1;
    const auto sk = static_cast<std::int64_t>(k);
    for (std::size_t i = 0; i <= k; ++i) {
      const auto si = static_cast<std::int64_t>(i);
      edges[i] = Timestamp{first.ns + span / sk * si + span % sk * si / sk};
    }
    const std::vector<ParamMap> points = expand_grid(grid);
    const std::size_t n = points.size();
    for (std::size_t i = 0; i < k; ++i) {
      rep.folds[i].start_ts = edges[i].ns;
      rep.folds[i].end_ts = edges[i + 1].ns;
      rep.folds[i].points.resize(n);
      for (std::size_t p = 0; p < n; ++p) rep.folds[i].points[p].params = points[p];
    }
    run_pool(k * n, threads, factory, [&](std::size_t t, MdSource* source) {
      const std::size_t fi = t / n;
      const std::size_t p = t % n;
      const BacktestConfig cfg = point_config(base, points[p]);
      if (source == nullptr) throw std::runtime_error("walk_forward: cannot open the data");
      source->reset();
      TimeSliceSource slice(*source, edges[fi], edges[fi + 1]);
      rep.folds[fi].points[p].result = run(cfg, &slice);
    });
  }

  double sum_in = 0.0;
  double sum_out = 0.0;
  double sum_hind = 0.0;
  for (std::size_t i = 0; i < k; ++i) {
    WalkForwardFold& f = rep.folds[i];
    f.scores.reserve(f.points.size());
    for (const SweepPoint& p : f.points) f.scores.push_back(score(p.result));
    if (f.scores.empty()) continue;
    f.best = argmax(f.scores);
    if (i == 0) continue;
    const WalkForwardFold& prev = rep.folds[i - 1];
    f.chosen = prev.best;
    f.in_sample = prev.scores[prev.best];
    f.out_of_sample = f.scores[f.chosen];
    f.hindsight = f.scores[f.best];
    sum_in += f.in_sample;
    sum_out += f.out_of_sample;
    sum_hind += f.hindsight;
    if (i >= 2 && f.chosen != prev.chosen) ++rep.choice_changes;
  }
  if (k > 1 && !rep.folds[0].scores.empty()) {
    const auto m = static_cast<double>(k - 1);
    rep.mean_in_sample = sum_in / m;
    rep.mean_out_of_sample = sum_out / m;
    rep.mean_hindsight = sum_hind / m;
  }
  return rep;
}

}  // namespace fastmm::bt
