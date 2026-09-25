#include "fastmm/backtest/backtest_runner.hpp"

#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/data_registry.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/pnl.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/core/journal.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

namespace fastmm::bt {

namespace {
constexpr std::size_t kReserveOrders = 1U << 15;
constexpr std::size_t kReserveFills = 1U << 14;
constexpr std::size_t kReserveBars = 1U << 12;
}  // namespace

// Venue-side collector: records orders and fills as the simulated venue sees them and
// samples equity bars. Vectors are reserved up front so a typical run does not reallocate.
struct BacktestSession::Impl final : sim::SimObserver {
  Impl(const InstrumentTable& instruments, std::size_t bars, const std::vector<Duration>& horizons)
      : ledger(instruments) {
    orders.reserve(kReserveOrders);
    fills.reserve(kReserveFills);
    equity.reserve(bars);
    for (Duration h : horizons) {
      if (h.ns > 0) fills.markout_horizon_ns.push_back(h.ns);
    }
    fills.markout_mid.resize(fills.markout_horizon_ns.size());
    next_.assign(fills.markout_horizon_ns.size(), 0);
  }

  void on_order_sent(const EventHeader& h, Timestamp ts, Timestamp venue_ts) override {
    orders.ts.push_back(ts.ns);
    orders.venue_ts.push_back(venue_ts.ns);
    orders.trigger_ts.push_back(static_cast<std::int64_t>(h.t0_cycles.v));
    orders.instrument.push_back(h.instrument.value);
    std::uint64_t id = 0;
    std::int8_t side = -1;
    std::int64_t px = 0;
    std::int64_t qty = 0;
    std::uint8_t kind = kOrderKindNew;
    std::uint8_t type = 0;
    switch (h.type) {
      case EventType::OutNewOrder: {
        const auto& m = msg_cast<OutNewOrderMsg>(&h);
        id = m.cl_ord_id.value;
        side = static_cast<std::int8_t>(m.side);
        px = m.price.raw;
        qty = m.qty.raw;
        type = static_cast<std::uint8_t>(m.type);
        break;
      }
      case EventType::OutCancel:
        id = msg_cast<OutCancelMsg>(&h).cl_ord_id.value;
        kind = kOrderKindCancel;
        break;
      case EventType::OutReplace: {
        const auto& m = msg_cast<OutReplaceMsg>(&h);
        id = m.cl_ord_id.value;
        px = m.price.raw;
        qty = m.qty.raw;
        kind = kOrderKindReplace;
        break;
      }
      default:
        break;
    }
    orders.cl_ord_id.push_back(id);
    orders.side.push_back(side);
    orders.price.push_back(px);
    orders.qty.push_back(qty);
    orders.kind.push_back(kind);
    orders.type.push_back(type);
  }
  void on_fill(const OrderFillMsg& f, Timestamp ts, const sim::FillContext& ctx) override {
    fills.ts.push_back(ts.ns);
    fills.instrument.push_back(f.hdr.instrument.value);
    fills.side.push_back(static_cast<std::int8_t>(f.side));
    fills.price.push_back(f.price.raw);
    fills.qty.push_back(f.qty.raw);
    fills.fee.push_back(f.fee.raw);
    fills.cl_ord_id.push_back(f.cl_ord_id.value);
    fills.liquidity.push_back(static_cast<std::uint8_t>(f.liquidity));
    fills.mid.push_back(ctx.mid.raw);
    fills.best_bid.push_back(ctx.best_bid.raw);
    fills.best_ask.push_back(ctx.best_ask.raw);
    fills.queue_ahead.push_back(ctx.queue_known ? ctx.queue_ahead.raw : -1);
    queue_position_known = queue_position_known || ctx.queue_known;
    for (std::vector<std::int64_t>& col : fills.markout_mid) col.push_back(0);
    ledger.on_fill(f);
  }
  void on_order_event(const EventHeader& h, Timestamp) override {
    if (h.type == EventType::OrderReject) ++rejects;
  }

