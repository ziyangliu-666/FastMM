#include "fastmm/backtest/sweep.hpp"

#include "fastmm/backtest/registrations.hpp"
#include "fastmm/core/log.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fastmm::bt {

SweepPoints expand_grid(const ParamGrid& grid) {
  SweepPoints out{ParamMap{}};
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

// The keys of a list of points, for a report's labels.
std::vector<std::string> point_names(const SweepPoints& points) {
  std::vector<std::string> names;
  for (const ParamMap& p : points) {
    for (const auto& kv : p) {
      if (std::find(names.begin(), names.end(), kv.first) == names.end()) names.push_back(kv.first);
    }
  }
  return names;
}

std::vector<std::string> grid_names(const ParamGrid& grid) {
  std::vector<std::string> names;
  for (const auto& [k, values] : grid) {
    if (!values.empty()) names.push_back(k);
  }
  return names;
}

}  // namespace

BacktestConfig point_config(const BacktestConfig& start, const ParamMap& point) {
  BacktestConfig cfg = start;
  for (const auto& [k, v] : point) {
    if (!cfg.set_setting(k, v)) cfg.params[k] = v;
  }
  cfg.output_dir.clear();
  cfg.journal_out.clear();
  cfg.keep_orders = false;
  return cfg;
}

std::vector<SweepPoint> sweep(const BacktestConfig& base,
                              const ParamGrid& grid,
                              const SourceFactory& source_factory,
                              const RunFn& run,
                              int threads,
                              const ResultHook& done) {
  return sweep(base, expand_grid(grid), source_factory, run, threads, done);
}

std::vector<SweepPoint> sweep(const BacktestConfig& base,
                              const SweepPoints& points,
                              const SourceFactory& source_factory,
                              const RunFn& run,
                              int threads,
                              const ResultHook& done) {
  // The registry is not thread-safe for writers: register before any worker can look it up
  // (run_backtest by name registers too, which would otherwise race on the first sweep).
  register_builtin_strategies();
  std::vector<SweepPoint> results(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) results[i].params = points[i];
  const BacktestConfig start = shared_start(base);
  for (const ParamMap& p : points) static_cast<void>(point_config(start, p));  // bad values first
  std::mutex done_mutex;
  run_pool(points.size(), threads, source_factory, [&](std::size_t i, MdSource* source) {
    const BacktestConfig cfg = point_config(start, points[i]);
    if (source) source->reset();
    const LogRunScope log_run(i);
    results[i].result = run(cfg, source);
    if (done) {
      const std::lock_guard lock(done_mutex);
      done(i, results[i].result);
    }
  });
  return results;
}

