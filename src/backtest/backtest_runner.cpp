#include "fastmm/backtest/backtest_runner.hpp"

#include "fastmm/backtest/csv_source.hpp"
#include "fastmm/backtest/data_registry.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/backtest/journal_state.hpp"
#include "fastmm/backtest/pnl.hpp"
#include "fastmm/backtest/registrations.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/session_state.hpp"
#include "fastmm/sim/recorded_session.hpp"
#include "fastmm/sim/sim_treasury.hpp"
#include "fastmm/sim/venue_order.hpp"
#include "fastmm/strategies/ab_test.hpp"
#include "fastmm/strategies/param_overrides.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace fastmm::bt {

namespace {
constexpr std::size_t kReserveOrders = 1U << 15;
constexpr std::size_t kReserveFills = 1U << 14;
constexpr std::size_t kReserveBars = 1U << 12;

// What a run does at simulated times besides the engine's events, as a slow tier of the driver:
//   * the pool treasuries ([venues.<primary>.treasury]): at each look they read the pools'
//     simulated accounts (the venue's free amounts) and step, and their transfers are carried out
//     by a SimTreasuryPort at simulated times;
//   * the start of trading ([backtest] start_at): the strategy restores its checkpoint as at a warm
//     standby's take-over (ControlCommand::TakeOver), then every venue an order can go to
//     reconciles with the start positions, and the engine trades from there;
//   * with balances_from_journal, the session's account events at their times: each recorded flow
//     of money in or out of an account (AccountFlowMsg), and a full snapshot of an account when the
//     session's connector refreshed it;
//   * a slow tier the caller set (a Python strategy's), at its own times.
struct RunTier {
  sim::SimTransport* transport = nullptr;
  InlineFeed* feed = nullptr;
  std::vector<std::unique_ptr<Treasury>> treasuries;
  std::unique_ptr<sim::SimTreasuryPort> port;
  Duration interval{std::numeric_limits<std::int64_t>::max()};
  Timestamp next_look = Timestamp::max();
  std::vector<TreasuryBalance> rows;
  Timestamp trade_at = Timestamp::max();
  std::uint32_t trade_venues = 0;
  bool take_over = false;
  std::vector<ReconcileMsg> positions;
  std::span<const AccountFlowMsg> flows;  // in time order
  std::size_t next_flow = 0;
  std::uint64_t flows_short = 0;  // the account had less free than left it live
  std::span<const std::pair<VenueId, Timestamp>> refreshes;
  std::size_t next_refresh = 0;
  std::span<const std::vector<std::uint64_t>> controls;  // in time order
  std::size_t next_control = 0;
  sim::SlowHooks user{};
  Timestamp user_next = Timestamp::max();

  void look(Timestamp now) {
    const sim::SimAccounts* accounts = transport->accounts();
    for (const std::unique_ptr<Treasury>& t : treasuries) {
      const TreasuryConfig& c = t->config();
      rows.clear();
      for (const VenueId v : c.members) {
        TreasuryBalance b;
        b.venue = v;
        b.asset.assign(c.asset);
        b.known = accounts != nullptr && accounts->enabled(v);
        if (b.known) b.free = accounts->free(v, c.asset);
        b.as_of_ns = now.ns;
        rows.push_back(b);
      }
      t->step(now.ns, rows, *port);
    }
  }

  void start_trading(Timestamp now) {
    if (take_over) {
      ControlMsg m{};
      init_header(m, EventType::Control);
      m.command = ControlCommand::TakeOver;
      m.hdr.recv_ts = now;
      if (!feed->push(m.hdr)) throw std::runtime_error("backtest: the engine's feed is full");
    }
    transport->publish_reconcile(now, trade_venues, positions);
    trade_at = Timestamp::max();
  }

  void account_events(Timestamp now) {
    for (; next_flow < flows.size() && sim::account_time(flows[next_flow].hdr) <= now;
         ++next_flow) {
      const AccountFlowMsg& f = flows[next_flow];
      if (transport->move_money(f.hdr.venue, f.asset.view(), f.amount, now) != f.amount)
        ++flows_short;
    }
    for (; next_refresh < refreshes.size() && refreshes[next_refresh].second <= now; ++next_refresh)
      transport->publish_snapshot(refreshes[next_refresh].first, now);
    for (; next_control < controls.size() && control_time(controls[next_control]) <= now;
         ++next_control) {
      const auto& head = *reinterpret_cast<const EventHeader*>(controls[next_control].data());
      if (!feed->push(head)) throw std::runtime_error("backtest: the engine's feed is full");
    }
  }

  [[nodiscard]] static Timestamp control_time(const std::vector<std::uint64_t>& bytes) noexcept {
    return reinterpret_cast<const EventHeader*>(bytes.data())->recv_ts;
  }

  [[nodiscard]] Timestamp next() const noexcept {
    const Timestamp flow =
        next_flow < flows.size() ? sim::account_time(flows[next_flow].hdr) : Timestamp::max();
    const Timestamp refresh =
        next_refresh < refreshes.size() ? refreshes[next_refresh].second : Timestamp::max();
    const Timestamp control =
        next_control < controls.size() ? control_time(controls[next_control]) : Timestamp::max();
    return std::min({user_next,
                     next_look,
                     port ? port->next_due() : Timestamp::max(),
                     trade_at,
                     flow,
                     refresh,
                     control});
  }

  static Timestamp run(void* ctx, Timestamp now) {
    auto& tier = *static_cast<RunTier*>(ctx);
    if (now >= tier.trade_at) tier.start_trading(now);
    tier.account_events(now);
    if (tier.port) tier.port->settle(now);
    if (tier.user.run != nullptr && now >= tier.user_next)
      tier.user_next = tier.user.run(tier.user.ctx, now);
    if (now >= tier.next_look) {
      tier.look(now);
      tier.next_look = now + tier.interval;
    }
    return tier.next();
  }
};
}  // namespace

// Venue-side collector: records orders and fills as the simulated venue sees them and
// samples equity bars. Vectors are reserved up front so a typical run does not reallocate.
struct BacktestSession::Impl final : sim::SimObserver {
  Impl(const InstrumentTable& instruments,
       std::size_t bars,
       const std::vector<Duration>& horizons,
       bool keep_orders_)
      : ledger(instruments), keep_orders(keep_orders_) {
    if (keep_orders) orders.reserve(kReserveOrders);
    fills.reserve(kReserveFills);
    equity.by_instrument.resize(instruments.size());
    equity.reserve(bars);
    for (Duration h : horizons) {
      if (h.ns > 0) fills.markout_horizon_ns.push_back(h.ns);
    }
    fills.markout_mid.resize(fills.markout_horizon_ns.size());
    next_.assign(fills.markout_horizon_ns.size(), 0);
  }