  void sample(Timestamp ts,
              const sim::SimTransport& transport,
              const InstrumentTable& instruments) {
    for (const Instrument& i : instruments) ledger.mark(i.id, transport.venue_mid(i.id));
    const InstrumentId first{0};
    equity.ts.push_back(ts.ns);
    equity.realized.push_back(ledger.realized().raw);
    equity.unrealized.push_back(ledger.unrealized().raw);
    equity.fees.push_back(ledger.fees().raw);
    equity.position.push_back(ledger.net_position().raw);
    equity.mid.push_back(transport.venue_mid(first).raw);
    const auto bid = transport.strategy_exposure(first, Side::Buy);
    const auto ask = transport.strategy_exposure(first, Side::Sell);
    equity.quoted.push_back(
        static_cast<std::uint8_t>((bid.orders > 0 ? 1U : 0U) | (ask.orders > 0 ? 2U : 0U)));
  }

  // ---- markouts -----------------------------------------------------------------------------
  // Fills are recorded in time order, so the fills still waiting for horizon j form a queue:
  // next_[j] is the oldest one and its deadline is the earliest of that horizon.
  [[nodiscard]] Timestamp next_markout_deadline() const noexcept {
    Timestamp best = Timestamp::max();
    for (std::size_t j = 0; j < next_.size(); ++j) {
      if (next_[j] >= fills.size()) continue;
      const Timestamp due{fills.ts[next_[j]] + fills.markout_horizon_ns[j]};
      if (due < best) best = due;
    }
    return best;
  }
  // Records the venue mid of every fill whose horizon has been reached at `now`. The simulated
  // clock is stopped there, so the mid is the one at the horizon, not an interpolation.
  void resolve_markouts(Timestamp now, const sim::SimTransport& transport) {
    for (std::size_t j = 0; j < next_.size(); ++j) {
      const std::int64_t h = fills.markout_horizon_ns[j];
      while (next_[j] < fills.size() && fills.ts[next_[j]] + h <= now.ns) {
        const std::size_t i = next_[j]++;
        const InstrumentId id{fills.instrument[i]};
        fills.markout_mid[j][i] = transport.venue_mid(id).raw;
      }
    }
  }
  // Shortest horizon: the run loop never advances further than this without stopping, so a fill
  // can never pass its own deadline unnoticed.
  [[nodiscard]] Duration markout_step() const noexcept {
    std::int64_t ns = 0;
    for (std::int64_t h : fills.markout_horizon_ns) ns = ns == 0 ? h : (h < ns ? h : ns);
    return Duration{ns};
  }

