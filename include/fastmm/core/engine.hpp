#pragma once
// Engine<Strategy, Clock, Transport, Feed>: the single decision thread (5.1, 5.6).
//
//   feed.next() -> journal(seq) -> dispatch(switch on type) -> book / OMS / positions
//               -> strategy hook -> QuoteManager diff -> risk -> OMS -> transport.send()
//
// Everything the engine touches after warm_up() is preallocated; no virtual calls, no
// exceptions, no heap. The same template runs live (TscClock, LiveTransport, RingFeed),
// in the simulator (SimClock, SimTransport, InlineFeed) and in replay. Live, it runs on its own
// thread (run()) or on the venue's network thread (run_inline() and drain(), run-to-completion).
//
// Strategy hooks are optional member functions, checked in the constructor and dispatched at
// compile time through the table in strategies/hooks.hpp: a hook with a wrong signature is a
// build error, not a silent no-op. Instrument-scoped hooks fire only for instruments in the table.
//
// Time: the engine reads its clock once per consumed event, once per fired timer and once at start
// and finish, and uses that value for every decision and Out* stamp inside (now()). The journal
// records it, so a replay on SimClock sees exactly the times the original run saw.
//
// Execution view: the engine keeps, from the order events only, the quantity of ours a venue's
// feed shows (OwnQuantity, for transports whose venues' public feed includes our orders:
// Transport::own_in_feed), each resting order's estimated queue position (QueueTracker, a load per
// book update and trade on an instrument without one) and each order's send and ack times
// (Oms::times). StrategyContext reads them: own_qty, best_ex_self, queue_ahead, order_times.
//
// Parameters (ADR-0013): a ParamUpdate event assigns new parameter values to the strategy
// (apply_param_update), then on_params runs. With EngineConfig::max_param_age, quoting is disabled
// before the first ParamUpdate and whenever none was applied for that long; the engine checks the
// deadline before each event and timer and with a one-shot timer of its own, all on the journaled
// engine clock.
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/recent_map.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/engine_config.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/own_quantity.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/queue_tracker.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/quote_presence.hpp"
#include "fastmm/core/record_stream.hpp"
#include "fastmm/core/reject_counters.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/strategy_context.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/timer_wheel.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/core/venue_health.hpp"
#include "fastmm/strategies/hooks.hpp"

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace fastmm {

struct EngineStats {
  std::uint64_t events = 0;
  std::uint64_t book_updates = 0;
  std::uint64_t trades = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;
  std::uint64_t fills = 0;
  std::uint64_t risk_rejects = 0;
  std::uint64_t venue_rejects = 0;  // OrderReject messages that changed an order
  std::uint64_t journal_overflows = 0;
  std::uint64_t records_written = 0;  // records handed to the store ring (core/record_stream.hpp)
  std::uint64_t records_dropped = 0;  // ... and dropped because it was full
  std::uint64_t transport_full = 0;
  std::uint64_t timers_fired = 0;
  std::uint64_t crossed_pulls = 0;
  std::uint64_t unknown_order_cancels = 0;
  std::uint64_t kills = 0;        // global kill switch trips
  std::uint64_t venue_kills = 0;  // per-venue kill switch trips (ControlCommand::TripVenueKill)
  std::uint64_t unconverted_fees = 0;    // fills whose commission asset is neither base nor quote
  std::uint64_t invalid_fee_assets = 0;  // fills whose fee_asset is out of range (booked as quote)
  std::uint64_t unknown_instrument_fills = 0;  // not booked, not passed to on_fill
  std::uint64_t param_updates = 0;             // ParamUpdate events applied
  std::uint64_t param_expiries = 0;            // max_param_age passed: quoting disabled
  std::uint64_t synthetic_fills = 0;  // fills booked from a cum_qty jump (missed fill messages)
  // Executions replayed from a venue's trade history (Venue::request_executions), and how many of
  // them named a synthetic fill, replacing its estimated price and missing fee with the venue's.
  std::uint64_t replayed_fills = 0;
  std::uint64_t corrected_fills = 0;
  // Reconciliations whose snapshot was preceded by a complete execution replay, and those where the
  // connector could not ask or the query failed: only the first kind can be taken as exact.
  std::uint64_t exact_reconciles = 0;
  std::uint64_t estimated_reconciles = 0;
  // Orders a reconciliation snapshot dropped while they still had working quantity: the venue
  // ended them without saying how, so the position may be short by up to that much.
  std::uint64_t unresolved_orders = 0;
  // Executions replayed from the venue that named no order of this session (booked, not errors).
  std::uint64_t replayed_foreign_fills = 0;
  // Reconciliations the engine asked a venue for (ControlCommand::Reconcile).
  std::uint64_t reconcile_requests = 0;
  std::uint64_t ack_timeouts = 0;    // PendingNew orders force-cancelled by the ack sweep
  std::uint64_t flattens = 0;        // operator flattens started (ControlCommand::Flatten)
  std::uint64_t flatten_orders = 0;  // reduce-only orders a flatten sent
  std::uint64_t steps = 0;
  std::uint64_t clock_reanchors = 0;     // TscClock picked up a recalibration continuously
  std::uint64_t clock_steps = 0;         // ... or had to step (old mapping off by > threshold)
  RejectCounts risk_rejects_by_reason;   // sums to risk_rejects
  RejectCounts venue_rejects_by_reason;  // sums to venue_rejects
};

// Funding payments booked (EventType::Funding), and those not booked: seen before (the same venue
// id on the same instrument), on an instrument outside the table, or in an asset other than the
// instrument's settlement currency. Apart from EngineStats: the event is rare, and its state stays
// off the lines the market-data path uses.
struct FundingStats {
  std::uint64_t payments = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t unbooked = 0;
};

template <class Strategy, ClockLike Clock, TransportLike Transport, FeedLike Feed = RingFeed>
class Engine {
 public:
  using Book = L2Book<256>;
  using Context = StrategyContext<Engine>;
  static constexpr std::size_t kOutBatch = 32;
  static constexpr std::size_t kOutSlotBytes = 192;  // largest Out*Msg
  // TimerMsg::engine values for the engine's own timers (1 is the max_param_age one).
  static constexpr std::uint8_t kAckSweepTimer = 2;
  static constexpr std::uint8_t kFlattenTimer = 3;

  Engine(const EngineConfig& cfg,
         const InstrumentTable& instruments,
         Clock& clock,
         Transport& transport,
         Feed& feed,
         Strategy& strategy,
         MsgRing* journal_ring = nullptr,
         MsgRing* record_ring = nullptr)
      : cfg_(cfg),
        instruments_(instruments),
        clock_(clock),
        transport_(transport),
        feed_(feed),
        strategy_(strategy),
        ctx_(this),
        books_(new Book[kMaxInstruments]),
        oms_(cfg.session_epoch),
        risk_(cfg.risk, clock.now()),
        quotes_(cfg.quotes),
        timers_(clock.now()),
        journal_(journal_ring),
        records_(record_ring, cfg.session_id),
        rng_(cfg.rng_seed),
        spin_(cfg.spin_mode),
        reject_log_(cfg.reject_log_interval),
        fx_on_(cfg.fx.active()),
        quoting_enabled_(cfg.quoting_enabled),
        params_stale_(cfg.max_param_age.ns > 0) {
    // Every hook the strategy declares must match the engine's call (strategies/hooks.hpp).
    static_assert(verify_strategy<Strategy, Context, Book>());
    // Replace is only used if every venue we trade supports it (QuoteManager is global).
    QuoteParams qp = cfg.quotes;
    for (const Instrument& inst : instruments_) {
      if (!transport_.supports_replace(inst.venue)) qp.supports_replace = false;
    }
    quotes_.set_params(qp);
    positions_.set_accounting(cfg.fx);
    risk_.set_fx(cfg.fx);
    // Our quantity in the feed is followed only for venues whose feed shows our orders.
    for (const Instrument& inst : instruments_) {
      if (transport_own_in_feed(inst.venue)) own_venues_ |= venue_mask(inst.venue);
    }
    if (own_venues_ != 0) {
      own_ = std::make_unique<OwnQuantity>();
      own_->prepare(instruments_.size());
    }
  }
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // ---- lifecycle ----------------------------------------------------------------------------

  // Touch every pool/page and exercise the dispatch path once so the first real event does
  // not pay for page faults or cold i-cache.
  void warm_up() noexcept {
    oms_.warm_up();
    Logger::instance().attach_current_thread();
    alignas(64) std::byte buf[BookDeltaMsg::size_for(8, 8)];
    for (const Instrument& inst : instruments_) {
      auto* d = reinterpret_cast<BookDeltaMsg*>(buf);
      init_header(*d, EventType::BookDelta, inst.id, inst.venue, sizeof buf);
      d->hdr.flags |= EventHeader::kSnapshot;
      d->bid_count = d->ask_count = 8;
      for (std::uint32_t i = 0; i < 8; ++i) {
        d->levels()[i] =
            Level{Price::from_raw(inst.tick.raw * static_cast<std::int64_t>(1000 - i)), inst.lot};
        d->levels()[8 + i] =
            Level{Price::from_raw(inst.tick.raw * static_cast<std::int64_t>(1001 + i)), inst.lot};
      }
      Book& b = books_[inst.id.value];
      b.apply_delta(*d);
      b.clear();
    }
    for (int i = 0; i < 1024; ++i) latency_.record(LatencyInterval::TickToTrade, 100);
    latency_.reset();
  }

  // Processes up to max_events_per_step events plus due timers. Returns the number handled.
  std::size_t step() noexcept {
    std::size_t n = 0;
    ++stats_.steps;
    in_engine_ = true;
    while (n < cfg_.max_events_per_step) {
      const EventHeader* h = feed_.next();
      if (h == nullptr) break;
      ++n;
      process(h);
      feed_.release();
    }
    refresh_clock();
    const Timestamp now = clock_.now();
    n += timers_.poll(now, [this](TimerId id, std::uint64_t ud) { on_timer_fired(id, ud); });
    if (now - last_publish_ >= cfg_.latency_publish_interval) publish_latency(now);
    in_engine_ = false;
    return n;
  }

  // Run-to-completion: processes every event the feed holds now, without timers. The venue calls
  // it (EventSink drain hook) right after decoding an event on this thread. Returns 0 when called
  // from inside the engine (the venue refused an order the engine was sending): the event stays in
  // its ring for the loop that is already running.
  std::size_t drain() noexcept {
    if (in_engine_) return 0;
    in_engine_ = true;
    std::size_t n = 0;
    while (const EventHeader* h = feed_.next()) {
      ++n;
      process(h);
      feed_.release();
    }
    in_engine_ = false;
    return n;
  }

  void run() {
    pin_to_cpu(cfg_.cpu);
    set_thread_name("fm-engine");
    warm_up();
    start();
    while (!stop_.load(std::memory_order_relaxed)) {
      if (step() != 0) {
        spin_.active();
      } else if (!spin_.spin()) {
        block_idle();
      }
    }
    finish();
  }
  // run() with the venue's network loop on this thread (fastmm-live [engine] threading =
  // "single"): `poll` runs one reactor iteration before every step. Market data decoded in it
  // reaches drain() at once; order events, control messages and timers wait for the step.
  void run_inline(InlinePollFn poll, void* ctx) {
    pin_to_cpu(cfg_.cpu);
    set_thread_name("fm-engine");
    warm_up();
    start();
    while (!stop_.load(std::memory_order_relaxed)) {
      const std::size_t n = poll(ctx) + step();
      if (n == 0) {
        spin_.idle();
      } else {
        spin_.active();
      }
    }
    finish();
  }
  // Calls on_start. on_quoting does not fire for the initial state (on_start reads
  // ctx.quoting_enabled()), only for a change made during on_start.
  void start() noexcept {
    if (started_) return;
    started_ = true;
    in_engine_ = true;
    latch_clock();
    set_event_origin(Cycles{}, Cycles{});
    // The rate limiter refills from the start time, not from construction (replay constructs the
    // engine at a different time).
    risk_.bucket().rebase(now_);
    if (journal_.enabled() && !journal_.record_clock(EngineTimeMsg::Kind::Start, now_))
      ++stats_.journal_overflows;
    FASTMM_LOG_INFO(
        "strategy {} hooks: {}", strategy_name(), implemented_hooks<Strategy, Context, Book>());
    if (cfg_.ack_timeout.ns > 0) ack_timer_ = timers_.add(now_, cfg_.ack_timeout, /*repeat=*/true);
    [[maybe_unused]] const bool quoting_before = quoting_enabled();
    if constexpr (has_hook(Hook::Start)) strategy_.on_start(ctx_);
    if constexpr (has_hook(Hook::Quoting)) notify_quoting(quoting_before);
    flush_out();
    unlatch_clock();
    in_engine_ = false;
  }
  void finish() noexcept {
    if (!started_ || finished_) return;
    finished_ = true;
    in_engine_ = true;
    latch_clock();
    set_event_origin(Cycles{}, Cycles{});
    if (journal_.enabled() && !journal_.record_clock(EngineTimeMsg::Kind::Finish, now_))
      ++stats_.journal_overflows;
    if constexpr (has_hook(Hook::Stop)) strategy_.on_stop(ctx_);
    flush_out();
    // The last word on every position, so a store holds the state the session ended in.
    if (records_.enabled()) {
      for (const Instrument& inst : instruments_) {
        const Position& p = positions_.get(inst.id);
        if (p.fills != 0 || !p.qty.is_zero()) emit_position(inst.id);
      }
    }
    publish_latency(now_);
    unlatch_clock();
    in_engine_ = false;
  }
  void stop() noexcept {
    stop_.store(true, std::memory_order_release);
    if constexpr (kFeedWaits) feed_.notify();
  }
  [[nodiscard]] bool stopped() const noexcept { return stop_.load(std::memory_order_acquire); }

  // ---- strategy-facing API ------------------------------------------------------------------

  // The time of the event, timer, start or finish being processed; outside those (direct calls
  // from tests or tools) the clock itself.
  [[nodiscard]] FASTMM_FORCE_INLINE Timestamp now() const noexcept {
    return FASTMM_LIKELY(latched_) ? now_ : clock_.now();
  }
  [[nodiscard]] const Book& book(InstrumentId id) const noexcept { return books_[id.value]; }
  [[nodiscard]] Book& book_mut(InstrumentId id) noexcept { return books_[id.value]; }
  [[nodiscard]] const Position& position(InstrumentId id) const noexcept {
    return positions_.get(id);
  }
  // Net PnL [risk] max_loss is measured against: this session's, plus the loss carried over from
  // earlier sessions (EngineConfig::pnl_carry, from the durable kill state), so a restart does not
  // re-arm the whole budget.
  [[nodiscard]] Notional net_pnl() const noexcept { return positions_.net_pnl() + cfg_.pnl_carry; }
  [[nodiscard]] const Instrument& instrument(InstrumentId id) const noexcept {
    return instruments_.get(id);
  }
  [[nodiscard]] const InstrumentTable& instruments() const noexcept { return instruments_; }
  [[nodiscard]] Xoshiro256ss& rng() noexcept { return rng_; }
  [[nodiscard]] Oms& oms() noexcept { return oms_; }
  [[nodiscard]] const Oms& oms() const noexcept { return oms_; }
  [[nodiscard]] RiskEngine& risk() noexcept { return risk_; }
  [[nodiscard]] const RiskEngine& risk() const noexcept { return risk_; }
  [[nodiscard]] PositionTracker& positions() noexcept { return positions_; }
  [[nodiscard]] const PositionTracker& positions() const noexcept { return positions_; }
  [[nodiscard]] LatencyTracker& latency() noexcept { return latency_; }
  [[nodiscard]] JournalWriter& journal() noexcept { return journal_; }
  [[nodiscard]] RecordWriter& records() noexcept { return records_; }
  [[nodiscard]] QuoteManager& quote_manager() noexcept { return quotes_; }
  // Time-weighted two-sided quoting, what a market-maker programme pays for. settle() first.
  [[nodiscard]] QuotePresence& presence() noexcept { return presence_; }
  [[nodiscard]] TimerWheel<>& timers() noexcept { return timers_; }
  [[nodiscard]] Strategy& strategy() noexcept { return strategy_; }
  [[nodiscard]] Transport& transport() noexcept { return transport_; }
  [[nodiscard]] Context& context() noexcept { return ctx_; }
  [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const FundingStats& funding_stats() const noexcept { return funding_->stats; }
  [[nodiscard]] const Seqlocked<LatencySnapshot>& latency_snapshot() const noexcept {
    return latency_pub_;
  }
  [[nodiscard]] EngineLiveStats live_stats() const noexcept { return live_pub_.load(); }
  [[nodiscard]] bool quoting_enabled() const noexcept {
    return quoting_enabled_ && !reconciling_ && !params_stale_ && !risk_.killed();
  }
  // ... and this instrument was not pulled on its own, nor its venue (ControlCommand::PullQuotes
  // with a scope, an engine-owned flatten).
  [[nodiscard]] bool quoting_enabled(InstrumentId id) const noexcept {
    if (!quoting_enabled() || !instruments_.contains(id)) return false;
    const Instrument& inst = instruments_.get(id);
    return !inst_pulled_[id.value] && !venue_is_pulled(inst.venue) &&
           !risk_.venue_killed(inst.venue) && !health_.gated(inst.venue, now());
  }
  // Where the operator's flatten stands, and how many orders it has sent.
  [[nodiscard]] FlattenState flatten_state() const noexcept { return flatten_state_; }
  // The instrument it covers; invalid when it covers every instrument.
  [[nodiscard]] InstrumentId flatten_scope() const noexcept { return flatten_scope_; }
  [[nodiscard]] bool reconciling() const noexcept { return reconciling_; }
  // max_param_age is set and no ParamUpdate was applied within it (or none yet).
  [[nodiscard]] bool params_stale() const noexcept { return params_stale_; }
  // The first reason the global kill switch was set for (None while it is not set).
  [[nodiscard]] KillReason kill_reason() const noexcept { return kill_reason_; }
  [[nodiscard]] KillReason venue_kill_reason(VenueId v) const noexcept {
    return venue_kill_reasons_[RiskEngine::venue_slot(v)];
  }
  [[nodiscard]] const EngineConfig& config() const noexcept { return cfg_; }

  // ---- venue state: fees, risk headroom, venue health ------------------------------------------

  // The maker/taker rates of an instrument (EngineConfig::fees): the configuration's, or the
  // account's own where the connector fetched them ([venues.<x>] fetch_fees).
  [[nodiscard]] const FeeRates& fees(InstrumentId id) const noexcept {
    return cfg_.fees.schedule(id);
  }
  // What each risk limit admits on `id` now (RiskEngine::headroom), with the inputs the next
  // check_new would use.
  [[nodiscard]] RiskHeadroom risk_headroom(InstrumentId id) const noexcept {
    if (!instruments_.contains(id)) return {};
    const Timestamp now = this->now();
    RiskInputs buy{now,
                   &positions_.get(id),
                   positions_.gross_exposure(),
                   positions_.net_exposure(),
                   oms_.open_qty(id, Side::Buy),
                   oms_.open_count(id),
                   Price{}};
    RiskInputs sell = buy;
    sell.open_same_side = oms_.open_qty(id, Side::Sell);
    return risk_.headroom(instruments_.get(id), buy, sell, net_pnl());
  }
  [[nodiscard]] VenueHealthView venue_health(VenueId v) const noexcept {
    return health_.view(v, now());
  }

  // ---- execution view -------------------------------------------------------------------------

  // Our resting quantity at (id, side, px) that the venue's feed shows as of venue time `at`; zero
  // where the feed does not show our orders.
  [[nodiscard]] Qty own_qty(InstrumentId id, Side side, Price px, Timestamp at) const noexcept {
    if (own_ == nullptr || !instruments_.contains(id) || !own_venue(instruments_.get(id).venue))
      return Qty{};
    return own_->own_at(id, side, px, at);
  }
  // ... as of the book's last update.
  [[nodiscard]] Qty own_qty(InstrumentId id, Side side, Price px) const noexcept {
    if (own_ == nullptr) return Qty{};
    return own_qty(id, side, px, books_[id.value].last_update());
  }
  // The best level on `side` after taking our quantity out; levels that were only ours are
  // skipped. Level{} when nothing is left.
  [[nodiscard]] Level best_ex_self(InstrumentId id, Side side) const noexcept {
    const Book& b = books_[id.value];
    const std::size_t n = b.depth(side);
    for (std::size_t i = 0; i < n; ++i) {
      const Level l = b.level(side, i);
      const Qty own = own_qty(id, side, l.price);
      if (own < l.qty) return Level{l.price, l.qty - own};
    }
    return Level{};
  }
  // Estimated quantity ahead of an open order (QueueTracker). The first call starts the tracking:
  // orders resting then join the back of their level as it shows now, later ones at their ack.
  [[nodiscard]] std::optional<Qty> queue_ahead(ClientOrderId id) noexcept {
    if (!queue_.enabled()) [[unlikely]]
      start_queue_tracking();
    return queue_.ahead(oms_.find(id));
  }
  FASTMM_NOINLINE void start_queue_tracking() noexcept {
    queue_.enable();
    oms_.for_each_open_order([&](Handle<Order> h, const Order& o) {
      if (resting(o.state) && instruments_.contains(o.instrument))
        queue_.place(h, o, queue_shown(o));
    });
  }
  [[nodiscard]] const QueueTracker& queue() const noexcept { return queue_; }
  [[nodiscard]] const OwnQuantity* own_quantity() const noexcept { return own_.get(); }

  [[nodiscard]] RunnerStats runner_stats() const noexcept {
    RunnerStats r;
    r.events = stats_.events;
    r.book_updates = stats_.book_updates;
    r.orders_sent = stats_.orders_sent;
    r.cancels_sent = stats_.cancels_sent;
    r.replaces_sent = stats_.replaces_sent;
    r.fills = stats_.fills;
    r.risk_rejects = stats_.risk_rejects;
    r.journal_overflows = stats_.journal_overflows;
    r.records_dropped = stats_.records_dropped;
    r.transport_full = stats_.transport_full;
    r.timers_fired = stats_.timers_fired;
    r.realized_pnl_raw = positions_.total_realized().raw;
    r.unrealized_pnl_raw = positions_.total_unrealized().raw;
    r.fees_raw = positions_.total_fees().raw;
    r.funding_raw = positions_.total_funding().raw;
    const auto& h = latency_.histogram(LatencyInterval::TickToTrade);
    r.tick_to_trade_p50_ns = h.percentile(0.5);
    r.tick_to_trade_p99_ns = h.percentile(0.99);
    r.venue_rejects = stats_.venue_rejects;
    r.risk_rejects_by_reason = stats_.risk_rejects_by_reason;
    r.venue_rejects_by_reason = stats_.venue_rejects_by_reason;
    return r;
  }

  // T3: the strategy's first order decision in the current event (StrategyContext calls this
  // before forwarding send/cancel/replace/set_quotes/pull_quotes). Serialize runs from here to
  // the hand-off to the transport; engine-initiated sends (kill switch, connection loss, pulled
  // quotes on a stale book) record no serialize sample.
  void mark_decision() noexcept {
    if (strategy_t3_.v == 0) strategy_t3_ = clock_.cycles();
  }

  Result<ClientOrderId, RejectReason> send_order(const NewOrderRequest& req) noexcept {
    enter_api();
    auto r = submit_new(req);
    flush_out();
    return r;
  }
  Result<void, RejectReason> cancel_order(ClientOrderId id) noexcept {
    enter_api();
    const Handle<Order> h = oms_.find(id);
    if (!h.valid()) return fail(RejectReason::UnknownOrder);
    auto r = submit_cancel(h);
    flush_out();
    return r;
  }
  Result<void, RejectReason> replace_order(ClientOrderId id, Price px, Qty qty) noexcept {
    enter_api();
    const Handle<Order> h = oms_.find(id);
    if (!h.valid()) return fail(RejectReason::UnknownOrder);
    auto r = submit_replace(h, px, qty);
    flush_out();
    return r;
  }
  // False when the quotes are ignored: quoting is disabled for the session or for this instrument
  // or its venue (an operator pull, a flatten), the instrument is not in the table, its venue's
  // kill switch is engaged (its quotes were pulled when it tripped) or the feed-lag gate holds it.
  bool set_quotes(InstrumentId id, const DesiredQuotes& q) noexcept {
    if (!quoting_enabled() || FASTMM_UNLIKELY(!instruments_.contains(id))) return false;
    if (FASTMM_UNLIKELY(inst_pulled_[id.value])) return false;
    const Instrument& inst = instruments_.get(id);
    if (FASTMM_UNLIKELY(risk_.venue_killed(inst.venue) || venue_is_pulled(inst.venue) ||
                        health_.gated(inst.venue, now())))
      return false;
    enter_api();
    Placer place{this};
    quotes_.reconcile(inst, q, oms_, now_, place);
    flush_out();
    return true;
  }
  void pull_quotes(InstrumentId id) noexcept {
    enter_api();
    Placer place{this};
    quotes_.pull_quotes(instruments_.get(id), oms_, place);
    flush_out();
  }
  void pull_all_quotes() noexcept {
    enter_api();
    Placer place{this};
    for (const Instrument& inst : instruments_) quotes_.pull_quotes(inst, oms_, place);
    flush_out();
  }
  // Trips the global kill switch from inside the engine thread (a strategy that failed): quoting is
  // disabled, quotes are pulled and every working order is cancelled. The first reason is kept.
  void trip_kill(KillReason reason) noexcept {
    risk_.trip();
    on_kill(reason);
  }
  // Cancels every working order (kill switch / control). Cancels are always allowed.
  void mass_cancel() noexcept {
    enter_api();
    StaticVector<Handle<Order>, kMaxOpenOrders> handles;
    oms_.for_each_open_order([&](Handle<Order> h, const Order& o) {
      if (o.is_working()) static_cast<void>(handles.push_back(h));
    });
    for (Handle<Order> h : handles) static_cast<void>(submit_cancel(h));
    flush_out();
  }
  [[nodiscard]] TimerId add_timer(Duration period,
                                  bool repeat,
                                  std::uint64_t user_data = 0) noexcept {
    return timers_.add(now(), period, repeat, user_data);
  }
  bool cancel_timer(TimerId id) noexcept { return timers_.cancel(id); }

  // Injects an event as if it came from the feed (tests, control thread bypass in sim).
  void inject(const EventHeader* h) noexcept { process(h); }

 private:
  struct Placer {
    Engine* e;
    bool operator()(QuoteAction& a) noexcept { return e->place_quote(a); }
  };

  // ---- strategy hooks -------------------------------------------------------------------------

  [[nodiscard]] static constexpr bool has_hook(Hook h) noexcept {
    return implements_hook<Strategy, Context, Book>(h);
  }
  [[nodiscard]] static std::string_view strategy_name() noexcept {
    if constexpr (requires {
                    { Strategy::name() } -> std::convertible_to<std::string_view>;
                  }) {
      return Strategy::name();
    } else {
      return "(unnamed)";
    }
  }
  // Reports a change of quoting_enabled() made by the event, timer or start-up that just ran, after
  // its hooks have returned and every flag is final. Never called from inside a context call, so a
  // kill switch tripped by set_quotes or send does not re-enter the strategy; a change made inside
  // on_quoting itself is reported by a further call once it returns (quoting can only be enabled
  // again by a control message or the end of a reconciliation, so this ends).
  void notify_quoting(bool before) noexcept {
    bool reported = before;
    for (bool now = quoting_enabled(); now != reported; now = quoting_enabled()) {
      reported = now;
      strategy_.on_quoting(ctx_, now);
      flush_out();
    }
  }

  // ---- event loop ---------------------------------------------------------------------------

  // The feed can wake a blocked engine (RingFeed): producers notify it after publishing.
  static constexpr bool kFeedWaits = requires(Feed& f) {
    { f.waker() } -> std::same_as<Waker&>;
    { f.pending() } -> std::same_as<bool>;
    f.notify();
  };
  static constexpr Duration kMaxIdleWait = milliseconds(1);

  // Adaptive spin with the spin budget used up: block until a producer notifies the feed, the next
  // timer or latency publish is due, or kMaxIdleWait passes. Feeds that cannot wake the engine
  // sleep 50 us.
  void block_idle() noexcept {
    if constexpr (kFeedWaits) {
      const Timestamp now = clock_.now();
      Timestamp until = now + kMaxIdleWait;
      if (cfg_.latency_publish_interval.ns > 0) {
        const Timestamp publish = last_publish_ + cfg_.latency_publish_interval;
        if (publish < until) until = publish;
      }
      until = timers_.next_expiry_before(until);
      if (until <= now) return;
      Waker& waker = feed_.waker();
      waker.prepare_wait();
      if (feed_.pending() || stop_.load(std::memory_order_acquire)) {
        waker.cancel_wait();
        return;
      }
      waker.wait(until - now);
    } else {
      sleep_for(microseconds(50));
    }
  }

  void process(const EventHeader* h) noexcept {
    ++stats_.events;
    latch_clock();
    if (journal_.enabled()) {
      auto r = journal_.record_at(*h, now_);
      if (FASTMM_UNLIKELY(!r)) {
        ++stats_.journal_overflows;
        on_journal_overflow();
      }
    }
    set_event_origin(h->t0_cycles, Cycles{h->t0_cycles.v + h->t1_delta});
    // The ParamUpdate that renews the parameters does not first expire them.
    const bool renews_params = h->type == EventType::ParamUpdate;
    if constexpr (has_hook(Hook::Quoting)) {
      const bool quoting_before = quoting_enabled();
      if (!renews_params) check_param_age();
      dispatch(h);
      notify_quoting(quoting_before);
    } else {
      if (!renews_params) check_param_age();
      dispatch(h);
    }
    unlatch_clock();
  }

  void dispatch(const EventHeader* h) noexcept {
    switch (h->type) {
      [[likely]] case EventType::BookDelta:
      case EventType::BookSnapshot:
        on_book_delta(msg_cast<BookDeltaMsg>(h));
        break;
      case EventType::Trade:
        on_trade(msg_cast<TradeMsg>(h));
        break;
      case EventType::BookTicker:
        on_book_ticker(msg_cast<BookTickerMsg>(h));
        break;
      case EventType::OptionTicker:
        on_option_ticker(msg_cast<OptionTickerMsg>(h));
        break;
      case EventType::OrderAck:
        on_order_ack(msg_cast<OrderAckMsg>(h));
        break;
      case EventType::OrderReject:
        on_order_reject(msg_cast<OrderRejectMsg>(h));
        break;
      case EventType::OrderCancelAck:
        on_cancel_ack(msg_cast<OrderCancelAckMsg>(h));
        break;
      case EventType::OrderCancelReject:
        on_cancel_reject(msg_cast<OrderCancelRejectMsg>(h));
        break;
      case EventType::OrderFill:
        on_fill(msg_cast<OrderFillMsg>(h));
        break;
      case EventType::OrderExpired:
        on_expired(msg_cast<OrderExpiredMsg>(h));
        break;
      case EventType::PositionUpdate:
        on_position_update(msg_cast<PositionUpdateMsg>(h));
        break;
      case EventType::Funding:
        on_funding(msg_cast<FundingMsg>(h));
        break;
      case EventType::Timer: {
        const auto& t = msg_cast<TimerMsg>(h);
        if (t.engine == kAckSweepTimer) {
          sweep_acks();  // replay of the engine's ack_timeout sweep
        } else if (t.engine == kFlattenTimer) {
          flatten_tick();  // replay of the engine's flatten sweep
        } else if (t.engine != 0) {
          check_param_age();  // replay of the engine's max_param_age timer
        } else {
          fire_strategy_timer(t.timer_id, t.user_data);
        }
        break;
      }
      case EventType::ParamUpdate:
        on_param_update(msg_cast<ParamUpdateMsg>(h));
        break;
      case EventType::Control:
        on_control(msg_cast<ControlMsg>(h));
        break;
      case EventType::ConnectionState:
        on_connection_state(msg_cast<ConnectionStateMsg>(h));
        break;
      case EventType::Reconcile:
        on_reconcile(msg_cast<ReconcileMsg>(h));
        break;
      case EventType::LatencySample:
      case EventType::OutNewOrder:
      case EventType::OutCancel:
      case EventType::OutReplace:
      case EventType::OrderAddL3:
      case EventType::OrderExecL3:
      case EventType::OrderCancelL3:
      case EventType::OrderReplaceL3:
      case EventType::Padding:
      case EventType::EngineTime:
      case EventType::Count:
        break;  // not consumed by the L2 engine
    }
  }

  // ---- market data --------------------------------------------------------------------------

  void on_book_delta(const BookDeltaMsg& d) noexcept {
    const InstrumentId id = d.hdr.instrument;
    if (FASTMM_UNLIKELY(!instruments_.contains(id))) return;
    Book& b = books_[id.value];
    b.apply_delta(d);
    ++stats_.book_updates;
    track_feed_lag(d.hdr);
    if (queue_.any(id)) queue_on_book(d, b);
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    const Timestamp now = now_;
    const Instrument& inst = instruments_.get(id);
    if (FASTMM_LIKELY(b.is_valid())) {
      const Price mid = b.mid();
      // The engine clock, which the stale check compares against: recv_ts is the network thread's
      // wall clock, and a host clock step moves it relative to the engine's TscClock.
      risk_.on_book(id, mid, now);
      positions_.mark(id, mid, inst);
      if (FASTMM_UNLIKELY(fx_on_) && cfg_.fx.prices[id.value] != 0) on_fx_mid(id, mid);
      if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
    } else {
      if (FASTMM_UNLIKELY(fx_on_) && cfg_.fx.prices[id.value] != 0)
        risk_.on_fx_book(cfg_.fx.prices[id.value] - 1U, false);
      if (b.crossed() && b.crossed_for(now) > cfg_.crossed_grace) {
        ++stats_.crossed_pulls;
        pull_quotes(id);
      }
    }
    if constexpr (has_hook(Hook::Book)) {
      const Book& view = b;
      strategy_.on_book(ctx_, id, view);
      record_strategy_hop(t2);
    }
    flush_out();
  }

  // ---- venue health (core/venue_health.hpp) ---------------------------------------------------
  // A market-data message with a venue time: its feed lag, and the gate when it engages. Snapshots
  // (REST, resync) carry no live venue time.
  FASTMM_FORCE_INLINE void track_feed_lag(const EventHeader& h) noexcept {
    if (FASTMM_UNLIKELY(!h.exch_ts.valid() || (h.flags & EventHeader::kSnapshot) != 0)) return;
    if (FASTMM_UNLIKELY(health_.on_md(h.venue, (h.recv_ts - h.exch_ts).ns, now_, feed_lag_limit_)))
      on_feed_lag_gate(h.venue, feed_lag_limit_);
  }
  // The gate engaged on venue `v`: its quotes go now; set_quotes ignores it and risk refuses
  // orders that could rest there (FeedLag) until VenueHealth::kGateHold passes without a message
  // over the limit. The strategy's next set_quotes after that places them again.
  FASTMM_NOINLINE void on_feed_lag_gate(VenueId v, Duration limit) noexcept {
    if (health_.view(v, now_).gate_engagements == 1) {
      FASTMM_LOG_WARN(
          "venue {} market data {} us late (max_feed_lag_ms {}): quotes pulled until it is back "
          "under for {} ms (first of this session; ctx.venue_health counts them)",
          v.value,
          health_.view(v, now_).feed_lag_excess.micros(),
          limit.millis(),
          VenueHealth::kGateHold.millis());
    }
    enter_api();
    Placer place{this};
    for (const Instrument& inst : instruments_) {
      if (inst.venue == v) quotes_.pull_quotes(inst, oms_, place);
    }
    flush_out();
  }

  // An FX source's mid ([accounting]): its currency's rate, for the PnL totals from now on and for
  // the orders it prices. A book that stops being valid leaves the totals at the last rate (a loss
  // already booked stays measured) and refuses new exposure in that currency until it is back.
  FASTMM_NOINLINE void on_fx_mid(InstrumentId id, Price mid) noexcept {
    const std::size_t c = cfg_.fx.prices[id.value] - 1U;
    positions_.set_rate(c, FxRate::from_mid(mid, cfg_.fx.sources[c].invert));
    risk_.on_fx_book(c, true);
  }

  void on_trade(const TradeMsg& t) noexcept {
    ++stats_.trades;
    const InstrumentId id = t.hdr.instrument;
    const bool known_instrument = instruments_.contains(id);
    if (known_instrument) {
      risk_.on_trade(id, t.price);
      if (queue_.any(id)) queue_on_trade(t);
    }
    track_feed_lag(t.hdr);
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    if constexpr (has_hook(Hook::Trade)) {
      if (FASTMM_LIKELY(known_instrument)) {
        strategy_.on_trade(ctx_, id, t);
        record_strategy_hop(t2);
      }
    }
    flush_out();
  }

  void on_book_ticker(const BookTickerMsg& m) noexcept {
    track_feed_lag(m.hdr);
    if (queue_.enabled()) queue_on_ticker(m);
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    if constexpr (has_hook(Hook::BookTicker)) {
      const InstrumentId id = m.hdr.instrument;
      if (FASTMM_LIKELY(instruments_.contains(id))) {
        strategy_.on_book_ticker(ctx_, id, m);
        record_strategy_hop(t2);
      }
    }
    flush_out();
  }

  void on_option_ticker(const OptionTickerMsg& m) noexcept {
    track_feed_lag(m.hdr);
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    if constexpr (has_hook(Hook::OptionTicker)) {
      const InstrumentId id = m.hdr.instrument;
      if (FASTMM_LIKELY(instruments_.contains(id))) {
        strategy_.on_option_ticker(ctx_, id, m);
        record_strategy_hop(t2);
      }
    }
    flush_out();
  }

  // ---- store records ---------------------------------------------------------------------------
  // What a storage backend turns into rows (core/record_stream.hpp). Never on the risk path: a
  // full ring drops the record and counts it, and nothing here can block or fail the session.
  // hdr.engine_ts is the engine clock, which is the wall clock in a live session.

  void account_record(bool ok) noexcept {
    if (FASTMM_LIKELY(ok)) {
      ++stats_.records_written;
    } else {
      ++stats_.records_dropped;
    }
  }

  void emit_position(InstrumentId id) noexcept {
    if (!records_.enabled() || !instruments_.contains(id)) return;
    PositionRecord r;
    records_.init(r, RecordType::Position, id, instruments_.get(id).venue, now_, now_);
    r.pos = positions_.get(id);
    r.total_realized = positions_.total_realized();
    r.total_unrealized = positions_.total_unrealized();
    r.total_fees = positions_.total_fees();
    r.pnl_carry = cfg_.pnl_carry;
    r.funding = positions_.funding(id);
    r.total_funding = positions_.total_funding();
    account_record(records_.put(r.hdr));
  }

  void emit_order(const OmsUpdate& u) noexcept {
    if (!records_.enabled() || !u.known) return;
    // A cancel-replace renames the OMS record in place, so the id it replaced never reports a
    // terminal state of its own. Close it here, or a store shows it open forever.
    if (u.replaced_cl_ord_id.valid()) {
      OrderRecord old;
      records_.init(old, RecordType::Order, u.order.instrument, u.order.venue, now_, now_);
      old.hdr.flags |= RecordHeader::kTerminal;
      old.hdr.aux[0] = static_cast<std::uint8_t>(u.prev);
      old.hdr.aux[2] = 1;
      old.order = u.order;
      old.order.cl_ord_id = u.replaced_cl_ord_id;
      account_record(records_.put(old.hdr));
    }
    OrderRecord r;
    records_.init(r, RecordType::Order, u.order.instrument, u.order.venue, now_, now_);
    if (u.terminal) r.hdr.flags |= RecordHeader::kTerminal;
    r.hdr.aux[0] = static_cast<std::uint8_t>(u.prev);
    r.hdr.aux[1] = static_cast<std::uint8_t>(u.action);
    r.order = u.order;
    account_record(records_.put(r.hdr));
  }

  void emit_kill(KillReason reason, VenueId venue, bool per_venue) noexcept {
    if (!records_.enabled()) return;
    KillRecord r;
    records_.init(r, RecordType::Kill, InstrumentId{}, venue, now_, now_);
    if (per_venue) r.hdr.flags |= RecordHeader::kVenue;
    r.reason = reason;
    r.kill_flags = risk_.kill_flags();
    r.kills = stats_.kills;
    r.venue_kills = stats_.venue_kills;
    r.realized = positions_.total_realized();
    r.unrealized = positions_.total_unrealized();
    r.fees = positions_.total_fees();
    r.pnl_carry = cfg_.pnl_carry;
    account_record(records_.put(r.hdr));
  }

  // A fill the venue reported (msg != nullptr) or one the engine booked from a cum_qty jump.
  void emit_fill(InstrumentId id,
                 Side side,
                 Price px,
                 Qty qty,
                 Qty booked,
                 Notional fee,
                 FeeAsset fee_asset,
                 const OmsUpdate& u,
                 const OrderFillMsg* msg) noexcept {
    if (!records_.enabled() || !instruments_.contains(id)) return;
    FillRecord r;
    records_.init(r, RecordType::Fill, id, instruments_.get(id).venue, now_, now_);
    if (msg == nullptr) r.hdr.flags |= RecordHeader::kSynthetic;
    if (u.action == OmsAction::LateFill) r.hdr.flags |= RecordHeader::kLate;
    if (u.action == OmsAction::UnknownFill) r.hdr.flags |= RecordHeader::kUnknown;
    if (u.terminal) r.hdr.flags |= RecordHeader::kTerminal;
    r.cl_ord_id = msg != nullptr ? msg->cl_ord_id : u.order.cl_ord_id;
    r.price = px;
    r.qty = qty;
    r.booked_qty = booked;
    r.cum_qty = msg != nullptr ? msg->cum_qty : u.order.cum_qty;
    r.leaves_qty = msg != nullptr ? msg->leaves_qty : u.order.leaves_qty();
    r.fee = fee;
    r.fee_amount = msg != nullptr ? msg->fee : Notional{};
    const Position& p = positions_.get(id);
    r.position_qty = p.qty;
    r.position_avg_px = p.avg_px;
    r.position_realized = p.realized;
    r.position_fees = p.fees;
    r.side = side;
    r.liquidity = msg != nullptr ? msg->liquidity : Liquidity::Unknown;
    r.fee_asset = fee_asset;
    r.venue_order_id = msg != nullptr ? msg->venue_order_id : u.order.venue_order_id;
    if (msg != nullptr) {
      r.exec_id = msg->exec_id;
      r.hdr.exch_ts = msg->hdr.exch_ts;
    }
    account_record(records_.put(r.hdr));
    emit_position(id);
  }

  void emit_funding(const FundingMsg& m) noexcept {
    if (!records_.enabled()) return;
    const InstrumentId id = m.hdr.instrument;
    FundingRecord r;
    records_.init(r, RecordType::Funding, id, instruments_.get(id).venue, now_, now_);
    if ((m.flags & FundingMsg::kReplayed) != 0) r.hdr.flags |= RecordHeader::kReplayed;
    r.hdr.exch_ts = m.hdr.exch_ts;
    r.amount = m.amount;
    const Position& p = positions_.get(id);
    r.position_qty = p.qty;
    r.position_realized = p.realized;
    r.position_funding = positions_.funding(id);
    r.total_funding = positions_.total_funding();
    r.funding_id = m.funding_id;
    r.asset = m.asset;
    account_record(records_.put(r.hdr));
    emit_position(id);
  }

  // ---- order events -------------------------------------------------------------------------

  // A venue message reported more filled quantity than the fills we received (a private-stream
  // outage, or a reconciliation snapshot): book the difference. The message does not say what the
  // missing quantity traded at, so the order's own price is used and no fee is booked. That is an
  // estimate, and it stays one only until an execution from the venue's trade history names it
  // (OmsUpdate::corrected_qty); a venue that cannot be asked
  // (VenueCapabilities::executions = false) leaves it standing, which is what
  // EngineStats::estimated_reconciles counts.
  // The strategy's on_fill hook does not fire for these: there is no OrderFillMsg behind them.
  void book_missed_fill(const OmsUpdate& u) noexcept {
    if (u.missed_qty.is_zero()) return;
    ++stats_.synthetic_fills;
    if (FASTMM_UNLIKELY(!instruments_.contains(u.order.instrument))) return;
    const Instrument& inst = instruments_.get(u.order.instrument);
    FASTMM_LOG_ERROR(
        "order {} was {} filled while we were not listening: booking {} at its own price {}",
        encode_cl_ord_id(u.order.cl_ord_id),
        inst.symbol,
        u.missed_qty,
        u.order.price);
    positions_.on_fill(
        u.order.instrument, u.order.side, u.order.price, u.missed_qty, Notional{}, inst);
    emit_fill(u.order.instrument,
              u.order.side,
              u.order.price,
              u.missed_qty,
              u.missed_qty,
              Notional{},
              FeeAsset::Quote,
              u,
              nullptr);
    if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
  }

  // ---- execution view: own quantity and queue position ------------------------------------------

  [[nodiscard]] bool transport_own_in_feed(VenueId v) const noexcept {
    if constexpr (requires {
                    { transport_.own_in_feed(v) } -> std::same_as<bool>;
                  }) {
      return transport_.own_in_feed(v);
    } else {
      return false;
    }
  }
  [[nodiscard]] bool own_venue(VenueId v) const noexcept {
    return (own_venues_ & venue_mask(v)) != 0;
  }
  // Order events and our outbound New / Replace, for OwnQuantity (live venues only).
  void own_inbound(const EventHeader& h) noexcept {
    if (own_ != nullptr && own_venue(h.venue)) [[unlikely]]
      own_->on_inbound(h);
  }
  void own_outbound(const EventHeader& h) noexcept {
    if (own_ != nullptr && own_venue(h.venue)) [[unlikely]]
      own_->on_outbound(h);
  }
  // Queue position updates, out of line: the market-data handlers keep only the check.
  FASTMM_NOINLINE void queue_on_book(const BookDeltaMsg& d, const Book& b) noexcept {
    const InstrumentId id = d.hdr.instrument;
    queue_.on_book(d, b, [&](Side s, Price px) { return own_qty(id, s, px, b.last_update()); });
  }
  FASTMM_NOINLINE void queue_on_trade(const TradeMsg& t) noexcept { queue_.on_trade(t, oms_); }
  FASTMM_NOINLINE void queue_on_ticker(const BookTickerMsg& m) noexcept {
    const InstrumentId id = m.hdr.instrument;
    if (!instruments_.contains(id)) return;
    queue_.on_ticker(
        m, books_[id.value], [&](Side s, Price p, Timestamp t) { return own_qty(id, s, p, t); });
  }
  // What the queue model places an order behind: the displayed quantity at its price less our
  // own, capped by a BookTicker newer than the depth book.
  [[nodiscard]] Qty queue_shown(const Order& o) const noexcept {
    const Book& b = books_[o.instrument.value];
    const Qty shown = level_qty(b, o.side, o.price);
    const Qty own = own_qty(o.instrument, o.side, o.price);
    return queue_.at_placement(
        own >= shown ? Qty{} : shown - own, o, b, [&](Side s, Price p, Timestamp t) {
          return own_qty(o.instrument, s, p, t);
        });
  }
  // Queue position and own quantity after an OMS update: a resting order enters the queue model,
  // a terminal one leaves it.
  void exec_view_update(const OmsUpdate& u, const EventHeader& h) noexcept {
    if (u.terminal) {
      queue_.remove(u.slot);
      if (own_ != nullptr && own_venue(u.order.venue)) [[unlikely]]
        own_->on_gone(u.order.cl_ord_id, h.exch_ts.valid() ? h.exch_ts : h.recv_ts);
      return;
    }
    if (queue_.enabled() && u.handle.valid() && resting(u.order.state) &&
        !queue_.tracked(u.handle) && instruments_.contains(u.order.instrument))
      queue_.place(u.handle, u.order, queue_shown(u.order));
  }

  // An order rests at the venue from its ack to its terminal state; a cancel or replace in flight
  // does not take it off the book.
  [[nodiscard]] static constexpr bool resting(OrderState st) noexcept {
    return st != OrderState::PendingNew && !is_terminal(st);
  }

  // The venue stopped holding an order that still had working quantity and did not say whether it
  // was cancelled or filled. Nothing can be booked from that, so say it loudly: the position is
  // right only if the order was cancelled.
  void report_unresolved(const OmsUpdate& u) noexcept {
    if (u.unresolved_qty.is_zero()) return;
    ++stats_.unresolved_orders;
    FASTMM_LOG_ERROR(
        "order {} left the venue's open orders with {} still working and no fill or cancel to "
        "explain it: {}",
        encode_cl_ord_id(u.order.cl_ord_id),
        u.unresolved_qty,
        reconcile_exact_ ? "the venue's executions were replayed first, so it was cancelled"
                         : "the venue could not be asked what it filled, so the position may be "
                           "short by that much");
  }

  void after_oms_update(const OmsUpdate& u, const EventHeader& h) noexcept {
    if (u.known) {
      const int delta =
          static_cast<int>(resting(u.order.state)) - static_cast<int>(resting(u.prev));
      if (delta != 0) presence_.on_live_change(u.order.instrument, u.order.side, delta, now_);
      if (u.changed) exec_view_update(u, h);
    }
    book_missed_fill(u);
    report_unresolved(u);
    if (u.action == OmsAction::CancelUnknown) {
      cancel_unknown(h, u);
    }
    if (u.action == OmsAction::ReconcileNeeded) request_reconcile(u);
    if (u.known && u.handle.valid() && instruments_.contains(u.order.instrument)) {
      Placer place{this};
      quotes_.on_order_update(u, instruments_.get(u.order.instrument), oms_, now_, place);
    } else if (u.terminal && instruments_.contains(u.order.instrument)) {
      Placer place{this};
      quotes_.on_order_update(u, instruments_.get(u.order.instrument), oms_, now_, place);
    }
    if (u.changed) {
      emit_order(u);
      if constexpr (has_hook(Hook::OrderUpdate)) strategy_.on_order_update(ctx_, u);
    }
    flush_out();
  }

  void on_order_ack(const OrderAckMsg& m) noexcept {
    own_inbound(m.hdr);
    // A replace keeps its place in the queue at the same price and no more than the leaves (the
    // simulator's rule); otherwise the order joins the back again at its new price.
    bool keep = false;
    if (const Handle<Order> h = queue_.enabled() ? oms_.find(m.cl_ord_id) : Handle<Order>{};
        queue_.tracked(h)) {
      const Order& o = oms_.get(h);
      keep = o.state == OrderState::PendingReplace && m.cl_ord_id == o.pending_cl_ord_id &&
             o.pending_price == o.price && o.pending_qty <= o.leaves_qty();
    }
    const OmsUpdate u = oms_.on_ack(m);
    if (u.changed && u.prev == OrderState::PendingNew && u.order.created.valid())
      health_.on_ack(u.order.venue, now_ - u.order.created, now_);
    if (u.changed && u.prev == OrderState::PendingReplace && !keep) queue_.remove(u.slot);
    after_oms_update(u, m.hdr);
  }
  void on_order_reject(const OrderRejectMsg& m) noexcept {
    own_inbound(m.hdr);
    const OmsUpdate u = oms_.on_reject(m);
    if (u.changed) {
      ++stats_.venue_rejects;
      stats_.venue_rejects_by_reason.add(m.reason);
      FASTMM_LOG_WARN(
          "order {} rejected: {} ({})", encode_cl_ord_id(m.cl_ord_id), m.reason, m.venue_code);
    }
    after_oms_update(u, m.hdr);
  }
  void on_cancel_ack(const OrderCancelAckMsg& m) noexcept {
    own_inbound(m.hdr);
    after_oms_update(oms_.on_cancel_ack(m), m.hdr);
  }
  void on_cancel_reject(const OrderCancelRejectMsg& m) noexcept {
    after_oms_update(oms_.on_cancel_reject(m), m.hdr);
  }
  void on_expired(const OrderExpiredMsg& m) noexcept {
    own_inbound(m.hdr);
    after_oms_update(oms_.on_expired(m), m.hdr);
  }

  void on_fill(const OrderFillMsg& f) noexcept {
    own_inbound(f.hdr);
    const OmsUpdate u = oms_.on_fill(f);
    if (u.action == OmsAction::Duplicate) return;
    ++stats_.fills;
    if (FASTMM_UNLIKELY((f.flags & OrderFillMsg::kReplayed) != 0)) ++stats_.replayed_fills;
    // A fill for an order that is no longer open (late fill) or was never ours still changes the
    // position: without an instrument in the OMS snapshot, the fill itself says what was traded.
    const bool from_order = u.order.instrument.valid();
    const InstrumentId id = from_order ? u.order.instrument : f.hdr.instrument;
    const Side side = from_order ? u.order.side : f.side;
    const bool known_instrument = instruments_.contains(id);
    Qty booked = f.qty;
    Notional fee = f.fee;
    // A parser that leaves fee_asset unset would let a byte of its scratch buffer decide how the
    // fill is booked. An out-of-range value is booked as Quote, the neutral case, and counted.
    FeeAsset fee_asset = f.fee_asset;
    if (FASTMM_UNLIKELY(fee_asset > FeeAsset::Other)) {
      if (stats_.invalid_fee_assets++ == 0) {
        FASTMM_LOG_ERROR("fill with fee_asset {} booked as quote (first on order {})",
                         static_cast<unsigned>(fee_asset),
                         encode_cl_ord_id(f.cl_ord_id));
      }
      fee_asset = FeeAsset::Quote;
    }
    if (FASTMM_LIKELY(known_instrument)) {
      const Instrument& inst = instruments_.get(id);
      // Commission in the base asset changes what we hold (a buy receives qty - fee, a sell
      // delivers qty + fee) and costs fee * price in quote terms; commission in another asset
      // (BNB) cannot be valued here and is counted instead of being booked as quote.
      if (fee_asset == FeeAsset::Base) {
        const Qty fee_base = Qty::from_raw(f.fee.raw);
        fee = inst.notional(f.price, fee_base);
        const Qty held = side == Side::Buy ? f.qty - fee_base : f.qty + fee_base;
        if (held.raw > 0) booked = held;
      } else if (fee_asset == FeeAsset::Other) {
        if (stats_.unconverted_fees++ == 0) {
          FASTMM_LOG_WARN(
              "fill commission in an asset other than base or quote is not included in "
              "fees or positions (first on order {})",
              encode_cl_ord_id(f.cl_ord_id));
        }
        fee = Notional{};
      }
      // Quantity a synthetic fill already put in the position is not booked again: this execution
      // names what the estimate could only guess, so it replaces its price and its fee. The rest of
      // the execution is an ordinary fill; the fee belongs to the correction, which adds it whole.
      const Notional exec_fee = fee;  // what this execution cost, whichever path books it
      if (FASTMM_UNLIKELY(!u.corrected_qty.is_zero())) {
        const Qty corrected = u.corrected_qty < booked ? u.corrected_qty : booked;
        ++stats_.corrected_fills;
        FASTMM_LOG_INFO("order {}: execution {} of {} at {} replaces the estimate booked at {}",
                        encode_cl_ord_id(f.cl_ord_id),
                        f.exec_id,
                        corrected,
                        f.price,
                        u.synthetic_px);
        positions_.correct_fill(id, side, u.synthetic_px, f.price, corrected, fee, inst);
        booked = booked - corrected;
        fee = Notional{};
      }
      if (booked.is_positive()) positions_.on_fill(id, side, f.price, booked, fee, inst);
      emit_fill(id, side, f.price, f.qty, booked, exec_fee, fee_asset, u, &f);
      if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
    } else if (stats_.unknown_instrument_fills++ == 0) {
      FASTMM_LOG_WARN("fill on instrument {} outside the instrument table is not booked", id.value);
    }
    if (u.action == OmsAction::UnknownFill) {
      // An execution replayed from before this session (a carried-over position, or a trade made
      // outside FastMM) names no order of ours; it is booked, and it is not an error.
      if ((f.flags & OrderFillMsg::kReplayed) != 0) {
        ++stats_.replayed_foreign_fills;
        FASTMM_LOG_INFO("execution {} of {} @ {} on no order of this session booked from the venue",
                        f.exec_id,
                        f.qty,
                        f.price);
      } else {
        FASTMM_LOG_ERROR(
            "fill for unknown order {} qty {} @ {}", encode_cl_ord_id(f.cl_ord_id), f.qty, f.price);
      }
    }
    if constexpr (has_hook(Hook::Fill)) {
      if (FASTMM_LIKELY(known_instrument)) {
        Fill fill;
        fill.instrument = id;
        fill.side = side;
        fill.price = f.price;
        fill.qty = f.qty;
        fill.position_delta = side == Side::Buy ? booked : -booked;
        fill.fee = fee;
        fill.fee_converted = fee_asset != FeeAsset::Other;
        fill.liquidity = f.liquidity;
        fill.known = u.known;
        fill.late = u.action == OmsAction::LateFill;
        fill.order_done = u.terminal;
        fill.update = u.known ? &u : nullptr;
        fill.msg = &f;
        strategy_.on_fill(ctx_, fill);
      }
    }
    after_oms_update(u, f.hdr);
  }

  // A funding payment is realized PnL of its instrument, in the settlement currency it names, and
  // counts against max_loss at once like a fill. Once per (venue id, instrument): the private
  // stream and a replay of the venue's history can both deliver it. The strategy is not called: it
  // reads the change in position(id).realized like any other PnL.
  FASTMM_NOINLINE void on_funding(const FundingMsg& m) noexcept {
    const InstrumentId id = m.hdr.instrument;
    if (!instruments_.contains(id)) {
      if (funding_->stats.unbooked++ == 0)
        FASTMM_LOG_WARN("funding on instrument {} outside the instrument table is not booked",
                        id.value);
      return;
    }
    if (!m.funding_id.empty()) {
      const std::uint64_t key =
          m.funding_id.hash() ^ (static_cast<std::uint64_t>(id.value) * 0x9E3779B97F4A7C15ULL);
      if (!funding_->seen.assign(key, 1)) {
        ++funding_->stats.duplicates;
        return;
      }
    }
    const Instrument& inst = instruments_.get(id);
    if (!m.asset.empty() && !inst.settlement_ccy().empty() &&
        !same_currency(m.asset.view(), inst.settlement_ccy())) {
      ++funding_->stats.unbooked;
      FASTMM_LOG_ERROR("funding {} of {} {} on {} is not in its settlement currency {}: not booked",
                       m.funding_id.view(),
                       m.amount,
                       m.asset.view(),
                       inst.symbol.view(),
                       inst.settlement_ccy());
      return;
    }
    ++funding_->stats.payments;
    positions_.on_funding(id, m.amount);
    emit_funding(m);
    if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
  }

  void on_position_update(const PositionUpdateMsg& m) noexcept {
    if (!instruments_.contains(m.hdr.instrument)) return;
    positions_.set(m.hdr.instrument, m.qty, m.avg_px, instruments_.get(m.hdr.instrument));
    emit_position(m.hdr.instrument);
    if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
  }

  // ---- parameters -----------------------------------------------------------------------------

  void on_param_update(const ParamUpdateMsg& m) noexcept {
    ++stats_.param_updates;
    if constexpr (requires { strategy_.apply_param_update(m); }) strategy_.apply_param_update(m);
    if (cfg_.max_param_age.ns > 0) {
      params_stale_ = false;
      param_deadline_ = now_ + cfg_.max_param_age;
      param_deadline_armed_ = true;
      if (param_timer_.valid()) static_cast<void>(timers_.cancel(param_timer_));
      // An invalid id (timer pool full) leaves the check before each event and timer.
      param_timer_ = timers_.add(now_, cfg_.max_param_age, false);
    }
    if constexpr (has_hook(Hook::Params)) strategy_.on_params(ctx_);
    flush_out();
  }

  FASTMM_FORCE_INLINE void check_param_age() noexcept {
    if (FASTMM_UNLIKELY(param_deadline_armed_ && now_ >= param_deadline_)) expire_params();
  }
  // Quoting stays disabled until the next ParamUpdate; on_quoting(false) follows the event or
  // timer.
  FASTMM_NOINLINE void expire_params() noexcept {
    param_deadline_armed_ = false;
    params_stale_ = true;
    ++stats_.param_expiries;
    FASTMM_LOG_WARN(
        "no parameter update for {} ms (max_param_age_ms): quotes pulled until the next",
        cfg_.max_param_age.millis());
    pull_all_quotes();
  }

  // ---- control / connection / reconcile ------------------------------------------------------

  void on_control(const ControlMsg& c) noexcept {
    switch (c.command) {
      case ControlCommand::Stop:
        stop();
        break;
      // With an instrument or a venue in the header only that scope stops quoting and has its
      // quotes pulled; without either, quoting stops for the whole session (as it always did).
      case ControlCommand::PullQuotes:
        if (c.hdr.instrument.valid()) {
          pull_instrument(c.hdr.instrument);
        } else if (c.hdr.venue.valid()) {
          pull_venue(c.hdr.venue);
        } else {
          quoting_enabled_ = false;
          pull_all_quotes();
        }
        break;
      case ControlCommand::ResumeQuotes:
        if (c.hdr.instrument.valid()) {
          if (instruments_.contains(c.hdr.instrument)) inst_pulled_[c.hdr.instrument.value] = false;
        } else if (c.hdr.venue.valid()) {
          venue_pulled_ &= ~venue_mask(c.hdr.venue);
        } else {
          // A resume without a scope is the operator taking the session back: every scoped pull is
          // cleared and a flatten that is still running is abandoned (the strategy quotes again).
          quoting_enabled_ = true;
          inst_pulled_.fill(false);
          venue_pulled_ = 0;
          if (flatten_state_ == FlattenState::Working) end_flatten(FlattenState::Stopped);
        }
        break;
      case ControlCommand::Flatten:
        begin_flatten(c.hdr.instrument, static_cast<std::int64_t>(c.arg));
        break;
      case ControlCommand::SetLimits:
        // The payload sits past the ControlMsg prefix; a short record (another producer, an older
        // journal) is ignored rather than read out of bounds.
        if (c.hdr.len >= sizeof(ControlLimitsMsg)) {
          const auto& m = msg_cast<ControlLimitsMsg>(&c.hdr);
          risk_.set_limits(m.limits, now_);
          feed_lag_limit_ = milliseconds(m.limits.max_feed_lag_ms);
          FASTMM_LOG_WARN(
              "risk limits replaced by the operator: max_position={} max_order_qty={} "
              "price_collar_bps={} orders_per_sec={}",
              m.limits.max_position,
              m.limits.max_order_qty,
              m.limits.price_collar_bps,
              m.limits.orders_per_sec);
        }
        break;
      case ControlCommand::TripKill:
        risk_.trip();
        on_kill(KillReason::Requested);
        break;
      case ControlCommand::TripVenueKill:
        on_venue_kill(c.hdr.venue, static_cast<KillReason>(static_cast<std::uint8_t>(c.arg)));
        break;
      case ControlCommand::ResetKill:
        // With a venue in the header only that venue's bit is cleared (it recovered and the rest
        // kept trading); without one, every bit, global included.
        if (c.hdr.venue.valid()) {
          risk_.reset_venue(c.hdr.venue);
          venue_kill_reasons_[RiskEngine::venue_slot(c.hdr.venue)] = KillReason::None;
        } else {
          risk_.reset();
          kill_reason_ = KillReason::None;
          venue_kill_reasons_.fill(KillReason::None);
        }
        quoting_enabled_ = true;
        publish_live(latency_pub_.load());
        break;
      case ControlCommand::RecalibrateTsc:
        // The calibrator publishes new calibrations and step() picks them up anyway; this
        // applies a just-published one before the rest of the step runs.
        refresh_clock();
        break;
      case ControlCommand::Reload:
        break;  // not implemented: configuration changes need a restart
      case ControlCommand::FlushStats:
        publish_latency(now());
        break;
      case ControlCommand::Reconcile:
        break;  // engine output, see request_reconcile
    }
  }

  void on_connection_state(const ConnectionStateMsg& m) noexcept {
    const VenueId venue = m.hdr.venue;
    if (m.state != ConnState::Live) {
      for (const Instrument& inst : instruments_) {
        if (inst.venue != venue) continue;
        if (m.channel == 0) {
          books_[inst.id.value].clear();
          if (FASTMM_UNLIKELY(cfg_.fx.prices[inst.id.value] != 0))
            risk_.on_fx_book(cfg_.fx.prices[inst.id.value] - 1U, false);
        }
        pull_quotes(inst.id);
      }
    }
    if constexpr (has_hook(Hook::Connection)) strategy_.on_connection(ctx_, m);
    flush_out();
  }

  void on_reconcile(const ReconcileMsg& m) noexcept {
    own_inbound(m.hdr);
    switch (m.kind) {
      case ReconcileMsg::Kind::Begin: {
        reconciling_ = true;
        reconcile_exact_ = (m.flags & ReconcileMsg::kExecutionsExact) != 0;
        if (reconcile_exact_) {
          ++stats_.exact_reconciles;
        } else {
          ++stats_.estimated_reconciles;
          FASTMM_LOG_WARN(
              "reconciling without the venue's executions: filled quantity this snapshot reports "
              "that no fill covered is an estimate, and an order it drops cannot be told from one "
              "that filled");
        }
        Placer place{this};
        for (const Instrument& inst : instruments_)
          quotes_.pull_quotes(inst, oms_, place, /*keep_desired=*/true);  // resumed at End
        flush_out();
        oms_.reconcile_begin(m.hdr.venue,
                             (m.flags & ReconcileMsg::kSentWatermark) != 0
                                 ? std::optional<ClientOrderId>(m.sent_watermark)
                                 : std::nullopt);
        break;
      }
      case ReconcileMsg::Kind::OpenOrder: {
        const OmsUpdate u = oms_.reconcile_open_order(m);
        after_oms_update(u, m.hdr);
        break;
      }
      case ReconcileMsg::Kind::Position:
        if (instruments_.contains(m.hdr.instrument)) {
          positions_.set(
              m.hdr.instrument, m.position_qty, m.avg_px, instruments_.get(m.hdr.instrument));
          emit_position(m.hdr.instrument);  // a session killed before it trades keeps it too
          if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
        }
        break;
      case ReconcileMsg::Kind::End:
        // Orders the venue no longer has end like any other update, so the quote manager frees
        // their slots too.
        oms_.reconcile_end([&](const OmsUpdate& u) { after_oms_update(u, m.hdr); }, m.hdr.venue);
        reconciling_ = false;
        resume_quotes();
        break;
    }
  }

  // The strategy is not asked again after a reconciliation (BasicMM requotes on a mid move), so
  // the quotes paused at Begin are placed again here. An instrument that cannot quote now (kill
  // switch, quoting disabled, no valid book) is pulled for good instead: its kept quotes are stale.
  void resume_quotes() noexcept {
    Placer place{this};
    const Timestamp now = now_;
    for (const Instrument& inst : instruments_) {
      if (!quotes_.resumable(inst.id)) continue;
      if (quoting_enabled(inst.id) && books_[inst.id.value].is_valid()) {
        quotes_.resume(inst, oms_, now, place);
      } else {
        quotes_.pull_quotes(inst, oms_, place);
      }
    }
    flush_out();
  }

  // ---- operator scopes and flatten -------------------------------------------------------------

  [[nodiscard]] static constexpr std::uint32_t venue_mask(VenueId v) noexcept {
    return v.value < 32U ? (1U << v.value) : 0U;
  }
  [[nodiscard]] bool venue_is_pulled(VenueId v) const noexcept {
    return (venue_pulled_ & venue_mask(v)) != 0;
  }
  // One instrument stops quoting and its quotes go; the rest of the session keeps trading.
  void pull_instrument(InstrumentId id) noexcept {
    if (!instruments_.contains(id)) return;
    inst_pulled_[id.value] = true;
    pull_quotes(id);
  }
  void pull_venue(VenueId v) noexcept {
    venue_pulled_ |= venue_mask(v);
    enter_api();
    Placer place{this};
    for (const Instrument& inst : instruments_) {
      if (inst.venue == v) quotes_.pull_quotes(inst, oms_, place);
    }
    flush_out();
  }

  // The engine flattens by itself: the strategy may be what broke, so it is not asked and not
  // told. Quoting stops in the scope, its working orders are cancelled, and a repeating engine
  // timer sends reduce-only slices through the touch until the position is gone (flatten_tick).
  void begin_flatten(InstrumentId scope, std::int64_t slippage_bps) noexcept {
    if (scope.valid() && !instruments_.contains(scope)) return;  // not ours to flatten
    flatten_scope_ = scope;
    flatten_bps_ = slippage_bps > 0 ? slippage_bps : cfg_.flatten_slippage_bps;
    flatten_state_ = FlattenState::Working;
    flatten_deadline_ =
        cfg_.flatten_timeout.ns > 0 ? now_ + cfg_.flatten_timeout : Timestamp::max();
    ++stats_.flattens;
    enter_api();
    if (scope.valid()) {
      pull_instrument(scope);
      StaticVector<Handle<Order>, kMaxOpenOrders> handles;
      oms_.for_each_open_order(scope, [&](Handle<Order> h, const Order& o) {
        if (o.is_working()) static_cast<void>(handles.push_back(h));
      });
      for (Handle<Order> h : handles) static_cast<void>(submit_cancel(h));
      flush_out();
    } else {
      quoting_enabled_ = false;
      pull_all_quotes();
      mass_cancel();
    }
    const std::string_view what =
        scope.valid() ? instruments_.get(scope).symbol.view() : std::string_view("all instruments");
    FASTMM_LOG_WARN(
        "flatten requested ({}): quoting is off there, its orders are cancelled and reduce-only "
        "orders go out {} bps through the touch every {} ms until it is flat",
        what,
        flatten_bps_,
        cfg_.flatten_interval.millis());
    if (!flatten_timer_.valid() && cfg_.flatten_interval.ns > 0)
      flatten_timer_ = timers_.add(now_, cfg_.flatten_interval, /*repeat=*/true);
    flatten_tick();  // do not wait a whole interval for the first slice
  }

  // Ends the flatten mode; the scope keeps its pull (quoting stays off until the operator
  // resumes), so nothing starts quoting into whatever broke.
  void end_flatten(FlattenState how) noexcept {
    if (flatten_timer_.valid()) {
      static_cast<void>(timers_.cancel(flatten_timer_));
      flatten_timer_ = TimerId{};
    }
    flatten_state_ = how;
    if (how == FlattenState::Flat) {
      FASTMM_LOG_WARN("flatten finished: flat after {} order(s)", stats_.flatten_orders);
    } else if (how == FlattenState::TimedOut) {
      FASTMM_LOG_ERROR(
          "flatten gave up after {} ms ([engine] flatten_timeout_ms) with {} instrument(s) still "
          "holding a position; quoting stays off until an operator resumes it",
          cfg_.flatten_timeout.millis(),
          flatten_left());
    }
    publish_live(latency_pub_.load());
  }

  // Instruments in the flatten's scope that still hold a position.
  [[nodiscard]] std::uint32_t flatten_left() const noexcept {
    std::uint32_t n = 0;
    for (const Instrument& inst : instruments_) {
      if (flatten_scope_.valid() && inst.id != flatten_scope_) continue;
      if (!positions_.get(inst.id).flat()) ++n;
    }
    return n;
  }

  // One pass over the scope: send what is missing, then decide whether it is done or out of time.
  void flatten_tick() noexcept {
    if (flatten_state_ != FlattenState::Working) return;
    enter_api();
    bool left = false;
    for (const Instrument& inst : instruments_) {
      if (flatten_scope_.valid() && inst.id != flatten_scope_) continue;
      left = flatten_instrument(inst) || left;
    }
    flush_out();
    if (!left) {
      end_flatten(FlattenState::Flat);
    } else if (now_ >= flatten_deadline_) {
      end_flatten(FlattenState::TimedOut);
    }
  }

  // True while `inst` still has a position to work off. Only one slice is out at a time: the
  // previous one has to be filled, expired or cancelled before its successor is priced, so the
  // flatten cannot sell the same position twice. The strategy's own orders do not hold it up:
  // they were cancelled when the flatten started, and a cancel the venue never acknowledges
  // would otherwise block the flatten for good.
  bool flatten_instrument(const Instrument& inst) noexcept {
    const Position& p = positions_.get(inst.id);
    if (p.flat()) return false;
    if (const ClientOrderId prev = flatten_order_[inst.id.value]; prev.valid()) {
      const Handle<Order> h = oms_.find(prev);
      if (h.valid() && oms_.is_live(h) && oms_.get(h).is_open()) return true;  // still in flight
    }
    const Side side = p.qty.raw > 0 ? Side::Sell : Side::Buy;
    const Book& b = books_[inst.id.value];
    if (!b.is_valid()) return true;  // no touch to price against; try again next tick
    const Price touch = side == Side::Sell ? b.best_bid().price : b.best_ask().price;
    if (!touch.is_positive()) return true;
    const Price slip = apply_bps(touch, flatten_bps_);
    const Price px = inst.round_price(side == Side::Sell ? touch - slip : touch + slip, side);
    if (!px.is_positive()) return true;
    Qty qty = p.qty.abs();
    if (risk_.limits().max_order_qty.is_positive() && qty > risk_.limits().max_order_qty)
      qty = risk_.limits().max_order_qty;  // one slice per tick until the rest is gone
    qty = inst.round_qty(qty);
    if (!inst.valid_qty(qty)) {
      // Less than one lot (or than the venue's minimum) is left: no order can move it.
      FASTMM_LOG_WARN("flatten leaves {} on {}: below the tradable minimum", p.qty, inst.symbol);
      return false;
    }
    NewOrderRequest r{};
    r.instrument = inst.id;
    r.venue = inst.venue;
    r.side = side;
    r.type = OrderType::Limit;
    r.tif = TimeInForce::Ioc;  // no resting remainder: the next tick reprices against the touch
    r.post_only = false;
    r.reduce_only = true;
    r.price = px;
    r.qty = qty;
    if (const auto id = submit_new(r, /*flatten=*/true)) {
      flatten_order_[inst.id.value] = *id;
      ++stats_.flatten_orders;
    }
    return true;
  }

  // The global flag is already set. KillReason::Requested: the control thread asked for it
  // (shutdown, operator); any other reason is a risk limit or an internal failure, which is an
  // error (fastmm-live exits or keeps running per [engine] on_kill). The first reason is kept.
  void on_kill(KillReason reason) noexcept {
    ++stats_.kills;
    quoting_enabled_ = false;
    if (kill_reason_ == KillReason::None) kill_reason_ = reason;
    emit_kill(reason, VenueId::invalid(), /*per_venue=*/false);
    if (reason == KillReason::Requested) {
      FASTMM_LOG_WARN("kill switch requested (flags={:#x}); pulling quotes and cancelling all",
                      risk_.kill_flags());
    } else {
      FASTMM_LOG_ERROR("kill switch engaged ({}, flags={:#x}); pulling quotes and cancelling all",
                       reason,
                       risk_.kill_flags());
    }
    pull_all_quotes();
    mass_cancel();
    publish_live(latency_pub_.load());
  }
  // One venue is unusable: new orders to it are refused by risk (VenueKilled), its quotes are
  // pulled and its working orders cancelled (the connector may refuse the cancels; fastmm-live's
  // shutdown cancel-all still tries), and the other venues keep trading. When every venue that has
  // instruments is killed nothing can trade: that is a global kill (KillReason::AllVenuesKilled).
  void on_venue_kill(VenueId venue, KillReason reason) noexcept {
    if (risk_.venue_killed(venue)) return;  // the first reason stays
    risk_.trip_venue(venue);
    ++stats_.venue_kills;
    venue_kill_reasons_[RiskEngine::venue_slot(venue)] = reason;
    emit_kill(reason, venue, /*per_venue=*/true);
    FASTMM_LOG_ERROR(
        "venue {} kill switch engaged ({}, flags={:#x}); pulling its quotes and cancelling its "
        "orders, other venues keep trading",
        venue.value,
        reason,
        risk_.kill_flags());
    enter_api();
    Placer place{this};
    for (const Instrument& inst : instruments_) {
      if (inst.venue == venue) quotes_.pull_quotes(inst, oms_, place);
    }
    StaticVector<Handle<Order>, kMaxOpenOrders> handles;
    oms_.for_each_open_order([&](Handle<Order> h, const Order& o) {
      if (o.venue == venue && o.is_working()) static_cast<void>(handles.push_back(h));
    });
    for (Handle<Order> h : handles) static_cast<void>(submit_cancel(h));
    flush_out();
    if (!risk_.killed() && all_venues_killed()) {
      risk_.trip();
      on_kill(KillReason::AllVenuesKilled);  // publishes
      return;
    }
    publish_live(latency_pub_.load());
  }
  [[nodiscard]] bool all_venues_killed() const noexcept {
    bool any = false;
    for (const Instrument& inst : instruments_) {
      if (!risk_.venue_killed(inst.venue)) return false;
      any = true;
    }
    return any;
  }
  void on_journal_overflow() noexcept {
    // 5.5: journal ring full is fatal for determinism guarantees; trip and cancel everything.
    if (!risk_.killed()) {
      risk_.trip();
      on_kill(KillReason::JournalOverflow);
    }
  }

  // ---- timers ---------------------------------------------------------------------------------

  void on_timer_fired(TimerId id, std::uint64_t user_data) noexcept {
    latch_clock();
    set_event_origin(Cycles{}, Cycles{});  // no inbound message: sends carry no T0
    [[maybe_unused]] const bool quoting_before = quoting_enabled();
    fire_timer(id, user_data);
    if constexpr (has_hook(Hook::Quoting)) notify_quoting(quoting_before);
    unlatch_clock();
  }
  void fire_timer(TimerId id, std::uint64_t user_data) noexcept {
    ++stats_.timers_fired;
    const bool ack_timer = ack_timer_.valid() && id == ack_timer_;
    const bool flatten_timer = flatten_timer_.valid() && id == flatten_timer_;
    const bool engine_timer =
        ack_timer || flatten_timer || (param_timer_.valid() && id == param_timer_);
    // Journal a synthetic TimerMsg so replay reproduces the strategy's timer calls.
    if (journal_.enabled()) {
      TimerMsg t{};
      init_header(t, EventType::Timer);
      t.timer_id = id;
      t.engine = ack_timer ? kAckSweepTimer : flatten_timer ? kFlattenTimer : engine_timer ? 1 : 0;
      t.user_data = user_data;
      t.fire_ts = now_;
      t.hdr.flags |= EventHeader::kSynthetic;
      if (!journal_.record_at(t.hdr, now_)) {
        ++stats_.journal_overflows;
        on_journal_overflow();
      }
    }
    check_param_age();
    if (ack_timer) {
      sweep_acks();
    } else if (flatten_timer) {
      flatten_tick();
    } else if (engine_timer) {
      param_timer_ = TimerId{};  // a one-shot timer is freed once it has fired
      flush_out();
    } else {
      fire_strategy_timer(id, user_data);
    }
  }

  // Orders whose ack never came: cancel them. Cancels are always allowed, so this runs even while
  // the kill switch is engaged; a venue that never saw the order answers VenueUnknownOrder, which
  // ends it and frees its slot, its open quantity and its max_open_orders slot.
  void sweep_acks() noexcept {
    const Duration timeout = cfg_.ack_timeout;
    static_cast<void>(oms_.sweep_pending(now_, timeout, [&](Handle<Order> h, const Order& o) {
      ++stats_.ack_timeouts;
      FASTMM_LOG_WARN("order {} has no ack after {} ms: cancelling it",
                      encode_cl_ord_id(o.cl_ord_id),
                      timeout.millis());
      if (!oms_.request_cancel_unacked(h)) return;
      const Order& co = oms_.get(h);
      OutCancelMsg m{};
      init_header(m, EventType::OutCancel, co.instrument, co.venue);
      m.hdr.recv_ts = now_;
      m.cl_ord_id = co.cl_ord_id;
      m.venue_order_id = co.venue_order_id;
      queue_out(m.hdr);
      ++stats_.cancels_sent;
    }));
    flush_out();
  }
  void fire_strategy_timer(TimerId id, std::uint64_t user_data) noexcept {
    if constexpr (has_hook(Hook::Timer)) strategy_.on_timer(ctx_, id, user_data);
    flush_out();
  }

  // ---- outbound -------------------------------------------------------------------------------

  bool place_quote(QuoteAction& a) noexcept {
    switch (a.kind) {
      case QuoteActionKind::New: {
        NewOrderRequest r{};
        const Instrument& inst = instruments_.get(a.instrument);
        r.instrument = a.instrument;
        r.venue = inst.venue;
        r.side = a.side;
        r.type = a.post_only ? OrderType::PostOnly : OrderType::Limit;
        r.tif = TimeInForce::Gtc;
        r.post_only = a.post_only;
        r.price = a.price;
        r.qty = a.qty;
        r.user_tag = QuoteManager::make_tag(a.side, a.level);
        auto res = submit_new(r);
        if (!res) return false;
        a.cl_ord_id = *res;
        a.handle = oms_.find(*res);
        return a.handle.valid();
      }
      case QuoteActionKind::Cancel:
        return submit_cancel(a.handle).has_value();
      case QuoteActionKind::Replace:
        return submit_replace(a.handle, a.price, a.qty).has_value();
    }
    return false;
  }

  // `flatten`: the engine is working a position off (flatten_instrument). The order passes the
  // same checks as any other except two, both expressed as missing RiskInputs: the position cap
  // (a reduce-only slice exists to get under it) and self-trade prevention (the flatten cancelled
  // every order of its scope before the first slice, so the only resting orders it could cross are
  // already cancelled -- and a cancel the venue never acknowledges must not leave the position on).
  // The price collar, the fat-finger band, the size, notional and rate limits all still apply.
  Result<ClientOrderId, RejectReason> submit_new(const NewOrderRequest& req,
                                                 bool flatten = false) noexcept {
    if (FASTMM_UNLIKELY(!instruments_.contains(req.instrument)))
      return fail(RejectReason::InstrumentDisabled);
    const Instrument& inst = instruments_.get(req.instrument);
    const Timestamp now = now_;
    OrderIntent oi{req.instrument, inst.venue, req.side, req.type, req.price, req.qty, req.tif};
    RiskInputs in{now,
                  flatten ? nullptr : &positions_.get(req.instrument),
                  positions_.gross_exposure(),
                  positions_.net_exposure(),
                  oms_.open_qty(req.instrument, req.side),
                  oms_.open_count(req.instrument),
                  flatten ? Price{} : oms_.best_own_px(req.instrument, opposite(req.side)),
                  health_.gated(inst.venue, now)};
    const RejectReason rr = risk_.check_new(oi, inst, in);
    if (FASTMM_UNLIKELY(rr != RejectReason::None)) {
      ++stats_.risk_rejects;
      stats_.risk_rejects_by_reason.add(rr);
      log_risk_reject(rr, inst, req.side, req.price, req.qty, false);
      return fail(rr);
    }
    const ClientOrderId id = oms_.next_cl_ord_id();
    if (FASTMM_UNLIKELY(!id.valid())) return fail(on_ids_exhausted());
    NewOrderRequest r = req;
    r.venue = inst.venue;
    auto h = oms_.submit(r, id, now);
    if (FASTMM_UNLIKELY(!h)) return fail(h.error());
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, req.instrument, inst.venue);
    m.hdr.recv_ts = now;
    m.cl_ord_id = id;
    m.price = req.price;
    m.qty = req.qty;
    m.side = req.side;
    m.type = req.post_only && req.type == OrderType::Limit ? OrderType::PostOnly : req.type;
    m.tif = req.tif;
    m.reduce_only = req.reduce_only ? 1 : 0;
    own_outbound(m.hdr);
    queue_out(m.hdr);
    ++stats_.orders_sent;
    return id;
  }

  Result<void, RejectReason> submit_cancel(Handle<Order> h) noexcept {
    auto r = oms_.request_cancel(h);
    if (!r) return r;
    const Order& o = oms_.get(h);
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, o.instrument, o.venue);
    m.hdr.recv_ts = now_;
    m.cl_ord_id = o.cl_ord_id;
    m.venue_order_id = o.venue_order_id;
    queue_out(m.hdr);
    ++stats_.cancels_sent;
    return {};
  }

  Result<void, RejectReason> submit_replace(Handle<Order> h, Price px, Qty qty) noexcept {
    if (!oms_.is_live(h)) return fail(RejectReason::UnknownOrder);
    const Order& o = oms_.get(h);
    if (!o.is_working()) return fail(RejectReason::InvalidState);
    if (!transport_.supports_replace(o.venue)) return fail(RejectReason::VenueReject);
    const Instrument& inst = instruments_.get(o.instrument);
    const Timestamp now = now_;
    OrderIntent oi{o.instrument, o.venue, o.side, o.type, px, qty, o.tif};
    RiskInputs in{now,
                  &positions_.get(o.instrument),
                  positions_.gross_exposure(),
                  positions_.net_exposure(),
                  oms_.open_qty(o.instrument, o.side),
                  oms_.open_count(o.instrument),
                  oms_.best_own_px(o.instrument, opposite(o.side)),
                  health_.gated(o.venue, now)};
    const RejectReason rr = risk_.check_replace(oi, o, inst, in);
    if (FASTMM_UNLIKELY(rr != RejectReason::None)) {
      ++stats_.risk_rejects;
      stats_.risk_rejects_by_reason.add(rr);
      log_risk_reject(rr, inst, o.side, px, qty, true);
      return fail(rr);
    }
    const ClientOrderId new_id = oms_.next_cl_ord_id();
    if (FASTMM_UNLIKELY(!new_id.valid())) return fail(on_ids_exhausted());
    auto r = oms_.request_replace(h, new_id, px, qty, now);
    if (!r) return r;
    OutReplaceMsg m{};
    init_header(m, EventType::OutReplace, o.instrument, o.venue);
    m.hdr.recv_ts = now;
    m.cl_ord_id = new_id;
    m.orig_cl_ord_id = o.cl_ord_id;
    m.venue_order_id = o.venue_order_id;
    m.price = px;
    m.qty = qty;
    own_outbound(m.hdr);
    queue_out(m.hdr);
    ++stats_.replaces_sent;
    return {};
  }

  // Cold: the rate limiter and the log record stay out of the order path's inlined code. Only the
  // arguments are packed here; the sink thread formats them.
  FASTMM_NOINLINE void log_risk_reject(RejectReason rr,
                                       const Instrument& inst,
                                       Side side,
                                       Price px,
                                       Qty qty,
                                       bool replace) noexcept {
    std::uint64_t suppressed = 0;
    if (!reject_log_.admit(rr, now_, suppressed)) return;
    const std::string_view what = replace ? std::string_view("replace") : std::string_view("new");
    if (suppressed == 0) {
      FASTMM_LOG_WARN(
          "risk reject {} on {} order: {} {} {} @ {}", rr, what, inst.symbol, side, qty, px);
    } else {
      FASTMM_LOG_WARN("risk reject {} on {} order: {} {} {} @ {} ({} more suppressed)",
                      rr,
                      what,
                      inst.symbol,
                      side,
                      qty,
                      px,
                      suppressed);
    }
  }

  // The session's client order id sequence is used up. Reusing one would repeat an id within the
  // session and let a late venue message match the wrong order, so the session stops trading:
  // restart it (the epoch file gives the next session a fresh sequence).
  FASTMM_NOINLINE RejectReason on_ids_exhausted() noexcept {
    if (!risk_.killed()) {
      risk_.trip();
      on_kill(KillReason::OrderIdsExhausted);
    }
    return RejectReason::PoolExhausted;
  }

  void cancel_unknown(const EventHeader& h, const OmsUpdate&) noexcept {
    // Never leave an unknown live order at the venue (5.8). We do not have an OMS record,
    // so send a bare cancel keyed by the venue's ids from the message.
    ++stats_.unknown_order_cancels;
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, h.instrument, h.venue);
    m.hdr.recv_ts = now_;
    if (h.type == EventType::OrderAck) {
      const auto& a = msg_cast<OrderAckMsg>(&h);
      m.cl_ord_id = a.cl_ord_id;
      m.venue_order_id = a.venue_order_id;
    } else if (h.type == EventType::Reconcile) {
      const auto& r = msg_cast<ReconcileMsg>(&h);
      m.cl_ord_id = r.cl_ord_id;
      m.venue_order_id = r.venue_order_id;
    }
    FASTMM_LOG_WARN("cancelling unknown live order {}", encode_cl_ord_id(m.cl_ord_id));
    queue_out(m.hdr);
    ++stats_.cancels_sent;
  }

  // The venue keeps refusing to cancel an order the engine thinks is working: ask the connector
  // for its open orders rather than retrying a cancel that cannot succeed.
  void request_reconcile(const OmsUpdate& u) noexcept {
    FASTMM_LOG_WARN("order {} exceeded cancel-reject retries; asking the venue to reconcile",
                    encode_cl_ord_id(u.order.cl_ord_id));
    ControlMsg m{};
    init_header(m, EventType::Control, u.order.instrument, u.order.venue);
    m.command = ControlCommand::Reconcile;
    queue_out(m.hdr);
    ++stats_.reconcile_requests;
  }

  void queue_out(const EventHeader& m) noexcept {
    FASTMM_ASSERT(m.len <= kOutSlotBytes);
    if (out_batch_.full()) flush_out();
    std::byte* slot = out_storage_ + out_batch_.size() * kOutSlotBytes;
    std::memcpy(slot, &m, m.len);
    auto* hdr = reinterpret_cast<EventHeader*>(slot);
    hdr->t0_cycles = event_t0_;
    static_cast<void>(out_batch_.push_back(hdr));
  }

  void flush_out() noexcept {
    if (out_batch_.empty()) return;
    const Cycles t4 = clock_.cycles();
    const std::size_t ok =
        transport_.send(std::span<const EventHeader* const>(out_batch_.data(), out_batch_.size()));
    const Cycles t5 = clock_.cycles();
    // Journaled after the hand-off: the transport accepts a prefix of the batch, and the rest
    // is recorded as dropped (replay refuses the same messages).
    if (journal_.enabled()) {
      for (std::size_t i = 0; i < out_batch_.size(); ++i) {
        if (!journal_.record_outbound(*out_batch_[i], i >= ok)) ++stats_.journal_overflows;
      }
    }
    if (FASTMM_UNLIKELY(ok != out_batch_.size())) {
      stats_.transport_full += out_batch_.size() - ok;
      FASTMM_LOG_ERROR("outbound transport full: {} message(s) dropped; tripping kill switch",
                       out_batch_.size() - ok);
      out_batch_.clear();
      if (!risk_.killed()) {
        risk_.trip();
        on_kill(KillReason::TransportFull);
      }
      return;
    }
    out_batch_.clear();
    if (strategy_t3_.v != 0) {
      latency_.record(LatencyInterval::Serialize,
                      static_cast<std::uint64_t>(clock_.cycles_to_ns(t4 - strategy_t3_)));
    }
    latency_.record(LatencyInterval::Send,
                    static_cast<std::uint64_t>(clock_.cycles_to_ns(t5 - t4)));
    if (event_t0_.v != 0 && !sent_in_event_) {
      sent_in_event_ = true;
      latency_.record(LatencyInterval::TickToTrade,
                      static_cast<std::uint64_t>(clock_.cycles_to_ns(t5 - event_t0_)));
    }
  }

  // ---- clock ------------------------------------------------------------------------------------

  // One clock read per event / fired timer / start / finish; now() returns it until unlatched.
  FASTMM_FORCE_INLINE void latch_clock() noexcept {
    now_ = clock_.now();
    latched_ = true;
  }
  FASTMM_FORCE_INLINE void unlatch_clock() noexcept { latched_ = false; }
  // T0 (network receive) and T1 (decode end) of the inbound message being handled; zero for timers,
  // start and finish. T3 and the tick-to-trade sample belong to that message only.
  FASTMM_FORCE_INLINE void set_event_origin(Cycles t0, Cycles t1) noexcept {
    event_t0_ = t0;
    event_t1_ = t1;
    strategy_t3_ = Cycles{};
    sent_in_event_ = false;
  }
  // Engine API called outside an event, timer, start or finish (tests, tools): take the clock now,
  // so the internal paths can read now_ unconditionally.
  void enter_api() noexcept {
    if (FASTMM_UNLIKELY(!latched_)) now_ = clock_.now();
  }

  // Clocks with a refresh() (TscClock) take recalibrations published by the calibrator thread
  // here, once per step next to the timer poll rather than per event. SimClock has none.
  void refresh_clock() noexcept {
    if constexpr (requires {
                    { clock_.refresh() } -> std::same_as<TscRefresh>;
                  }) {
      const TscRefresh r = clock_.refresh();
      if (FASTMM_LIKELY(r == TscRefresh::None)) return;
      switch (r) {
        case TscRefresh::Reanchored:
        case TscRefresh::Adopted:
          ++stats_.clock_reanchors;
          break;
        case TscRefresh::Stepped:
          ++stats_.clock_steps;
          FASTMM_LOG_WARN("TSC recalibration stepped the engine clock by {} ns (threshold {} ns)",
                          clock_.last_offset_ns(),
                          clock_.step_threshold().ns);
          break;
        case TscRefresh::Rejected:
          FASTMM_LOG_WARN("ignoring a published TSC calibration without a TSC mapping");
          break;
        case TscRefresh::None:
          break;
      }
    }
  }

  // ---- latency ----------------------------------------------------------------------------------

  void record_md_hops(Cycles t2) noexcept {
    if (event_t0_.v == 0) return;
    if (event_t1_.v > event_t0_.v) {
      latency_.record(LatencyInterval::Decode,
                      static_cast<std::uint64_t>(clock_.cycles_to_ns(event_t1_ - event_t0_)));
      latency_.record(LatencyInterval::BookApply,
                      static_cast<std::uint64_t>(clock_.cycles_to_ns(t2 - event_t1_)));
    }
    latency_.record(LatencyInterval::WireToBook,
                    static_cast<std::uint64_t>(clock_.cycles_to_ns(t2 - event_t0_)));
  }
  // Strategy (T2 -> T3) ends at the strategy's first order decision in this event, or at the end
  // of its callback when it made none.
  void record_strategy_hop(Cycles t2) noexcept {
    const Cycles t3 = strategy_t3_.v != 0 ? strategy_t3_ : clock_.cycles();
    latency_.record(LatencyInterval::Strategy,
                    static_cast<std::uint64_t>(clock_.cycles_to_ns(t3 - t2)));
  }
  void publish_latency(Timestamp now) noexcept {
    last_publish_ = now;
    const LatencySnapshot s = latency_.snapshot(now.ns);
    latency_pub_.store(s);
    publish_live(s);
    if (journal_.enabled()) {
      LatencySampleMsg m{};
      init_header(m, EventType::LatencySample);
      m.hdr.flags |= EventHeader::kSynthetic;
      m.hdr.recv_ts = now;
      const auto& st = s.interval[static_cast<std::size_t>(LatencyInterval::TickToTrade)];
      m.interval = static_cast<std::uint8_t>(LatencyInterval::TickToTrade);
      m.count = st.count;
      m.p50_ns = static_cast<std::int64_t>(st.p50);
      m.p90_ns = static_cast<std::int64_t>(st.p90);
      m.p99_ns = static_cast<std::int64_t>(st.p99);
      m.p999_ns = static_cast<std::int64_t>(st.p999);
      m.max_ns = static_cast<std::int64_t>(st.max);
      static_cast<void>(journal_.record(m.hdr));
    }
  }

  void publish_live(const LatencySnapshot& latency) noexcept {
    EngineLiveStats live;
    live.stats = runner_stats();
    live.kills = stats_.kills;
    live.venue_kills = stats_.venue_kills;
    live.kill_flags = risk_.kill_flags();
    live.kill_reason = kill_reason_;
    live.venue_kill_reasons = venue_kill_reasons_;
    live.flatten_state = flatten_state_;
    live.flatten_instruments_left = flatten_state_ == FlattenState::Off ? 0 : flatten_left();
    live.flatten_orders = stats_.flatten_orders;
    presence_.settle(now_);
    const QuotePresenceStats presence = presence_.total();
    live.quoting_elapsed_ns = presence.elapsed_ns;
    live.quoting_two_sided_ns = presence.two_sided_ns;
    live.max_loss_raw = risk_.limits().max_loss.raw;
    live.latency = latency;
    live_pub_.store(live);
  }

  // ---- members ----------------------------------------------------------------------------------

  EngineConfig cfg_;
  const InstrumentTable& instruments_;
  Clock& clock_;
  Transport& transport_;
  Feed& feed_;
  Strategy& strategy_;
  Context ctx_;
  std::unique_ptr<Book[]> books_;
  Oms oms_;
  RiskEngine risk_;
  VenueHealth health_;  // feed lag and ack RTT per venue, and the feed-lag gate
  Duration feed_lag_limit_ = milliseconds(cfg_.risk.max_feed_lag_ms);  // [risk] max_feed_lag_ms
  QuoteManager quotes_;
  QuotePresence presence_;
  TimerWheel<> timers_;
  PositionTracker positions_;
  LatencyTracker latency_;
  JournalWriter journal_;
  RecordWriter records_;
  Xoshiro256ss rng_;
  SpinPolicy spin_;
  RejectLogLimiter reject_log_;
  EngineStats stats_{};
  Seqlocked<LatencySnapshot> latency_pub_;
  Seqlocked<EngineLiveStats> live_pub_;
  Timestamp last_publish_{};
  Timestamp now_{};  // valid while latched_

  alignas(kCacheLine) std::byte out_storage_[kOutBatch * kOutSlotBytes] = {};
  StaticVector<const EventHeader*, kOutBatch> out_batch_;

  Cycles event_t0_{};
  Cycles event_t1_{};
  Cycles strategy_t3_{};
  bool sent_in_event_ = false;
  bool fx_on_;  // cfg_.fx converts ([accounting])
  bool latched_ = false;
  bool in_engine_ = false;  // inside step(), drain(), start() or finish()
  bool quoting_enabled_;
  bool reconciling_ = false;
  // The reconciliation in progress was preceded by a complete execution replay, so what it reports
  // is the venue's own and nothing in it has to be estimated (ReconcileMsg::kExecutionsExact).
  bool reconcile_exact_ = false;
  bool params_stale_;                  // max_param_age passed, or no ParamUpdate yet
  bool param_deadline_armed_ = false;  // param_deadline_ applies (max_param_age set, fresh)
  Timestamp param_deadline_{};
  TimerId param_timer_{};    // the engine's one-shot max_param_age timer
  TimerId ack_timer_{};      // the engine's repeating ack_timeout sweep
  TimerId flatten_timer_{};  // ... and its flatten sweep while one runs
  FlattenState flatten_state_ = FlattenState::Off;
  InstrumentId flatten_scope_{};  // the flatten's instrument; invalid: every instrument
  std::int64_t flatten_bps_ = 0;
  Timestamp flatten_deadline_ = Timestamp::max();
  // Instruments and venues an operator pulled on their own (ControlCommand::PullQuotes with a
  // scope, a flatten); they stay pulled until a matching resume.
  std::array<bool, kMaxInstruments> inst_pulled_{};
  // The flatten slice each instrument has in flight (invalid: none).
  std::array<ClientOrderId, kMaxInstruments> flatten_order_{};
  std::uint32_t venue_pulled_ = 0;
  KillReason kill_reason_ = KillReason::None;
  std::array<KillReason, kKillVenueSlots> venue_kill_reasons_{};
  bool started_ = false;
  bool finished_ = false;
  std::atomic<bool> stop_{false};
  // Funding payments booked, by venue id and instrument (on_funding), and the counters. Payments
  // come a few a day per instrument; the window reaches back further than any replay of the venue's
  // history.
  static constexpr std::size_t kFundingWindow = 4096;
  struct FundingState {
    RecentMap<std::uint64_t, std::uint8_t, kFundingWindow> seen;
    FundingStats stats;
  };
  std::unique_ptr<FundingState> funding_ = std::make_unique<FundingState>();
  // Venues whose feed shows our orders (bit v), and our quantity there; null when there are none.
  std::uint32_t own_venues_ = 0;
  std::unique_ptr<OwnQuantity> own_;
  QueueTracker queue_{cfg_.queue_conservatism_bps};
};

}  // namespace fastmm