  void on_order_sent(const EventHeader& h, Timestamp ts, Timestamp venue_ts) override {
    const auto trigger = static_cast<std::int64_t>(h.t0_cycles.v);
    std::uint64_t id = 0;
    std::uint64_t orig = 0;
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
        orig = m.orig_cl_ord_id.value;
        px = m.price.raw;
        qty = m.qty.raw;
        kind = kOrderKindReplace;
        break;
      }
      default:
        break;
    }
    order_stats.on_order(kind, id, orig, venue_ts.ns, trigger);
    if (!keep_orders) return;
    orders.ts.push_back(ts.ns);
    orders.venue_ts.push_back(venue_ts.ns);
    orders.trigger_ts.push_back(trigger);
    orders.instrument.push_back(h.instrument.value);
    orders.cl_ord_id.push_back(id);
    orders.orig_cl_ord_id.push_back(orig);
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
    order_stats.on_fill(f.cl_ord_id.value, ts.ns);
    if (!f.leaves_qty.is_positive()) order_stats.on_end(f.cl_ord_id.value);
  }
  void on_order_event(const EventHeader& h, Timestamp) override {
    switch (h.type) {
      case EventType::OrderReject:
        ++rejects;
        order_stats.on_end(msg_cast<OrderRejectMsg>(&h).cl_ord_id.value);
        break;
      case EventType::OrderCancelAck:
        order_stats.on_end(msg_cast<OrderCancelAckMsg>(&h).cl_ord_id.value);
        break;
      case EventType::OrderExpired:
        order_stats.on_end(msg_cast<OrderExpiredMsg>(&h).cl_ord_id.value);
        break;
      default:
        break;
    }
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
    equity.quoted.push_back(quoted(transport, first));
    for (const Instrument& i : instruments) {
      InstrumentEquityRows& r = equity.by_instrument[i.id.value];
      const Position& p = ledger.position(i.id);
      r.pnl.push_back(p.net_pnl().raw);
      r.position.push_back(p.qty.raw);
      r.mid.push_back(transport.venue_mid(i.id).raw);
      r.quoted.push_back(quoted(transport, i.id));
    }
  }
  [[nodiscard]] static std::uint8_t quoted(const sim::SimTransport& transport, InstrumentId id) {
    const auto bid = transport.strategy_exposure(id, Side::Buy);
    const auto ask = transport.strategy_exposure(id, Side::Sell);
    return static_cast<std::uint8_t>((bid.orders > 0 ? 1U : 0U) | (ask.orders > 0 ? 2U : 0U));
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
  bool keep_orders;
  FillRows fills;
  OrderRows orders;
  OrderStats order_stats;
  EquityRows equity;
  std::vector<std::size_t> next_;  // per horizon, the first unresolved fill
  std::uint64_t rejects = 0;
  bool queue_position_known = false;
  std::unique_ptr<MsgRing> journal_ring;
  std::unique_ptr<JournalFileWriter> journal_file;
};