  PnLLedger ledger;
  FillRows fills;
  OrderRows orders;
  EquityRows equity;
  std::vector<std::size_t> next_;  // per horizon, the first unresolved fill
  std::uint64_t rejects = 0;
  bool queue_position_known = false;
  std::unique_ptr<MsgRing> journal_ring;
  std::unique_ptr<JournalFileWriter> journal_file;
};

SyntheticSourceConfig synthetic_source_config(const BacktestConfig& cfg) {
  SyntheticSourceConfig sc;
  sc.generator = cfg.generator;
  sc.md = cfg.transport.md;
  sc.seed = cfg.seed;
  sc.venue = cfg.transport.venue;
  sc.start = cfg.start;
  sc.duration = cfg.duration;
  sc.seed_levels = cfg.generator_seed_levels;
  return sc;
}

BacktestSession::BacktestSession(const BacktestConfig& cfg,
                                 MdSource* source,
                                 const ParamSchema* schema,
                                 std::string_view strategy_meta)
    : cfg_(cfg), source_(source) {
  if (cfg_.instruments.size() == 0) throw std::invalid_argument("backtest: no instruments");
  if (cfg_.equity_bar.ns <= 0) throw std::invalid_argument("backtest: equity_bar must be > 0");
  if (source_ == nullptr) {  // synthetic market
    if (cfg_.transport.fill_model == sim::FillModel::Matching) {
      generator_ = std::make_unique<sim::MarketGenerator>(
          cfg_.generator, cfg_.seed, InstrumentId{0}, cfg_.start, cfg_.start + cfg_.duration);
    } else {
      synthetic_ = std::make_unique<SyntheticSource>(synthetic_source_config(cfg_));
      source_ = synthetic_.get();
    }
  }
  Timestamp start = cfg_.start;
  if (source_ != nullptr && source_->start_ts().valid()) start = source_->start_ts();
  backend_ = std::make_unique<sim::SimBackend>(cfg_.instruments, cfg_.transport, start);
  const std::size_t bars = source_ == synthetic_.get() || generator_ != nullptr
                               ? static_cast<std::size_t>(cfg_.duration.ns / cfg_.equity_bar.ns) + 4
                               : kReserveBars;
  impl_ = std::make_unique<Impl>(cfg_.instruments, bars, cfg_.markout_horizons);
  backend_->transport.set_observer(impl_.get());
  if (!cfg_.journal_out.empty()) {
    impl_->journal_ring = std::make_unique<MsgRing>(16U << 20);
    JournalSessionInfo info;
    info.session_id = cfg_.seed;
    info.start_ts = start;
    info.rng_seed = cfg_.engine.rng_seed;
    info.strategy = cfg_.strategy;
    info.instruments = &cfg_.instruments;
    info.has_session = true;
    info.session_epoch = cfg_.engine.session_epoch;
    info.quoting_enabled = cfg_.engine.quoting_enabled;
    info.replace_venues = cfg_.transport.supports_replace ? ~std::uint64_t{0} : 0;
    info.config_toml = cfg_.config_toml;
    info.params = schema;
    info.strategy_meta = strategy_meta;
    if (!cfg_.config_toml.empty()) info.config_hash = Config::text_hash(cfg_.config_toml);
    impl_->journal_file =
        std::make_unique<JournalFileWriter>(*impl_->journal_ring, cfg_.journal_out, info);
    if (!impl_->journal_file->ok())
      throw std::runtime_error("backtest: cannot create journal " + cfg_.journal_out);
  }
  deps_.engine = cfg_.engine;
  deps_.instruments = &cfg_.instruments;
  deps_.params = cfg_.params;
  deps_.journal_ring = impl_->journal_ring.get();
  deps_.backend = backend_.get();
}

BacktestSession::~BacktestSession() {
  if (impl_ && impl_->journal_file) impl_->journal_file->stop();
}

BacktestResult BacktestSession::run(const sim::EngineHooks& hooks,
                                    const IEngineRunner* runner,
                                    std::string_view strategy_name) {
  if (!hooks.valid()) throw std::invalid_argument("backtest: engine hooks not bound");
  const auto wall0 = std::chrono::steady_clock::now();
  sim::SimBackend& b = *backend_;
  sim::SimDriver driver(b.clock, b.transport, b.feed, hooks);
  driver.set_measure_wall_clock(cfg_.measure_wall_clock);
  if (generator_ != nullptr) driver.set_generator(generator_.get(), cfg_.generator_seed_levels);
  if (source_ != nullptr) driver.set_source(source_);
  if (impl_->journal_file) driver.set_journal_writer(impl_->journal_file.get());
  driver.set_param_schedule(params_);
  driver.set_slow_hooks(slow_);
  driver.start();
  const Timestamp start = b.clock.now();
  Timestamp bar_end = start + cfg_.equity_bar;
  // The loop stops at every equity bar and at every markout horizon that has come due, and never
  // runs longer than the shortest horizon at a time, so a fill made inside one step still has its
  // own horizon ahead of the next stop. run_until() is purely time-bounded, so splitting an
  // interval processes exactly the same events in the same order: the run stays deterministic.
  const Duration step = impl_->markout_step();
  Timestamp reached = start;
  for (;;) {
    Timestamp stop = bar_end;
    if (const Timestamp due = impl_->next_markout_deadline(); due < stop) stop = due;
    if (step.ns > 0 && reached + step < stop) stop = reached + step;
    const bool more = driver.run_until(stop);
    if (!more) {
      const Timestamp end = b.clock.now();
      impl_->resolve_markouts(end, b.transport);
      impl_->sample(end, b.transport, cfg_.instruments);
      break;
    }
    impl_->resolve_markouts(stop, b.transport);
    reached = stop;
    if (stop == bar_end) {
      impl_->sample(bar_end, b.transport, cfg_.instruments);
      bar_end += cfg_.equity_bar;
    }
  }
  driver.finish();
  if (impl_->journal_file) {
    impl_->journal_file->stop();
    impl_->journal_file.reset();
  }

  BacktestResult r;
  r.strategy = std::string(strategy_name);
  r.params = cfg_.params;
  r.seed = cfg_.seed;
  r.fills = std::move(impl_->fills);
  r.orders = std::move(impl_->orders);
  r.equity = std::move(impl_->equity);
  if (runner != nullptr) r.engine = runner->stats();
  r.transport = b.transport.stats();
  r.outbound_sha256 = b.transport.outbound_hash().hex();
  r.outbound_messages = b.transport.outbound_hash().count();
  r.md_events = driver.stats().md_delivered;
  r.engine_steps = driver.stats().engine_steps;
  r.start_ts = start.ns;
  r.end_ts = b.clock.now().ns;
  MetricsInputs in;
  in.bar = cfg_.equity_bar;
  in.initial_capital = cfg_.initial_capital;
  in.rejects = impl_->rejects;
  in.wall_p50_ns = driver.stats().md_step_ns.percentile(0.5);
  in.wall_p99_ns = driver.stats().md_step_ns.percentile(0.99);
  in.queue_position_known = impl_->queue_position_known;
  in.end_ts = r.end_ts;
  in.final_mid.resize(cfg_.instruments.size());
  for (const Instrument& i : cfg_.instruments)
    in.final_mid[i.id.value] = b.transport.venue_mid(i.id).raw;
  r.metrics = compute_metrics(r.equity, r.fills, r.orders, in);
  r.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
  return r;
}

BacktestResult run_backtest(const BacktestConfig& cfg,
                            std::string_view strategy,
                            MdSource* source) {
  register_builtin_strategies();
  const StrategyEntry* entry = StrategyRegistry::instance().find(strategy);
  if (entry == nullptr)
    throw std::invalid_argument("backtest: unknown strategy '" + std::string(strategy) + "'");
  BacktestSession session(cfg, source, entry->schema);
  std::unique_ptr<IEngineRunner> runner =
      StrategyRegistry::instance().make(strategy, TransportKind::Sim, session.deps());
  if (runner == nullptr) {
    throw std::invalid_argument("backtest: strategy '" + std::string(strategy) +
                                "' has no simulator factory");
  }
  return session.run(session.backend().hooks, runner.get(), runner->strategy_name());
}

namespace {
bool ends_with(std::string_view s, std::string_view suffix) noexcept {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
}  // namespace

std::unique_ptr<MdSource> open_data(std::string_view spec, const InstrumentTable* instruments) {
  if (spec.empty() || spec == "synthetic") return nullptr;
  register_builtin_data_sources();
  const std::string_view head = spec.substr(0, spec.find(':'));
  const DataSourceEntry* entry = DataSourceRegistry::instance().find(head);
  if (entry != nullptr) {
    DataSourceOptions opts = DataSourceOptions::parse(spec, entry->positional);
    opts.set_instruments(instruments);
    return entry->open(opts);
  }
  // A bare path: infer the source from the extension.
  const std::string path(spec);
  if (ends_with(spec, ".fmj")) return std::make_unique<JournalSource>(path);
  if (ends_with(spec, ".csv")) return std::make_unique<CsvSource>(path);
  std::string known;
  for (const DataSourceEntry& e : DataSourceRegistry::instance().entries())
    known.append(known.empty() ? "" : ", ").append(e.name);
  throw std::runtime_error("backtest: '" + path +
                           "' is neither *.fmj, *.csv nor <source>:<args> (" + known + ")");
}

std::unique_ptr<MdSource> open_source(const BacktestConfig& cfg) {
  if (cfg.source.empty()) return open_data(cfg.path, &cfg.instruments);
  // [backtest] source holds a whole spec; the older `source` + `path` pair is the same thing
  // with the path as the source's one positional argument.
  std::string spec = cfg.source;
  if (spec.find(':') == std::string::npos && !cfg.path.empty()) spec += ":" + cfg.path;
  return open_data(spec, &cfg.instruments);
}

}  // namespace fastmm::bt