ScoreFn score_metric(std::string_view name) {
  if (name == "net_pnl") return [](const BacktestResult& r) { return r.metrics.net_pnl; };
  if (name == "realized_pnl") return [](const BacktestResult& r) { return r.metrics.realized_pnl; };
  if (name == "sharpe_bar") return [](const BacktestResult& r) { return r.metrics.sharpe_bar; };
  if (name == "spread_captured_bps") {
    return [](const BacktestResult& r) { return r.metrics.spread_captured_bps; };
  }
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
  // A fold's first instant in the data's time, UTC to the millisecond.
  auto utc = [](std::int64_t ts) {
    const std::time_t s = ts / 1'000'000'000;
    std::tm t{};
    gmtime_r(&s, &t);
    char buf[48];
    std::snprintf(buf,
                  sizeof buf,
                  "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  t.tm_year + 1900,
                  t.tm_mon + 1,
                  t.tm_mday,
                  t.tm_hour,
                  t.tm_min,
                  t.tm_sec,
                  static_cast<int>(ts % 1'000'000'000 / 1'000'000));
    return std::string(buf);
  };
  std::string out = "walk-forward: " + std::to_string(folds.size()) + " folds, " +
                    std::to_string(folds.empty() ? 0 : folds.front().points.size()) +
                    " points, metric " + metric + ", chosen on the previous fold\n";
  char line[256];
  std::snprintf(line,
                sizeof line,
                "%4s  %-23s  %6s  %12s  %13s  %6s  %12s\n",
                "fold",
                "from (UTC)",
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
    const std::string chosen = i == 0 ? "-" : "#" + std::to_string(f.chosen);
    std::snprintf(line,
                  sizeof line,
                  "%4zu  %-23s  %6s  %12s  %13s  %6s  %12s\n",
                  i,
                  utc(f.start_ts).c_str(),
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

namespace {

// The synthetic market of `base` as a stream: what the folds cut when there is no data source.
SourceFactory synthetic_stream(const BacktestConfig& base) {
  if (base.transport.fill_model != sim::FillModel::L2Queue) {
    throw std::invalid_argument(
        "walk_forward: folds > 1 on the synthetic market need fill_model l2_queue (the "
        "coupled matching market cannot be cut in time)");
  }
  return [&base]() -> std::unique_ptr<MdSource> {
    return std::make_unique<SyntheticSource>(synthetic_source_config(base));
  };
}

// A TimeSliceSource that owns the cursor it cuts.
class OwnedSlice final : public MdSource {
 public:
  OwnedSlice(std::unique_ptr<MdSource> inner, Timestamp t0, Timestamp t1)
      : inner_(std::move(inner)), slice_(*inner_, t0, t1) {}
  const EventHeader* next() override { return slice_.next(); }
  void reset() override { slice_.reset(); }
  [[nodiscard]] Timestamp start_ts() const override { return slice_.start_ts(); }
  [[nodiscard]] bool order_by_order() const override { return slice_.order_by_order(); }

 private:
  std::unique_ptr<MdSource> inner_;
  TimeSliceSource slice_;
};

}  // namespace

std::vector<Timestamp> fold_edges(const BacktestConfig& base,
                                  const SourceFactory& source_factory,
                                  int folds) {
  if (folds < 2) throw std::invalid_argument("walk_forward: fold edges need folds >= 2");
  std::unique_ptr<MdSource> probe = source_factory ? source_factory() : nullptr;
  if (probe == nullptr) probe = synthetic_stream(base)();
  // Event-time range of the data, cut into k equal slices.
  Timestamp first{};
  Timestamp last{};
  for (const EventHeader* h = probe->next(); h != nullptr; h = probe->next()) {
    if (!first.valid()) first = event_time(*h);
    last = event_time(*h);
  }
  if (!first.valid()) throw std::invalid_argument("walk_forward: the data has no events");
  const auto k = static_cast<std::size_t>(folds);
  std::vector<Timestamp> edges(k + 1);
  const std::int64_t span = last.ns - first.ns + 1;
  const auto sk = static_cast<std::int64_t>(k);
  for (std::size_t i = 0; i <= k; ++i) {
    const auto si = static_cast<std::int64_t>(i);
    edges[i] = Timestamp{first.ns + span / sk * si + span % sk * si / sk};
  }
  return edges;
}

std::unique_ptr<MdSource> fold_source(const BacktestConfig& base,
                                      std::unique_ptr<MdSource> inner,
                                      Timestamp t0,
                                      Timestamp t1) {
  if (inner == nullptr) inner = synthetic_stream(base)();
  return std::make_unique<OwnedSlice>(std::move(inner), t0, t1);
}

void score_walk_forward(WalkForwardReport& rep, const ScoreFn& score_fn) {
  const ScoreFn score = score_fn ? score_fn : score_metric(rep.metric);
  rep.choice_changes = 0;
  const std::size_t k = rep.folds.size();
  if (k == 1) {
    WalkForwardFold& f = rep.folds[0];
    for (const SweepPoint& p : f.points) {
      if (f.start_ts == 0 || p.result.start_ts < f.start_ts) f.start_ts = p.result.start_ts;
      if (p.result.end_ts > f.end_ts) f.end_ts = p.result.end_ts;
    }
  }
  double sum_in = 0.0;
  double sum_out = 0.0;
  double sum_hind = 0.0;
  for (std::size_t i = 0; i < k; ++i) {
    WalkForwardFold& f = rep.folds[i];
    f.scores.clear();
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
}

WalkForwardReport walk_forward(const BacktestConfig& base,
                               const ParamGrid& grid,
                               const SourceFactory& source_factory,
                               const RunFn& run,
                               int folds,
                               std::string_view metric,
                               int threads) {
  WalkForwardReport rep =
      walk_forward(base, expand_grid(grid), source_factory, run, folds, metric, threads);
  rep.param_names = grid_names(grid);
  return rep;
}

WalkForwardReport walk_forward(const BacktestConfig& base,
                               const SweepPoints& points,
                               const SourceFactory& source_factory,
                               const RunFn& run,
                               int folds,
                               std::string_view metric,
                               int threads) {
  if (folds < 1) throw std::invalid_argument("walk_forward: folds must be >= 1");
  static_cast<void>(score_metric(metric));  // an unknown metric fails before anything runs
  WalkForwardReport rep;
  rep.metric = std::string(metric);
  rep.param_names = point_names(points);
  const auto k = static_cast<std::size_t>(folds);
  rep.folds.resize(k);

  if (k == 1) {
    rep.folds[0].points = sweep(base, points, source_factory, run, threads);
  } else {
    register_builtin_strategies();
    const std::vector<Timestamp> edges = fold_edges(base, source_factory, folds);
    // The per-worker cursor that the folds slice: the caller's source, or the synthetic
    // market as a stream.
    std::unique_ptr<MdSource> probe = source_factory ? source_factory() : nullptr;
    const SourceFactory factory = probe != nullptr ? source_factory : synthetic_stream(base);
    probe.reset();
    const std::size_t n = points.size();
    for (std::size_t i = 0; i < k; ++i) {
      rep.folds[i].start_ts = edges[i].ns;
      rep.folds[i].end_ts = edges[i + 1].ns;
      rep.folds[i].points.resize(n);
      for (std::size_t p = 0; p < n; ++p) rep.folds[i].points[p].params = points[p];
    }
    const BacktestConfig start = shared_start(base);
    for (const ParamMap& p : points) static_cast<void>(point_config(start, p));
    run_pool(k * n, threads, factory, [&](std::size_t t, MdSource* source) {
      const std::size_t fi = t / n;
      const std::size_t p = t % n;
      const BacktestConfig cfg = point_config(start, points[p]);
      if (source == nullptr) throw std::runtime_error("walk_forward: cannot open the data");
      source->reset();
      TimeSliceSource slice(*source, edges[fi], edges[fi + 1]);
      const LogRunScope log_run(p);
      rep.folds[fi].points[p].result = run(cfg, &slice);
    });
  }
  score_walk_forward(rep);
  return rep;
}

}  // namespace fastmm::bt