BacktestConfig shared_start(BacktestConfig cfg) {
  if (cfg.initial_state == InitialState::StateFile && !cfg.engine.state_file.empty()) {
    auto loaded = read_strategy_state(
        cfg.engine.state_file, cfg.engine.initial_state, cfg.engine.initial_state_version);
    if (!loaded) throw std::invalid_argument("backtest: " + loaded.error());
  }
  cfg.engine.state_file.clear();
  return cfg;
}

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
    : cfg_(cfg), source_(source), schema_(schema) {
  if (cfg_.instruments.size() == 0) throw std::invalid_argument("backtest: no instruments");
  if (cfg_.initial_state == InitialState::StateFile && !cfg_.engine.state_file.empty()) {
    auto loaded = read_strategy_state(
        cfg_.engine.state_file, cfg_.engine.initial_state, cfg_.engine.initial_state_version);
    if (!loaded) throw std::invalid_argument("backtest: " + loaded.error());
    state_what_ = "the state file " + cfg_.engine.state_file;
  }
  if (cfg_.initial_state == InitialState::None) cfg_.engine.initial_state.clear();
  const sim::RecordedSession* recorded = source != nullptr ? source->recorded() : nullptr;
  if (cfg_.initial_state == InitialState::Journal) {
    if (recorded == nullptr) {
      throw std::invalid_argument(
          "backtest: initial_state = \"journal\" needs a journal as the data source");
    }
    const sim::SessionStart s = recorded->start(cfg_.start_at);
    if (cfg_.start_at.valid() && !s.has_state) {
      throw std::invalid_argument(
          "backtest: start_at with initial_state = \"journal\": the journal has no checkpoint at "
          "or before start_at (record the session with [engine] checkpoint_s)");
    }
    if (cfg_.start_at.valid()) {
      // Restored when trading starts, as a warm standby's take-over does (RunTier).
      cfg_.engine.initial_state.clear();
      staged_state_ = s.strategy;
      staged_version_ = s.version;
      trade_at_ = s.at;
      state_what_ = journal_state_at(s.at);
      staged_empty_ = s.has_state && s.strategy.empty();
    } else {
      cfg_.engine.initial_state = s.strategy;
      cfg_.engine.initial_state_version = s.version;
      state_what_ = "the journal's state at its start";
    }
    start_positions_ = s.positions;
    start_note_ = s.note;
    FASTMM_LOG_INFO("backtest: {}", start_note_);
  }
  if (cfg_.start_at.valid()) {
    if (!trade_at_.valid()) trade_at_ = cfg_.start_at;
    // Every venue an order can go to reconciles when trading starts; until then the engine refuses
    // orders (EngineConfig::await_reconcile), as before a live session's first reconciliation.
    for (std::uint32_t v = 0; v < PoolPlan::kVenues; ++v) {
      const VenueId primary = cfg_.transport.pools.primary(VenueId{static_cast<std::uint8_t>(v)});
      if (std::any_of(cfg_.instruments.begin(), cfg_.instruments.end(), [&](const Instrument& i) {
            return i.venue == primary;
          }))
        trade_venues_ |= 1U << v;
    }
    FASTMM_LOG_INFO("backtest: the strategy warms up on the data; trading starts at {} ns",
                    trade_at_.ns);
  }
  for (const StartPosition& p : cfg_.positions) {
    std::erase_if(start_positions_,
                  [&](const ReconcileMsg& m) { return m.hdr.instrument == p.instrument; });
    if (p.qty.is_zero()) continue;
    ReconcileMsg m{};
    init_header(m, EventType::Reconcile, p.instrument, cfg_.instruments.get(p.instrument).venue);
    m.kind = ReconcileMsg::Kind::Position;
    m.position_qty = p.qty;
    m.avg_px = p.avg_px;
    start_positions_.push_back(m);
  }
  if (!cfg_.positions.empty()) {
    FASTMM_LOG_INFO("backtest: {} positions from [backtest.positions]", cfg_.positions.size());
  }
  if (cfg_.params_from_journal) {
    if (recorded == nullptr) {
      throw std::invalid_argument(
          "backtest: params_from_journal needs a journal as the data source");
    }
    if (recorded->starts_mid_session) {
      FASTMM_LOG_WARN(
          "backtest: the journal starts at a later part of its session: the parameter updates "
          "before it are not replayed, so [strategy.params] must hold the values as of its start");
    }
    const sim::RecordedParams* rp = &recorded->params;
    // Recorded field index -> this schema's, by name and type (as a replay maps them); a pinned
    // parameter maps to nothing.
    param_map_.assign(rp->table.size(), -1);
    const auto pinned = [&](std::string_view name) {
      return std::find(cfg_.params_pinned.begin(), cfg_.params_pinned.end(), name) !=
             cfg_.params_pinned.end();
    };
    for (std::size_t i = 0; i < rp->table.size(); ++i) {
      for (std::size_t j = 0; schema != nullptr && j < schema->size(); ++j) {
        const ParamDesc& d = schema->begin()[j];
        if (rp->table[i].first == d.name &&
            rp->table[i].second == static_cast<std::uint8_t>(d.type) && !pinned(d.name))
          param_map_[i] = static_cast<int>(j);
      }
    }
    // A journal with no field table numbers its fields as this schema does.
    if (rp->table.empty() && schema != nullptr && !cfg_.params_pinned.empty()) {
      param_map_.assign(schema->size(), -1);
      for (std::size_t j = 0; j < schema->size(); ++j)
        if (!pinned(schema->begin()[j].name)) param_map_[j] = static_cast<int>(j);
    }
    for (ParamUpdateMsg m : rp->updates) {
      if (journal_param(m)) journal_params_.push_back(m);
    }
    FASTMM_LOG_INFO("backtest: {} parameter updates of the journal replayed",
                    journal_params_.size());
  }
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
  if (cfg_.transport.fill_model == sim::FillModel::L3Queue &&
      (source_ == nullptr || !source_->order_by_order())) {
    throw std::invalid_argument(
        "backtest: fill_model l3_queue needs a source that carries every order (itch); "
        "over L2 data it would see no queue at all");
  }
  if (cfg_.balances_from_journal) {
    if (recorded == nullptr) {
      throw std::invalid_argument(
          "backtest: balances_from_journal needs one journal as the data source");
    }
    std::vector<sim::SimAccountConfig>& accounts = cfg_.transport.accounts;
    for (const sim::SimAccountConfig& a : recorded->balances_at(trade_at_)) {
      const bool traded =
          std::any_of(cfg_.instruments.begin(), cfg_.instruments.end(), [&](const Instrument& i) {
            return i.venue == cfg_.transport.pools.primary(a.venue);
          });
      if (!traded) continue;
      const auto it =
          std::find_if(accounts.begin(), accounts.end(), [&](const sim::SimAccountConfig& c) {
            return c.venue == a.venue;
          });
      if (it != accounts.end()) {
        *it = a;
      } else {
        accounts.push_back(a);
      }
    }
    // The account events after the balances the accounts start from: the flows of money in and
    // out, and the connector's refreshes. A pool whose treasury the run simulates moves its own
    // money: the session's flows of its accounts are left out.
    const std::vector<std::pair<VenueId, Timestamp>> refreshes = recorded->refreshes();
    for (const std::pair<VenueId, Timestamp>& r : refreshes) {
      const bool seen = std::any_of(account_origin_.begin(),
                                    account_origin_.end(),
                                    [&](const auto& a) { return a.first == r.first; });
      if (!seen) account_origin_.push_back(r);  // the first snapshot
    }
    std::size_t left_out = 0;
    for (const AccountFlowMsg& f : recorded->flows) {
      if (!flow_applies(f.hdr.venue, sim::account_time(f.hdr))) continue;
      const VenueId v = f.hdr.venue;
      if (std::any_of(cfg_.treasuries.begin(), cfg_.treasuries.end(), [&](const TreasuryConfig& t) {
            return std::find(t.members.begin(), t.members.end(), v) != t.members.end();
          })) {
        ++left_out;
        continue;
      }
      flows_.push_back(f);
    }
    std::stable_sort(
        flows_.begin(), flows_.end(), [](const AccountFlowMsg& x, const AccountFlowMsg& y) {
          return sim::account_time(x.hdr) < sim::account_time(y.hdr);
        });
    for (const auto& [v, t] : refreshes) {
      if (refresh_applies(v, t)) refreshes_.emplace_back(v, t);
    }
    std::stable_sort(refreshes_.begin(), refreshes_.end(), [](const auto& x, const auto& y) {
      return x.second < y.second;
    });
    FASTMM_LOG_INFO(
        "backtest: the accounts follow {} recorded flows of money and {} refreshes{}",
        flows_.size(),
        refreshes_.size(),
        left_out > 0 ? fmt::format(" ({} flows of a simulated treasury's pool left out)", left_out)
                     : std::string());
  }
  if (cfg_.controls_from_journal) {
    if (recorded == nullptr) {
      throw std::invalid_argument(
          "backtest: controls_from_journal needs a journal as the data source");
    }
    controls_ = recorded->controls;
    std::stable_sort(controls_.begin(), controls_.end(), [](const auto& x, const auto& y) {
      return RunTier::control_time(x) < RunTier::control_time(y);
    });
    FASTMM_LOG_INFO("backtest: {} operator commands of the journal replayed", controls_.size());
  }
  following_ = recorded != nullptr && recorded->growing;
  if (following_ && cfg_.latency_from_journal) {
    throw std::invalid_argument(
        "backtest: latency_from_journal needs the whole journal: a journal being followed has "
        "only its past");
  }
  Timestamp start = cfg_.start;
  if (source_ != nullptr && source_->start_ts().valid()) start = source_->start_ts();
  backend_ = std::make_unique<sim::SimBackend>(cfg_.instruments, cfg_.transport, start);
  if (cfg_.latency_from_journal) {
    if (recorded == nullptr) {
      throw std::invalid_argument(
          "backtest: latency_from_journal needs a journal as the data source");
    }
    backend_->transport.set_recorded_latency(&recorded->latency);
    FASTMM_LOG_INFO("backtest: latencies of {} recorded messages followed",
                    recorded->latency.size());
  }
  const std::size_t bars = source_ == synthetic_.get() || generator_ != nullptr
                               ? static_cast<std::size_t>(cfg_.duration.ns / cfg_.equity_bar.ns) + 4
                               : kReserveBars;
  impl_ = std::make_unique<Impl>(cfg_.instruments, bars, cfg_.markout_horizons, cfg_.keep_orders);
  impl_->ledger.set_accounting(cfg_.engine.fx);
  backend_->transport.set_observer(impl_.get());
  if (!cfg_.journal_out.empty()) {
    impl_->journal_ring = std::make_unique<MsgRing>(16U << 20);
    JournalSessionInfo info;
    // Each arm of a branch is a session of its own: its journal does not continue another arm's.
    info.session_id =
        cfg_.seed + (cfg_.ab_arm >= 0 ? (static_cast<std::uint64_t>(cfg_.ab_arm) + 1) << 48U : 0);
    info.start_ts = start;
    info.rng_seed = cfg_.engine.rng_seed;
    info.strategy = cfg_.strategy;
    info.instruments = &cfg_.instruments;
    info.has_session = true;
    info.session_epoch = cfg_.engine.session_epoch;
    info.quoting_enabled = cfg_.engine.quoting_enabled;
    info.replace_venues = cfg_.transport.replace_mask();
    info.own_in_feed = cfg_.transport.own_orders_in_feed;
    info.await_reconcile = cfg_.engine.await_reconcile | trade_venues_;
    info.config_toml = cfg_.config_toml;
    info.params = schema;
    info.strategy_meta = strategy_meta;
    if (!cfg_.config_toml.empty()) info.config_hash = Config::text_hash(cfg_.config_toml);
    if (const std::filesystem::path jp(cfg_.journal_out); jp.has_parent_path()) {
      std::error_code ec;  // a failure shows as the writer's below
      std::filesystem::create_directories(jp.parent_path(), ec);
    }
    impl_->journal_file =
        std::make_unique<JournalFileWriter>(*impl_->journal_ring, cfg_.journal_out, info);
    if (!impl_->journal_file->ok())
      throw std::runtime_error("backtest: cannot create journal " + cfg_.journal_out);
  }
  deps_.engine = cfg_.engine;
  deps_.engine.publish_live = false;  // nobody watches a backtest
  deps_.engine.venue_names = cfg_.venue_names;
  deps_.engine.await_reconcile |= trade_venues_;
  deps_.engine.fees = cfg_.transport.fees;  // what the simulated venue charges
  // The strategy's queue estimate uses the fill model's conservatism.
  deps_.engine.queue_conservatism_bps = cfg_.transport.queue_conservatism_bps;
  deps_.instruments = &cfg_.instruments;
  deps_.params = cfg_.params;
  deps_.journal_ring = impl_->journal_ring.get();
  deps_.backend = backend_.get();
}

