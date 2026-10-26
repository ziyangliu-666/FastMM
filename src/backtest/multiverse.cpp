#include "fastmm/backtest/multiverse.hpp"

#include "fastmm/backtest/registrations.hpp"
#include "fastmm/backtest/synthetic_source.hpp"
#include "fastmm/sim/venue_order.hpp"
#include "fastmm/strategies/registry.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace fastmm::bt {

namespace {

// Runs one pass holds at most: each keeps its own engine and simulated venue.
constexpr std::size_t kMaxRunsPerPass = 8;

Timestamp event_time(const EventHeader& h) noexcept {
  return h.exch_ts.valid() ? h.exch_ts : h.recv_ts;
}

void check_shareable(const std::vector<BacktestConfig>& cfgs) {
  const BacktestConfig& first = cfgs.front();
  for (const BacktestConfig& c : cfgs) {
    if (c.transport.fill_model != sim::FillModel::L2Queue)
      throw std::invalid_argument("multiverse: needs fill_model = \"l2_queue\"");
    if (c.instruments.size() != first.instruments.size())
      throw std::invalid_argument("multiverse: the runs must share one instrument table");
    for (std::size_t i = 0; i < c.instruments.size(); ++i) {
      const InstrumentId id{static_cast<std::uint32_t>(i)};
      if (c.instruments.get(id).symbol != first.instruments.get(id).symbol ||
          c.instruments.get(id).venue != first.instruments.get(id).venue)
        throw std::invalid_argument("multiverse: the runs must share one instrument table");
    }
    if (c.reorder_window != first.reorder_window)
      throw std::invalid_argument("multiverse: the runs must share one reorder_window");
  }
}

// One group: its runs in lockstep over one cursor.
void run_group(const std::vector<BacktestConfig>& cfgs,
               std::span<const std::size_t> which,
               const StrategyEntry& entry,
               MdSource& source,
               std::vector<BacktestResult>& out) {
  sim::SimWorld world(cfgs[which.front()].instruments.size());
  struct Run {
    std::unique_ptr<BacktestSession> session;
    std::unique_ptr<IEngineRunner> runner;
    std::size_t index;
    bool live = true;
  };
  std::vector<Run> runs;
  runs.reserve(which.size());
  for (const std::size_t i : which) {
    const BacktestConfig& cfg = cfgs[i];
    Run r;
    r.index = i;
    r.session = std::make_unique<BacktestSession>(cfg, &source, entry.schema);
    if (cfg.ab.enabled()) r.session->enable_ab(entry.publisher);
    r.runner = StrategyRegistry::instance().make(entry.name, TransportKind::Sim, r.session->deps());
    if (r.runner == nullptr) {
      throw std::invalid_argument("multiverse: strategy '" + std::string(entry.name) +
                                  "' has no simulator factory");
    }
    r.session->backend().transport.attach_world(world);
    runs.push_back(std::move(r));
  }
  for (Run& r : runs) r.session->begin(r.session->backend().hooks, r.runner.get(), true);

  const Duration window = cfgs[which.front()].reorder_window;
  std::unique_ptr<sim::VenueOrderSource> ordered;
  MdSource* events = &source;
  if (window.ns > 0) {
    ordered = std::make_unique<sim::VenueOrderSource>(source, window);
    events = ordered.get();
  }
  std::size_t live = runs.size();
  while (live > 0) {
    const EventHeader* ev = events->next();
    if (ev == nullptr) break;
    const Timestamp t = event_time(*ev);
    for (Run& r : runs) {
      if (r.live && !r.session->advance_before(t)) {
        r.live = false;
        --live;
      }
    }
    world.before(*ev, t);
    for (Run& r : runs) {
      if (r.live) r.session->world_event(*ev);
    }
    world.after(*ev, t);
  }
  for (Run& r : runs) {
    BacktestResult res = r.session->end(r.runner->strategy_name());
    if (ordered) {
      res.md_reordered = ordered->reordered();
      res.md_late = ordered->late();
    }
    out[r.index] = std::move(res);
  }
}

}  // namespace

std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                           std::string_view strategy,
                                           MdSource& source) {
  std::vector<BacktestResult> out(cfgs.size());
  if (cfgs.empty()) return out;
  check_shareable(cfgs);
  register_builtin_strategies();
  const StrategyEntry* entry = StrategyRegistry::instance().find(strategy);
  if (entry == nullptr)
    throw std::invalid_argument("multiverse: unknown strategy '" + std::string(strategy) + "'");
  std::vector<std::size_t> all(cfgs.size());
  for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;
  run_group(cfgs, all, *entry, source, out);
  return out;
}