bool BacktestSession::journal_param(ParamUpdateMsg& m) const noexcept {
  if (!param_map_.empty()) {
    std::uint32_t kept = 0;
    const std::uint32_t n = std::min<std::uint32_t>(m.count, ParamUpdateMsg::kMaxFields);
    for (std::uint32_t i = 0; i < n; ++i) {
      const int to = m.field[i] < param_map_.size() ? param_map_[m.field[i]] : -1;
      if (to < 0) continue;
      m.field[kept] = static_cast<std::uint16_t>(to);
      m.value[kept] = m.value[i];
      ++kept;
    }
    m.count = kept;
  }
  return m.count != 0;
}

// A flow after the balances its account starts from, on an account of a venue traded here.
bool BacktestSession::flow_applies(VenueId v, Timestamp t) const {
  const bool traded =
      std::any_of(cfg_.instruments.begin(), cfg_.instruments.end(), [&](const Instrument& i) {
        return i.venue == cfg_.transport.pools.primary(v);
      });
  if (!traded) return false;
  if (trade_at_.valid()) return t > trade_at_;
  for (const auto& [venue, first] : account_origin_) {
    if (venue == v) return t > first;
  }
  return true;
}

bool BacktestSession::refresh_applies(VenueId v, Timestamp t) const {
  return flow_applies(v, t);
}

// [ab]: the test's publisher puts each update on the schedule at the time it is asked for, which
// the schedule asks one change ahead of the clock (ParamSchedule::set_refill). The arms at the
// start go straight into the engine's feed, ahead of the first event.
struct BacktestSession::AbRun {
  sim::ParamSchedule* schedule = nullptr;
  InlineFeed* feed = nullptr;  // set while the start's arms are published
  Timestamp at{};
  std::unique_ptr<ParamPublisher> publisher;
  std::unique_ptr<AbController> controller;  // null without [ab]
  std::vector<InstrumentParams> overrides;   // [strategy.overrides]

  static Timestamp refill(void* ctx, Timestamp due) {
    auto* run = static_cast<AbRun*>(ctx);
    run->at = due;
    static_cast<void>(run->controller->poll(due));
    return run->controller->next_change(due);
  }
};

void BacktestSession::enable_ab(StrategyEntry::PublisherFactory publisher) {
  if (!cfg_.publishes_params()) return;
  if (publisher == nullptr) {
    throw std::invalid_argument(
        "backtest: [ab] and [strategy.overrides] need a strategy whose parameters are a "
        "FASTMM_PARAMS struct");
  }
  enable_ab([&](ParamSink sink) { return publisher(sink, cfg_.params, cfg_.instruments.size()); });
}

void BacktestSession::enable_ab(
    const std::function<std::unique_ptr<ParamPublisher>(ParamSink)>& make_publisher) {
  if (!cfg_.publishes_params()) return;
  const bool branch = cfg_.ab.assignment == "branch";
  if (branch && (cfg_.ab_arm < 0 || static_cast<std::size_t>(cfg_.ab_arm) >= cfg_.ab.arms.size())) {
    throw std::invalid_argument(
        "backtest: [ab] assignment = \"branch\" runs one arm per backtest "
        "(BacktestConfig::ab_arm); "
        "fastmm-backtest runs every arm");
  }
  auto run = std::make_unique<AbRun>();
  const ParamSink sink{run.get(), [](void* c, const ParamUpdateMsg& m) noexcept {
                         auto* r = static_cast<AbRun*>(c);
                         if (r->feed != nullptr) {
                           ParamUpdateMsg copy = m;
                           copy.hdr.recv_ts = r->at;
                           return r->feed->push(copy.hdr);
                         }
                         try {
                           r->schedule->at(r->at, m);
                           return true;
                         } catch (...) {
                           return false;
                         }
                       }};
  run->publisher = make_publisher(sink);
  try {
    if (cfg_.ab.enabled()) {
      AbPlan plan(cfg_.ab, cfg_.instruments, cfg_.venue_names);
      if (branch) {
        plan.set_branch(cfg_.ab_arm);
        plan.set_default_start(trade_at_.valid() ? trade_at_.ns : backend_->clock.now().ns);
      }
      run->controller = std::make_unique<AbController>(std::move(plan), *run->publisher);
    }
    run->overrides = resolve_overrides(
        cfg_.param_groups, cfg_.param_overrides, cfg_.instruments, cfg_.venue_names);
  } catch (const ConfigError& e) {
    throw std::invalid_argument(std::string("backtest: ") + e.what());
  }
  if (!run->overrides.empty() && !run->publisher->per_instrument()) {
    throw std::invalid_argument(
        "backtest: [strategy.overrides] needs a strategy that keeps a parameter set per "
        "instrument (ParamScope::PerInstrument, or a Python strategy)");
  }
  for (const InstrumentParams& o : run->overrides) {
    const std::string symbol(cfg_.instruments.get(o.instrument).symbol.view());
    for (const auto& [name, value] : o.values) {
      if (run->controller && run->controller->owns(name))
        throw std::invalid_argument("backtest: [strategy.overrides] sets " + name + " for " +
                                    symbol + ", which the [ab] arms set");
    }
    ParamUpdateMsg m{};
    if (auto err = run->publisher->build(o.values, o.instrument, m))
      throw std::invalid_argument("backtest: [strategy.overrides] " + symbol + ": " + *err);
  }
  if (!run->controller) {
    ab_ = std::move(run);
    return;
  }
  // The recorded updates keep the parameters the test does not own (params_from_journal).
  if (!journal_params_.empty() && schema_ != nullptr) {
    std::size_t dropped = 0;
    for (ParamUpdateMsg& m : journal_params_) {
      std::uint32_t kept = 0;
      for (std::uint32_t i = 0; i < m.count; ++i) {
        if (m.field[i] < schema_->size() &&
            run->controller->owns(schema_->begin()[m.field[i]].name)) {
          ++dropped;
          continue;
        }
        m.field[kept] = m.field[i];
        m.value[kept] = m.value[i];
        ++kept;
      }
      m.count = kept;
    }
    std::erase_if(journal_params_, [](const ParamUpdateMsg& m) { return m.count == 0; });
    if (dropped > 0) {
      FASTMM_LOG_INFO("backtest: {} recorded parameter values left out: the [ab] arms set them",
                      dropped);
    }
  }
  ab_ = std::move(run);
}

// A run between begin() and end().
struct BacktestSession::Loop {
  std::chrono::steady_clock::time_point wall0;
  IEngineRunner* runner = nullptr;
  std::unique_ptr<sim::SimDriver> driver;
  std::unique_ptr<sim::VenueOrderSource> ordered;
  std::unique_ptr<RunTier> tier;
  sim::ParamSchedule* schedule = nullptr;
  Timestamp start{};
  Timestamp bar_end{};
  Duration step{};
  Timestamp reached{};
  // The next stop, once found past a multiverse's event: a fill made before it cannot bring a
  // markout horizon ahead of it (each is at least `step` after the fill), so it holds until
  // reached.
  Timestamp next_stop{};
  bool done = false;
  // next_stop()'s last answer and what it depends on.
  Timestamp stop{};
  std::size_t stop_fills = static_cast<std::size_t>(-1);
  Timestamp stop_reached{};
  Timestamp stop_bar{};
};

BacktestSession::~BacktestSession() {
  if (impl_ && impl_->journal_file) impl_->journal_file->stop();
}