std::vector<BacktestResult> run_multiverse(const std::vector<BacktestConfig>& cfgs,
                                           std::string_view strategy,
                                           const SourceFactory& source_factory,
                                           int threads) {
  std::vector<BacktestResult> out(cfgs.size());
  if (cfgs.empty()) return out;
  check_shareable(cfgs);
  register_builtin_strategies();
  const StrategyEntry* entry = StrategyRegistry::instance().find(strategy);
  if (entry == nullptr)
    throw std::invalid_argument("multiverse: unknown strategy '" + std::string(strategy) + "'");
  if (!source_factory) throw std::invalid_argument("multiverse: needs recorded data");

  // A pass takes at most kMaxRunsPerPass runs, so the runs a worker holds at once stay few; group
  // g takes runs g, g + G, g + 2G, ... of the G groups, so neighbouring grid points, which cost
  // alike, spread over the workers.
  unsigned w = threads > 0 ? static_cast<unsigned>(threads) : std::thread::hardware_concurrency();
  if (w == 0) w = 1;
  if (w > 32) w = 32;  // Logger supports a bounded number of threads
  std::size_t g = std::max<std::size_t>(w, (cfgs.size() + kMaxRunsPerPass - 1) / kMaxRunsPerPass);
  if (g > cfgs.size()) g = cfgs.size();
  if (w > g) w = static_cast<unsigned>(g);
  std::vector<std::vector<std::size_t>> groups(g);
  for (std::size_t i = 0; i < cfgs.size(); ++i) groups[i % g].push_back(i);

  const auto run_one = [&](std::size_t k) {
    std::unique_ptr<MdSource> source = source_factory();
    if (source == nullptr) throw std::invalid_argument("multiverse: needs recorded data");
    run_group(cfgs, groups[k], *entry, *source, out);
  };
  if (w == 1) {
    for (std::size_t k = 0; k < g; ++k) run_one(k);
    return out;
  }
  std::atomic<std::size_t> next{0};
  std::exception_ptr error;
  std::atomic<bool> failed{false};
  {
    std::vector<std::jthread> pool;
    pool.reserve(w);
    for (unsigned t = 0; t < w; ++t) {
      pool.emplace_back([&] {
        for (;;) {
          const std::size_t k = next.fetch_add(1, std::memory_order_relaxed);
          if (k >= g || failed.load(std::memory_order_relaxed)) return;
          try {
            run_one(k);
          } catch (...) {
            if (!failed.exchange(true)) error = std::current_exception();
            return;
          }
        }
      });
    }
  }
  if (error) std::rethrow_exception(error);
  return out;
}

std::vector<SweepPoint> sweep_multiverse(const BacktestConfig& base,
                                         std::string_view strategy,
                                         const ParamGrid& grid,
                                         const SourceFactory& source_factory,
                                         int threads) {
  const std::vector<ParamMap> points = expand_grid(grid);
  const BacktestConfig start = shared_start(base);
  std::vector<BacktestConfig> cfgs;
  cfgs.reserve(points.size());
  for (const ParamMap& p : points) {
    BacktestConfig c = start;
    for (const auto& [k, v] : p) c.params[k] = v;
    c.output_dir.clear();
    c.journal_out.clear();
    cfgs.push_back(std::move(c));
  }
  // Without data, the synthetic market as a stream (l2_queue), the one each point would read.
  const SourceFactory factory = [&]() -> std::unique_ptr<MdSource> {
    std::unique_ptr<MdSource> s = source_factory ? source_factory() : nullptr;
    if (s == nullptr) s = std::make_unique<SyntheticSource>(synthetic_source_config(base));
    return s;
  };
  std::vector<BacktestResult> results = run_multiverse(cfgs, strategy, factory, threads);
  std::vector<SweepPoint> out(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    out[i].params = points[i];
    out[i].result = std::move(results[i]);
  }
  return out;
}

std::vector<SweepPoint> sweep_by_name(const BacktestConfig& base,
                                      std::string strategy,
                                      const ParamGrid& grid,
                                      const SourceFactory& source_factory,
                                      int threads,
                                      bool separately) {
  if (!separately && base.transport.fill_model == sim::FillModel::L2Queue)
    return sweep_multiverse(base, strategy, grid, source_factory, threads);
  return sweep(
      base,
      grid,
      source_factory,
      [strategy = std::move(strategy)](const BacktestConfig& c, MdSource* s) {
        return run_backtest(c, strategy, s);
      },
      threads);
}

WalkForwardReport walk_forward_by_name(const BacktestConfig& base,
                                       std::string strategy,
                                       const ParamGrid& grid,
                                       const SourceFactory& source_factory,
                                       int folds,
                                       std::string_view metric,
                                       int threads) {
  if (base.transport.fill_model != sim::FillModel::L2Queue) {
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
  if (folds < 1) throw std::invalid_argument("walk_forward: folds must be >= 1");
  static_cast<void>(score_metric(metric));  // an unknown metric fails before anything runs
  WalkForwardReport rep;
  rep.metric = std::string(metric);
  for (const auto& [k, values] : grid) {
    if (!values.empty()) rep.param_names.push_back(k);
  }
  const auto k = static_cast<std::size_t>(folds);
  rep.folds.resize(k);
  if (k == 1) {
    rep.folds[0].points = sweep_multiverse(base, strategy, grid, source_factory, threads);
  } else {
    const std::vector<Timestamp> edges = fold_edges(base, source_factory, folds);
    for (std::size_t i = 0; i < k; ++i) {
      // Each fold starts on the book as of its first instant with a flat engine, as alone.
      const SourceFactory slice = [&]() -> std::unique_ptr<MdSource> {
        return fold_source(
            base, source_factory ? source_factory() : nullptr, edges[i], edges[i + 1]);
      };
      rep.folds[i].start_ts = edges[i].ns;
      rep.folds[i].end_ts = edges[i + 1].ns;
      rep.folds[i].points = sweep_multiverse(base, strategy, grid, slice, threads);
    }
  }
  score_walk_forward(rep);
  return rep;
}

}  // namespace fastmm::bt