void BacktestSession::begin(const sim::EngineHooks& hooks,
                            IEngineRunner* runner,
                            const Timestamp* world) {
  if (!hooks.valid()) throw std::invalid_argument("backtest: engine hooks not bound");
  if (loop_) throw std::logic_error("backtest: a session runs once");
  loop_ = std::make_unique<Loop>();
  Loop& lp = *loop_;
  lp.wall0 = std::chrono::steady_clock::now();
  lp.runner = runner;
  if (runner != nullptr && runner->keeps_state()) {
    const std::uint32_t v = runner->strategy_state_version();
    check_state_version("backtest",
                        state_what_,
                        runner->strategy_name(),
                        cfg_.engine.initial_state.size(),
                        cfg_.engine.initial_state_version,
                        v);
    check_state_version(
        "backtest", state_what_, runner->strategy_name(), staged_state_.size(), staged_version_, v);
    if (staged_empty_)
      FASTMM_LOG_WARN(
          "backtest: {} has no strategy bytes: the strategy keeps what it learnt in "
          "the warm-up",
          state_what_);
  }
  sim::SimBackend& b = *backend_;
  lp.driver = std::make_unique<sim::SimDriver>(b.clock, b.transport, b.feed, hooks);
  sim::SimDriver& driver = *lp.driver;
  driver.set_measure_wall_clock(cfg_.measure_wall_clock);
  if (generator_ != nullptr) driver.set_generator(generator_.get(), cfg_.generator_seed_levels);
  // Recorded streams are not monotone in venue time: the venue takes them in its own order.
  // A multiverse's runs take the recorded events from the multiverse (world_event()).
  if (world != nullptr) {
    if (source_ == nullptr || generator_ != nullptr)
      throw std::invalid_argument("backtest: a multiverse needs recorded data");
    driver.follow_world(world);
  } else if (source_ != nullptr && cfg_.reorder_window.ns > 0) {
    lp.ordered = std::make_unique<sim::VenueOrderSource>(*source_, cfg_.reorder_window);
    driver.set_source(lp.ordered.get());
    driver.count_by_instrument(cfg_.instruments.size());
  } else if (source_ != nullptr) {
    driver.set_source(source_);
    driver.count_by_instrument(cfg_.instruments.size());
  }
  if (impl_->journal_file) driver.set_journal_writer(impl_->journal_file.get());
  sim::ParamSchedule* schedule = params_;
  if (!journal_params_.empty() || (following_ && cfg_.params_from_journal)) {
    if (schedule == nullptr) {
      own_params_ = std::make_unique<sim::ParamSchedule>();
      schedule = own_params_.get();
    }
    for (const ParamUpdateMsg& m : journal_params_) schedule->at(m.hdr.recv_ts, m);
  }
  if (cfg_.publishes_params()) {
    if (!ab_)
      throw std::invalid_argument(
          "backtest: [ab] and [strategy.overrides] need enable_ab() before run()");
    if (schedule == nullptr) {
      own_params_ = std::make_unique<sim::ParamSchedule>();
      schedule = own_params_.get();
    }
    ab_->schedule = schedule;
  }
  driver.set_param_schedule(schedule);
  lp.schedule = schedule;
  std::unique_ptr<RunTier>& tier = lp.tier;
  if (!cfg_.treasuries.empty() || trade_at_.valid() || !flows_.empty() || !refreshes_.empty() ||
      !controls_.empty() || following_) {
    tier = std::make_unique<RunTier>();
    tier->transport = &b.transport;
    tier->feed = &b.feed;
    if (!cfg_.treasuries.empty()) {
      tier->port = std::make_unique<sim::SimTreasuryPort>(
          b.transport, [&b] { return b.clock.now(); }, cfg_.transfer_latency);
      for (const TreasuryConfig& c : cfg_.treasuries) {
        auto t = std::make_unique<Treasury>(c);
        if (auto r = t->open(); !r) throw std::runtime_error("backtest: treasury: " + r.error());
        tier->interval = Duration{std::min(tier->interval.ns, c.interval_ns)};
        tier->treasuries.push_back(std::move(t));
      }
      tier->next_look = b.clock.now();
    }
    if (trade_at_.valid()) {
      // The checkpoint's bytes are restored at the take-over.
      tier->take_over = !staged_state_.empty();
      if (tier->take_over) {
        if (runner == nullptr)
          throw std::invalid_argument("backtest: start_at from a checkpoint needs the runner");
        runner->stage_strategy_state(staged_state_);
      }
      tier->trade_at = std::max(trade_at_, b.clock.now());
      tier->trade_venues = trade_venues_;
      tier->positions = start_positions_;
    }
    // Those before the run's start are in the balances the accounts start from.
    tier->flows = flows_;
    tier->refreshes = refreshes_;
    tier->controls = controls_;
    while (tier->next_flow < flows_.size() &&
           sim::account_time(flows_[tier->next_flow].hdr) < b.clock.now())
      ++tier->next_flow;
    while (tier->next_refresh < refreshes_.size() &&
           refreshes_[tier->next_refresh].second < b.clock.now())
      ++tier->next_refresh;
    while (tier->next_control < controls_.size() &&
           RunTier::control_time(controls_[tier->next_control]) < b.clock.now())
      ++tier->next_control;
    tier->user = slow_;
    tier->user_next = slow_.run != nullptr ? slow_.first : Timestamp::max();
    sim::SlowHooks tier_hooks;
    tier_hooks.ctx = tier.get();
    tier_hooks.run = &RunTier::run;
    tier_hooks.first = tier->next();
    driver.set_slow_hooks(tier_hooks);
  } else {
    driver.set_slow_hooks(slow_);
  }
  // The recorded session's positions reach the engine at the first step, as its restore did live
  // (with start_at, in the venues' reconciliation when trading starts).
  if (!start_positions_.empty() && !trade_at_.valid())
    b.transport.publish_positions(b.clock.now(), start_positions_);
  driver.start();
  const Timestamp start = b.clock.now();
  lp.start = start;
  if (ab_) {
    ab_->feed = &b.feed;
    ab_->at = start;
    bool all = true;
    for (const InstrumentParams& o : ab_->overrides) {
      all = all && ab_->publisher->publish_as(
                       o.values, o.instrument, ParamUpdateMsg::Origin::Config, "config") != 0;
    }
    if (ab_->controller) all = all && ab_->controller->poll(start);
    ab_->feed = nullptr;
    if (!all) {
      throw std::runtime_error(
          "backtest: the engine's feed has no room for the overrides and the [ab] arms");
    }
    if (ab_->controller) {
      schedule->set_refill(b.clock, ab_.get(), &AbRun::refill, ab_->controller->next_change(start));
    }
  }
  lp.bar_end = start + cfg_.equity_bar;
  // The loop stops at every equity bar and at every markout horizon that has come due, and never
  // runs longer than the shortest horizon at a time, so a fill made inside one step still has its
  // own horizon ahead of the next stop. run_until() is purely time-bounded, so splitting an
  // interval processes exactly the same events in the same order: the run stays deterministic.
  lp.step = impl_->markout_step();
  lp.reached = start;
}

Timestamp BacktestSession::next_stop() const noexcept {
  Loop& lp = *loop_;
  // It moves only with a fill (a markout deadline) or a stop reached.
  if (lp.stop_fills == impl_->fills.size() && lp.stop_reached == lp.reached &&
      lp.stop_bar == lp.bar_end)
    return lp.stop;
  Timestamp stop = lp.bar_end;
  if (const Timestamp due = impl_->next_markout_deadline(); due < stop) stop = due;
  if (lp.step.ns > 0 && lp.reached + lp.step < stop) stop = lp.reached + lp.step;
  lp.stop = stop;
  lp.stop_fills = impl_->fills.size();
  lp.stop_reached = lp.reached;
  lp.stop_bar = lp.bar_end;
  return stop;
}

// The stops of the loop before `limit`, then the events before it (all of them with
// SimKey::never()). False once the run is over.
bool BacktestSession::advance(sim::SimKey limit) {
  Loop& lp = *loop_;
  if (lp.done) return false;
  sim::SimBackend& b = *backend_;
  for (;;) {
    const Timestamp stop = next_stop();
    // A stop comes after every event of its instant.
    const sim::SimKey at{stop, sim::SimKey::kStop};
    if (!(at < limit)) {
      // Everything before the multiverse's next event; the stop comes after it. False: the engine
      // stopped, as the solo loop's run_until(stop) would have found.
      lp.next_stop = stop;
      if (lp.driver->run_before(limit)) return true;
    } else if (lp.driver->run_until(stop)) {
      impl_->resolve_markouts(stop, b.transport);
      lp.reached = stop;
      if (stop == lp.bar_end) {
        impl_->sample(lp.bar_end, b.transport, cfg_.instruments);
        lp.bar_end += cfg_.equity_bar;
      }
      continue;
    }
    const Timestamp end = b.clock.now();
    impl_->resolve_markouts(end, b.transport);
    impl_->sample(end, b.transport, cfg_.instruments);
    lp.done = true;
    return false;
  }
}

bool BacktestSession::advance_before(sim::SimKey k) {
  Loop& lp = *loop_;
  if (!lp.done && k <= sim::SimKey{lp.next_stop, sim::SimKey::kStop} && lp.driver->run_before(k))
    return true;
  return advance(k);
}

sim::SimKey BacktestSession::next_key() {
  const Loop& lp = *loop_;
  if (lp.done) return sim::SimKey::never();
  const sim::SimKey stop{next_stop(), sim::SimKey::kStop};
  const sim::SimKey own = lp.driver->next_key();
  return own < stop ? own : stop;
}

sim::SimKey BacktestSession::step_key() {
  return loop_->done ? sim::SimKey::never() : loop_->driver->step_key();
}

void BacktestSession::world_event(const EventHeader& md) noexcept {
  loop_->driver->world_event(md);
}

void BacktestSession::world_venue_event(const EventHeader& md) noexcept {
  loop_->driver->world_venue_event(md);
}

void BacktestSession::world_feed_event(const EventHeader& md) noexcept {
  loop_->driver->world_feed_event(md);
}

void BacktestSession::shared_market_data(const EventHeader& md) noexcept {
  loop_->driver->shared_market_data(md);
}

void BacktestSession::reach(Timestamp t) noexcept {
  loop_->driver->reach(t);
}

void BacktestSession::credit_market_data(std::uint64_t n) noexcept {
  loop_->driver->credit_market_data(n);
}

bool BacktestSession::stopped() const noexcept {
  return loop_->driver->stopped();
}

void BacktestSession::wake_slow_tier() {
  Loop& lp = *loop_;
  RunTier& tier = *lp.tier;
  tier.flows = flows_;
  tier.refreshes = refreshes_;
  tier.controls = controls_;
  lp.driver->slow_at(tier.next());
}

void BacktestSession::take_record(const EventHeader& record) {
  Loop& lp = *loop_;
  if (lp.done) return;
  // Each list stays in time order past what the run has taken, its equal times in journal order.
  const auto insert = [](auto& list, std::size_t from, auto item, auto time) {
    const auto at =
        std::upper_bound(list.begin() + static_cast<std::ptrdiff_t>(std::min(from, list.size())),
                         list.end(),
                         time(item),
                         [&](Timestamp t, const auto& x) { return t < time(x); });
    list.insert(at, std::move(item));
  };
  switch (record.type) {
    case EventType::ParamUpdate: {
      if (!cfg_.params_from_journal) return;
      ParamUpdateMsg m = msg_cast<ParamUpdateMsg>(&record);
      if (journal_param(m)) lp.schedule->at(m.hdr.recv_ts, m);
      return;
    }
    case EventType::Control: {
      if (!cfg_.controls_from_journal) return;
      std::vector<std::uint64_t> bytes((record.len + 7) / 8);
      std::memcpy(bytes.data(), &record, record.len);
      insert(controls_, lp.tier->next_control, std::move(bytes), RunTier::control_time);
      wake_slow_tier();
      return;
    }
    case EventType::AccountFlow: {
      if (!cfg_.balances_from_journal) return;
      const auto& f = msg_cast<AccountFlowMsg>(&record);
      if (!flow_applies(f.hdr.venue, sim::account_time(f.hdr))) return;
      if (std::any_of(cfg_.treasuries.begin(), cfg_.treasuries.end(), [&](const TreasuryConfig& t) {
            return std::find(t.members.begin(), t.members.end(), f.hdr.venue) != t.members.end();
          }))
        return;
      insert(flows_, lp.tier->next_flow, f, [](const AccountFlowMsg& x) {
        return sim::account_time(x.hdr);
      });
      wake_slow_tier();
      return;
    }
    case EventType::Balance: {
      if (!cfg_.balances_from_journal) return;
      const VenueId v = record.venue;
      const Timestamp t = sim::account_time(record);
      const bool seen = std::any_of(account_origin_.begin(),
                                    account_origin_.end(),
                                    [&](const auto& a) { return a.first == v; });
      if (!seen) {
        account_origin_.emplace_back(v, t);
        return;
      }
      if (!refresh_applies(v, t)) return;
      insert(refreshes_, lp.tier->next_refresh, std::make_pair(v, t), [](const auto& x) {
        return x.second;
      });
      wake_slow_tier();
      return;
    }
    default:
      return;
  }
}

const FillRows& BacktestSession::fill_rows() const noexcept {
  return impl_->fills;
}

const EquityRows& BacktestSession::equity_rows() const noexcept {
  return impl_->equity;
}

std::uint64_t BacktestSession::orders_sent() const noexcept {
  return backend_->transport.stats().orders_sent + backend_->transport.stats().replaces_sent;
}

std::size_t BacktestSession::forget_rows(std::size_t fills, std::size_t bars) {
  Impl& im = *impl_;
  for (const std::size_t n : im.next_) fills = std::min(fills, n);
  const auto drop = [](auto& col, std::size_t n) {
    col.erase(col.begin(), col.begin() + static_cast<std::ptrdiff_t>(std::min(n, col.size())));
  };
  if (fills != 0) {
    FillRows& f = im.fills;
    drop(f.ts, fills);
    drop(f.instrument, fills);
    drop(f.side, fills);
    drop(f.price, fills);
    drop(f.qty, fills);
    drop(f.fee, fills);
    drop(f.cl_ord_id, fills);
    drop(f.liquidity, fills);
    drop(f.mid, fills);
    drop(f.best_bid, fills);
    drop(f.best_ask, fills);
    drop(f.queue_ahead, fills);
    for (std::vector<std::int64_t>& col : f.markout_mid) drop(col, fills);
    for (std::size_t& n : im.next_) n -= fills;
    loop_->stop_fills = static_cast<std::size_t>(-1);
  }
  if (bars != 0) {
    EquityRows& e = im.equity;
    drop(e.ts, bars);
    drop(e.realized, bars);
    drop(e.unrealized, bars);
    drop(e.fees, bars);
    drop(e.position, bars);
    drop(e.mid, bars);
    drop(e.quoted, bars);
    for (InstrumentEquityRows& r : e.by_instrument) {
      drop(r.pnl, bars);
      drop(r.position, bars);
      drop(r.mid, bars);
      drop(r.quoted, bars);
    }
  }
  return fills;
}

BacktestResult BacktestSession::run(const sim::EngineHooks& hooks,
                                    IEngineRunner* runner,
                                    std::string_view strategy_name) {
  const LogClockScope log_clock(backend_->clock);
  begin(hooks, runner, nullptr);
  return end(strategy_name);
}

BacktestResult BacktestSession::end(std::string_view strategy_name) {
  Loop& lp = *loop_;
  lp.driver->follow_world(nullptr);
  while (advance(sim::SimKey::never())) {
  }
  sim::SimBackend& b = *backend_;
  sim::SimDriver& driver = *lp.driver;
  IEngineRunner* runner = lp.runner;
  const std::unique_ptr<sim::VenueOrderSource>& ordered = lp.ordered;
  const std::unique_ptr<RunTier>& tier = lp.tier;
  const Timestamp start = lp.start;
  const auto wall0 = lp.wall0;
  driver.finish();
  if (driver.by_instrument().size() > 1)
    warn_unfed_instruments(cfg_.instruments, driver.by_instrument());
  // The strategy's state at the end ([strategy] state_file): the next backtest, or a live session,
  // starts from it. Captures during the run are superseded by this one.
  if (runner != nullptr && !cfg_.engine.state_file.empty()) {
    std::string bytes;
    if (runner->take_strategy_state(bytes)) {
      if (auto w =
              write_strategy_state(cfg_.engine.state_file, bytes, runner->strategy_state_version());
          !w)
        throw std::runtime_error("backtest: strategy state: " + w.error());
    }
  }
  if (impl_->journal_file) {
    impl_->journal_file->stop();
    impl_->journal_file.reset();
  }

  BacktestResult r;
  r.strategy = std::string(strategy_name);
  r.params = cfg_.params;
  r.seed = cfg_.seed;
  r.instruments.resize(cfg_.instruments.size());
  for (const Instrument& i : cfg_.instruments) {
    const std::size_t v = i.venue.value;
    r.instruments[i.id.value] =
        (v < cfg_.venue_names.size() ? cfg_.venue_names[v] : "venue " + std::to_string(v)) + ":" +
        std::string(i.symbol.view());
  }
  r.fills = std::move(impl_->fills);
  r.orders = std::move(impl_->orders);
  r.equity = std::move(impl_->equity);
  if (runner != nullptr) r.engine = runner->stats();
  r.transport = b.transport.stats();
  r.outbound_sha256 = b.transport.outbound_hash().hex();
  r.outbound_messages = b.transport.outbound_hash().count();
  r.md_events = driver.stats().md_delivered;
  if (ordered) {
    r.md_reordered = ordered->reordered();
    r.md_late = ordered->late();
  }
  r.engine_steps = driver.stats().engine_steps;
  if (tier) {
    r.treasury.pools = static_cast<std::uint32_t>(tier->treasuries.size());
    for (const std::unique_ptr<Treasury>& t : tier->treasuries) {
      const TreasuryStats& ts = t->stats();
      r.treasury.sent += ts.sent;
      r.treasury.done += ts.done;
      r.treasury.failed += ts.failed;
      r.treasury.timed_out += ts.timed_out;
      r.treasury.limited += ts.limited;
      r.treasury.dry_run_plans += ts.dry_run_plans;
    }
    if (tier->port) r.treasury.transfers = tier->port->records();
    if (tier->flows_short > 0) {
      FASTMM_LOG_INFO(
          "backtest: {} recorded flows out of an account moved less than live: the account had "
          "less free",
          tier->flows_short);
    }
  }
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
  r.metrics = compute_metrics(r.equity, r.fills, impl_->order_stats, in);
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
  if (cfg.publishes_params()) session.enable_ab(entry->publisher);
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

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::unique_ptr<MdSource> open_one(std::string_view spec, const InstrumentTable* instruments);
}  // namespace

void warn_unfed_instruments(const InstrumentTable& instruments,
                            std::span<const std::uint64_t> by_instrument) {
  for (const Instrument& i : instruments) {
    if (i.id.value < by_instrument.size() && by_instrument[i.id.value] == 0) {
      FASTMM_LOG_WARN("backtest: the data has no events for instrument {} ({} on venue {})",
                      i.id.value,
                      i.symbol.view(),
                      i.venue.value);
    }
  }
  if (by_instrument.size() > instruments.size() && by_instrument.back() != 0) {
    FASTMM_LOG_WARN(
        "backtest: {} events of the data name an instrument id the configuration lacks (it has "
        "{} instruments)",
        by_instrument.back(),
        instruments.size());
  }
}

std::unique_ptr<MdSource> open_data(std::string_view spec, const InstrumentTable* instruments) {
  spec = trim(spec);
  if (spec.find(';') != std::string_view::npos) {
    // Several sources (one per venue, typically), merged by event time.
    std::vector<std::unique_ptr<MdSource>> parts;
    std::size_t pos = 0;
    for (;;) {
      const std::size_t semi = spec.find(';', pos);
      const std::string_view one = trim(
          spec.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos));
      if (one.empty() || one == "synthetic") {
        throw std::runtime_error("backtest: '" + std::string(spec) +
                                 "': every ';'-separated part must name a data source");
      }
      parts.push_back(open_one(one, instruments));
      if (semi == std::string_view::npos) break;
      pos = semi + 1;
    }
    return std::make_unique<OwnedMergedSource>(std::move(parts));
  }
  return open_one(spec, instruments);
}

namespace {
std::unique_ptr<MdSource> open_one(std::string_view spec, const InstrumentTable* instruments) {
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
  if (ends_with(spec, ".fmj"))
    return std::make_unique<JournalSource>(path, false, true, instruments);
  if (ends_with(spec, ".csv")) return std::make_unique<CsvSource>(path);
  std::string known;
  for (const DataSourceEntry& e : DataSourceRegistry::instance().entries())
    known.append(known.empty() ? "" : ", ").append(e.name);
  throw std::runtime_error("backtest: '" + path +
                           "' is neither *.fmj, *.csv nor <source>:<args> (" + known + ")");
}
}  // namespace

std::unique_ptr<MdSource> open_source(const BacktestConfig& cfg) {
  if (cfg.source.starts_with('[')) {
    throw std::runtime_error(
        "backtest.source: a list is not accepted; write the sources as one string separated by "
        "';' (\"binance:BTCUSDT,2024-03-27,venue=0; csv:other.csv,venue=1\")");
  }
  if (cfg.source.empty()) return open_data(cfg.path, &cfg.instruments);
  // [backtest] source holds a whole spec; the older `source` + `path` pair is the same thing
  // with the path as the source's one positional argument.
  std::string spec = cfg.source;
  if (spec.find(':') == std::string::npos && !cfg.path.empty()) spec += ":" + cfg.path;
  return open_data(spec, &cfg.instruments);
}

}  // namespace fastmm::bt
