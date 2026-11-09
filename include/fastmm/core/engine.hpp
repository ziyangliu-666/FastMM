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
// Balances: BalanceMsg events (a venue's report of one asset) feed a BalanceBook, which the engine
// moves with its own orders and fills until the venue's next report (core/balance_book.hpp). Until
// the first report nothing of it runs; after it, every order event updates the order's hold and
// [risk] check_balance refuses an order the estimate does not cover (BalanceShort).
//
// Perpetuals: PerpState events (a venue's mark, index and funding of one derivative) feed a
// PerpBook (core/perp_book.hpp) the strategy reads through ctx.mark, ctx.index and ctx.funding.
// With [accounting] mark = "venue" a position with a fresh venue mark is valued at it instead of
// the book's mid, for the unrealized PnL, max_loss and the exposure limits.
//
// Account pools (core/account_pool.hpp, EngineConfig::pools): a venue's instruments, books and
// positions are the primary's; a new order on one of them goes to the account the request names
// (NewOrderRequest::account) or to the member whose balance covers it and whose order-count window
// has the most room left, and keeps that member in Order::venue, so its cancels, replaces and
// events carry the member's id. Balances are per member (ctx.balance(member, asset)); a member
// whose order link is not live, or whose kill switch is engaged, is routed around. With
// [venues.<primary>.pool] placement = "home" each instrument has a home account instead
// (core/pool_homes.hpp): an order that adds to the position goes to the home, one that reduces it
// to the accounts holding it, the pieces away from home first. With sweep = true the engine moves
// the pieces home by internal transfer and sends the pool's spot sells to the home
// (core/transfer_lane.hpp).
//
// Parameters (ADR-0013): a ParamUpdate event assigns new parameter values to the strategy
// (apply_param_update), then on_params runs. With EngineConfig::max_param_age, quoting is disabled
// before the first ParamUpdate and whenever none was applied for that long; the engine checks the
// deadline before each event and timer and with a one-shot timer of its own, all on the journaled
// engine clock.
#include "fastmm/core/account_pool.hpp"
#include "fastmm/core/balance_book.hpp"
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/recent_map.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/engine_config.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/fill_cluster.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/market_view.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/order_budget.hpp"
#include "fastmm/core/own_quantity.hpp"
#include "fastmm/core/perp_book.hpp"
#include "fastmm/core/pool_homes.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/queue_tracker.hpp"
#include "fastmm/core/quote_diag.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/quote_presence.hpp"
#include "fastmm/core/record_stream.hpp"
#include "fastmm/core/reject_counters.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/spsc_ring.hpp"
#include "fastmm/core/strategy_context.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/timer_wheel.hpp"
#include "fastmm/core/transfer.hpp"
#include "fastmm/core/transfer_lane.hpp"
#include "fastmm/core/transport.hpp"
#include "fastmm/core/venue_health.hpp"
#include "fastmm/strategies/hooks.hpp"
#include "fastmm/strategies/param_overrides.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm {

static_assert(PoolPlan::kVenues >= kMaxVenues, "a pool plan covers every venue id a session has");

struct EngineStats {
  std::uint64_t events = 0;
  std::uint64_t book_updates = 0;
  std::uint64_t trades = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;
  std::uint64_t fills = 0;
  std::uint64_t risk_rejects = 0;
  std::uint64_t venue_rejects = 0;                // OrderReject messages that changed an order
  std::uint64_t risk_reject_notices_dropped = 0;  // on_risk_reject calls lost to a full queue
  std::uint64_t journal_overflows = 0;
  std::uint64_t records_written = 0;   // records handed to the store ring (core/record_stream.hpp)
  std::uint64_t records_dropped = 0;   // ... and dropped because it was full
  std::uint64_t markouts_dropped = 0;  // [engine] markout_ms: fills a full horizon queue skipped
  std::uint64_t transport_full = 0;
  std::uint64_t timers_fired = 0;
  std::uint64_t batch_ends = 0;  // on_batch_end calls (ctx.request_batch_end)
  // Checkpoints written to the journal, those skipped because the journal ring lacked the room,
  // and the bytes of the last one written.
  std::uint64_t checkpoints = 0;
  std::uint64_t checkpoints_skipped = 0;
  std::uint64_t checkpoint_bytes = 0;
  std::uint64_t crossed_pulls = 0;
  std::uint64_t unknown_order_cancels = 0;
  std::uint64_t unknown_orders_kept = 0;  // not FastMM's, left alone ([engine] cancel_unknown)
  std::uint64_t kills = 0;                // global kill switch trips
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
  std::uint64_t ack_timeouts = 0;          // PendingNew orders force-cancelled by the ack sweep
  std::uint64_t flattens = 0;              // operator flattens started (ControlCommand::Flatten)
  std::uint64_t reduce_only_switches = 0;  // ControlCommand::ReduceOnly applied
  std::uint64_t home_changes = 0;          // pool homes changed (ControlCommand::PoolHome)
  std::uint64_t home_unavailable = 0;      // orders refused HomeUnavailable
  std::uint64_t home_spills = 0;           // adding orders spill = "busy" sent off their home
  std::uint64_t priced_out = 0;            // orders and moves refused PricedOut ([capacity])
  std::uint64_t treasury_records = 0;  // pool treasury transfer steps (ControlCommand::Transfer)
  std::uint64_t flatten_orders = 0;    // reduce-only orders a flatten sent
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
  static constexpr std::uint8_t kStateTimer = 4;
  // Not a timer: the on_batch_end call a strategy asked for (ctx.request_batch_end), journaled as a
  // TimerMsg so a replay calls it after the same event.
  static constexpr std::uint8_t kBatchEndTimer = 5;
  // The journal's checkpoint ([engine] checkpoint_s).
  static constexpr std::uint8_t kCheckpointTimer = 6;
  // The pools' sweep (core/transfer_lane.hpp), every kSweepPeriod.
  static constexpr std::uint8_t kSweepTimer = 7;
  static constexpr Duration kSweepPeriod = milliseconds(250);

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
        own_books_(new Book[std::max<std::size_t>(instruments.size(), 1)]),
        books_(own_books_.get()),
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
    // Quotes are replaced in place on the instruments whose venue supports it; [engine]
    // supports_replace = false turns replace off everywhere. replace_all_venues (a replay of a
    // journal recorded before this was per venue) turns it off everywhere if one venue lacks it.
    QuoteParams qp = cfg.quotes;
    for (const Instrument& inst : instruments_) {
      const bool venue_replace = transport_.supports_replace(inst.venue);
      quotes_.set_replace(inst.id, venue_replace);
      if (!venue_replace && qp.replace_all_venues) qp.supports_replace = false;
    }
    quotes_.set_params(qp);
    cluster_.configure(cfg_.fill_cluster);
    if (records_.enabled()) {
      for (const std::int64_t ms : cfg.markout_ms) {
        if (ms <= 0 || markouts_.size() == kMaxMarkoutHorizons) continue;
        MarkoutQueue& q = markouts_.emplace_back();
        q.horizon_ms = static_cast<std::int32_t>(std::min<std::int64_t>(ms, 86'400'000));
        q.ring.resize(kMarkoutQueue);
      }
    }
    positions_.set_accounting(cfg.fx);
    risk_.set_fx(cfg.fx);
    // Our quantity in the feed is followed only for venues whose feed shows our orders (a pool
    // member's orders show in its primary's feed).
    for (const Instrument& inst : instruments_) {
      if (!transport_own_in_feed(inst.venue)) continue;
      for (const VenueId v : cfg_.pools.members(inst.venue)) own_venues_ |= venue_mask(v);
    }
    if (own_venues_ != 0) {
      own_ = std::make_unique<OwnQuantity>();
      own_shown_ = std::make_unique<OwnShown[]>(instruments_.size());
    }
    risk_.set_underlying(cfg.underlying);
    balances_->build(instruments_, cfg.fees, cfg.balance, &cfg_.pools);
    perp_.configure(cfg.perp);
    // Venues whose positions wait for their first reconciliation: only those an instrument trades,
    // and the members of their pools.
    for (const Instrument& inst : instruments_) {
      for (const VenueId v : cfg_.pools.members(inst.venue)) {
        if (v.value < 32U && ((cfg.await_reconcile >> v.value) & 1U) != 0)
          awaiting_ |= venue_mask(v);
      }
    }
    ro_session_ = cfg.reduce_only;
    ro_venues_ = cfg.reduce_only_venues;
    refresh_reduce_only();
    if (pools_on_) held_ = std::make_unique<HeldTable>();
    if (cfg.capacity.on()) {
      CapacityConfig cc = cfg.capacity;
      double min_n = 0.0;
      for (const Instrument& inst : instruments_) {
        const double m = inst.min_notional.to_double();
        if (m > 0.0 && (min_n == 0.0 || m < min_n)) min_n = m;
      }
      cc.min_notional = min_n;
      if (cfg.risk.max_order_notional.is_positive())
        cc.max_notional = cfg.risk.max_order_notional.to_double();
      capacity_ = std::make_unique<CapacityPrice>();
      capacity_->configure(cc);
      capacity_on_ = true;
    }
    for (std::atomic<std::uint8_t>& h : home_pub_)
      h.store(VenueId{}.value, std::memory_order_relaxed);
    if (homes_on_) {
      for (std::size_t v = 0; v < kMaxVenues; ++v) {
        const VenueId primary{static_cast<std::uint8_t>(v)};
        const LaneConfig& lc = cfg_.homes.lane_of(primary);
        if (!lc.on() || !cfg_.homes.of(primary).configured || !cfg_.pools.pooled(primary)) continue;
        lanes_[v] = std::make_unique<TransferLane>(primary, lc);
        lanes_on_ = true;
      }
      if (lanes_on_) {
        sweep_cells_ = std::make_unique<SweepCells>();
        lane_out_ = std::make_unique<SpscRing<LaneIntent, kLaneRing>>();
      }
    }
  }
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // ---- lifecycle ----------------------------------------------------------------------------

  // Touch every pool/page and exercise the dispatch path once so the first real event does
  // not pay for page faults or cold i-cache.
  void warm_up() noexcept {
    oms_.warm_up();
    timers_.warm_up();
    if (own_ != nullptr) own_->warm_up();
    Logger::instance().attach_current_thread();
    alignas(64) std::byte buf[BookDeltaMsg::size_for(8, 8)];
    for (const Instrument& inst : instruments_) {
      if (own_books_ == nullptr) break;  // the books are the market's (follow_market)
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
  std::size_t step() noexcept { return step_from(nullptr); }

  // step() with `event` first: a message the caller holds, read where it lies instead of from the
  // feed (a multiverse's runs share their market data, backtest/multiverse.hpp).
  std::size_t step_with(const EventHeader& event) noexcept { return step_from(&event); }

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
    batch_end();
    in_engine_ = false;
    return n;
  }

  void run() {
    pin_to_cpu(cfg_.cpu);
    set_thread_name("fm-engine");
    warm_up();
    start();
    while (!stop_.load(std::memory_order_relaxed)) {
      beat();
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
      beat();
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
    catch_up();
    show_own_all();
    latch_clock();
    set_event_origin(Cycles{}, Cycles{});
    // The rate limiter refills from the start time, not from construction (replay constructs the
    // engine at a different time).
    risk_.bucket().rebase(now_);
    if (journal_.enabled() && !journal_.record_clock(EngineTimeMsg::Kind::Start, now_))
      ++stats_.journal_overflows;
    if (homes_on_) apply_pins();
    publish_pool();
    FASTMM_LOG_INFO(
        "strategy {} hooks: {}", strategy_name(), implemented_hooks<Strategy, Context, Book>());
    if (cfg_.ack_timeout.ns > 0) ack_timer_ = timers_.add(now_, cfg_.ack_timeout, /*repeat=*/true);
    if (lanes_on_) {
      lane_start_ = now_;
      sweep_timer_ = timers_.add(now_, kSweepPeriod, /*repeat=*/true);
      std::uint32_t live = 0;
      for (std::size_t v = 0; v < kMaxVenues; ++v) {
        if (lanes_[v] != nullptr && !lanes_[v]->config().dry_run) live |= 1U << v;
      }
      set_lanes_live(live);
    }
    const bool quoting_before = quoting_enabled();
    if constexpr (has_hook(Hook::Start)) strategy_.on_start(ctx_);
    after_hooks(quoting_before);
    if constexpr (KeepsState<Strategy>) {
      restore_state();
      if (!cfg_.state_file.empty() && cfg_.state_interval.ns > 0)
        state_timer_ = timers_.add(now_, cfg_.state_interval, /*repeat=*/true);
      // What the session starts from, for a replay or a backtest over the journal.
      if (journal_.enabled() && !cfg_.initial_state.empty()) {
        journal_state(StrategyStateMsg::Kind::Restored,
                      cfg_.initial_state,
                      state_restored_ ? StrategyStateMsg::kAccepted : 0,
                      false);
      }
    }
    if (journal_.enabled()) {
      // A checkpoint's buffers, made once so that writing one (the timer below, a take-over)
      // does not allocate on the engine thread.
      position_buf_ = std::make_unique<StatePosition[]>(instruments_.size() + 1);
      order_buf_ = std::make_unique<StateOrder[]>(kMaxOpenOrders);
      if (cfg_.checkpoint_interval.ns > 0)
        checkpoint_timer_ = timers_.add(now_, cfg_.checkpoint_interval, /*repeat=*/true);
    }
    flush_out();
    unlatch_clock();
    hide_own_all();
    update_interest();
    in_engine_ = false;
  }
  void finish() noexcept {
    if (!started_ || finished_) return;
    finished_ = true;
    in_engine_ = true;
    catch_up();
    show_own_all();
    latch_clock();
    set_event_origin(Cycles{}, Cycles{});
    if (journal_.enabled() && !journal_.record_clock(EngineTimeMsg::Kind::Finish, now_))
      ++stats_.journal_overflows;
    if constexpr (has_hook(Hook::Stop)) strategy_.on_stop(ctx_);
    flush_out();
    if constexpr (KeepsState<Strategy>) {
      if (!cfg_.state_file.empty()) capture_state();
    }
    // The last word on every position, so a store holds the state the session ended in.
    if (records_.enabled()) {
      flush_reject_folds();
      for (const Instrument& inst : instruments_) {
        const Position& p = positions_.get(inst.id);
        if (p.fills != 0 || !p.qty.is_zero()) emit_position(inst.id);
      }
    }
    publish_latency(now_);
    unlatch_clock();
    hide_own_all();
    in_engine_ = false;
  }
  // The engine thread is the only writer: a plain increment, no read-modify-write. With the kill
  // switch engaged the turn also publishes how many orders are still open (killed_open_orders).
  FASTMM_FORCE_INLINE void beat() noexcept {
    beat_.store(beat_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (FASTMM_UNLIKELY(risk_.killed()))
      killed_open_.store(oms_.open_count(), std::memory_order_relaxed);
  }
  void stop() noexcept {
    stop_.store(true, std::memory_order_release);
    if constexpr (kFeedWaits) feed_.notify();
  }
  // ctx.request_batch_end(): on_batch_end runs once the events waiting now are handled. Orders it
  // sends carry the T0 of the event that asked last.
  void request_batch_end() noexcept {
    if constexpr (has_hook(Hook::BatchEnd)) {
      batch_end_wanted_ = true;
      batch_t0_ = event_t0_;
      batch_t1_ = event_t1_;
    }
  }
  // ctx.request_checkpoint(): a checkpoint once the events waiting now are handled, journaled
  // like the checkpoint timer so a replay writes it after the same event.
  void request_checkpoint() noexcept {
    if constexpr (!kFeedReplaysTimers) checkpoint_wanted_ = journal_.enabled();
  }
  [[nodiscard]] static constexpr bool keeps_state() noexcept { return KeepsState<Strategy>; }
  [[nodiscard]] std::uint32_t strategy_state_version() noexcept {
    return state_version_of(strategy_);
  }
  [[nodiscard]] bool stopped() const noexcept { return stop_.load(std::memory_order_acquire); }

  // ---- strategy-facing API ------------------------------------------------------------------

  // The time of the event, timer, start or finish being processed; outside those (direct calls
  // from tests or tools) the clock itself.
  [[nodiscard]] FASTMM_FORCE_INLINE Timestamp now() const noexcept {
    return FASTMM_LIKELY(latched_) ? now_ : clock_.now();
  }
  // The book of an instrument of the table; an empty one for any other id.
  [[nodiscard]] const Book& book(InstrumentId id) const noexcept {
    static const Book kNone{};
    return FASTMM_LIKELY(id.value < instruments_.size()) ? books_[id.value] : kNone;
  }
  // The engine follows `market`, whose books it reads and which keeps what every message changes
  // besides (market_view.hpp): the engine is handed only the messages md_interest() asks for,
  // and catches up with the others before it takes anything. Before start().
  void follow_market(const MarketView& market) noexcept {
    own_books_.reset();
    books_ = market.books.get();
    market_ = &market;
    market_seen_ = market.counts;
  }
  // What this engine needs of the market data to come; current after every step.
  [[nodiscard]] const MdInterest& md_interest() const noexcept { return interest_; }

  // ---- what the strategy watches (StrategyContext::watch, quiet_book) ---------------------------
  void watch(InstrumentId id, bool on) noexcept {
    if (id.value >= kMaxInstruments || watch_[id.value].on == on) return;
    watch_[id.value].on = on;
    interest_changed(id);
  }
  void quiet_book(InstrumentId id, Price lo, Price hi) noexcept {
    if (id.value >= kMaxInstruments) return;
    Watch& w = watch_[id.value];
    if (w.quiet && w.lo == lo && w.hi == hi) return;
    w.quiet = true;
    w.lo = lo;
    w.hi = hi;
    interest_changed(id);
  }
  void quiet_book(InstrumentId id) noexcept {
    if (id.value >= kMaxInstruments || !watch_[id.value].quiet) return;
    watch_[id.value].quiet = false;
    interest_changed(id);
  }
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
  // EngineConfig::venue_names[v], or "venue <id>" when it names none.
  [[nodiscard]] std::string_view venue_name(VenueId v) const noexcept {
    static constexpr std::string_view kById[] = {
        "venue 0", "venue 1", "venue 2", "venue 3", "venue 4", "venue 5", "venue 6", "venue 7"};
    static_assert(std::size(kById) == kMaxVenues);
    if (v.value < cfg_.venue_names.size() && !cfg_.venue_names[v.value].empty())
      return cfg_.venue_names[v.value];
    return v.value < kMaxVenues ? kById[v.value] : std::string_view("venue ?");
  }
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
  [[nodiscard]] EngineLiveStats live_stats() const noexcept { return live_pub_->load(); }
  // Turns of run() or run_inline() so far, 0 before the first (after on_start). Any thread reads it
  // to tell an engine that is still running from one stuck inside a step (fastmm-live's [engine]
  // stall_ms): an idle engine turns at least every kMaxIdleWait.
  [[nodiscard]] std::uint64_t heartbeat() const noexcept {
    return beat_.load(std::memory_order_relaxed);
  }
  // The orders not yet over (cancelled, filled, refused) as of the engine's last turn with the
  // global kill switch engaged; kUnknownOpenOrders before such a turn. A shutdown waits for 0:
  // every cancel the kill queued has been confirmed, and every order still in flight when it
  // tripped has been acknowledged and cancelled. Any thread.
  [[nodiscard]] std::uint32_t killed_open_orders() const noexcept {
    return killed_open_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] bool quoting_enabled() const noexcept {
    return quoting_enabled_ && !reconciling() && !params_stale_ && !risk_.killed();
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
  // Reduce-only mode holds the instrument (ControlCommand::ReduceOnly, [risk] reduce_only): an
  // order or replace on it that would leave its position no nearer zero is refused.
  [[nodiscard]] bool reduce_only(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments && reduce_only_[id.value];
  }
  // The scopes it was turned on for: the whole session, the venues (bit per venue id), and how
  // many instruments it holds through any of them.
  [[nodiscard]] bool reduce_only_session() const noexcept { return ro_session_; }
  [[nodiscard]] std::uint32_t reduce_only_venues() const noexcept { return ro_venues_; }
  [[nodiscard]] std::uint32_t reduce_only_count() const noexcept { return ro_count_; }
  // A venue's orders and position are being reconciled, or (EngineConfig::await_reconcile) have
  // not been since the session started: the positions may still be missing fills.
  [[nodiscard]] bool reconciling() const noexcept { return (reconciling_ | awaiting_) != 0; }
  // Venues whose first reconciliation has not ended (EngineConfig::await_reconcile).
  [[nodiscard]] std::uint32_t awaiting_reconcile() const noexcept { return awaiting_; }
  // max_param_age is set and no ParamUpdate was applied within it (or none yet).
  [[nodiscard]] bool params_stale() const noexcept { return params_stale_; }
  // The first reason the global kill switch was set for (None while it is not set).
  [[nodiscard]] KillReason kill_reason() const noexcept { return kill_reason_; }
  // Engine time the global kill switch was set (0 while it is not set).
  [[nodiscard]] Timestamp kill_ts() const noexcept { return kill_ts_; }
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
    if (risk_.underlying_on()) {
      buy.underlying = underlying_inputs(id, Side::Buy);
      sell.underlying = underlying_inputs(id, Side::Sell);
    }
    RiskHeadroom h = risk_.headroom(instruments_.get(id), buy, sell, net_pnl());
    if (balances_live_ && book(id).is_valid()) {
      const Price mid = book(id).mid();
      h.balance_buy_qty = balance_room(id, Side::Buy, mid);
      h.balance_sell_qty = balance_room(id, Side::Sell, mid);
    }
    return h;
  }
  [[nodiscard]] VenueHealthView venue_health(VenueId v) const noexcept {
    return health_.view(v, now());
  }
  [[nodiscard]] FillClusterView fill_cluster() const noexcept { return cluster_.view(); }
  // ctx.order_budget: the [risk] bucket's whole tokens, and the venue's windows where the transport
  // carries them (LiveTransport: the connector's publication; a backtest has none). The windows
  // count the new orders and replaces sent to the account that the publication had not seen yet
  // (OrderBudget::orders_taken): the connector publishes after the venue answers, so without them
  // every order of a burst would see the room the first one saw.
  // A window whose end (RateWindow::ends_ns) has passed since the publication is read as the
  // connector's limiter rolls it, empty, and a pause past its end as over; OrderBudget::wait_ns
  // is the time until the account admits an order again.
  [[nodiscard]] OrderBudget order_budget(VenueId v) const noexcept {
    OrderBudget b;
    const std::int64_t t = now().ns;
    if constexpr (requires(const Transport& tr, OrderBudget& o) {
                    { tr.venue_budget(v, o) } -> std::same_as<bool>;
                  }) {
      if (transport_.venue_budget(v, b)) {
        for (RateWindow* w : {&b.orders_10s, &b.orders_1m, &b.orders_1d}) {
          const std::int64_t len = w->window_ms * 1'000'000;
          if (w->ends_ns == 0 || t < w->ends_ns || len <= 0) continue;
          w->used = 0;
          w->ends_ns += (((t - w->ends_ns) / len) + 1) * len;
        }
        if (b.venue_paused && b.paused_until_ns != 0 && t >= b.paused_until_ns)
          b.venue_paused = false;
        if (v.value < kMaxVenues && sent_orders_[v.value] > b.orders_taken) {
          const auto unseen = static_cast<std::int64_t>(sent_orders_[v.value] - b.orders_taken);
          for (RateWindow* w : {&b.orders_10s, &b.orders_1m, &b.orders_1d}) {
            if (w->known()) w->used += unseen;
          }
        }
      }
    }
    b.local_tokens = risk_.bucket().available(Timestamp{t});
    b.local_wait_ns = risk_.bucket().wait_ns(Timestamp{t});
    b.wait_ns = b.wait_from(t);
    return b;
  }

  // ---- strategy state ([strategy] state_file) ------------------------------------------------

  // The state captured since the last call (state_interval, finish), for the caller to write to
  // EngineConfig::state_file. Any thread: the engine thread fills the buffer under the same lock,
  // once per interval, so neither side waits on the other in practice.
  bool take_strategy_state(std::string& out) {
    if constexpr (!KeepsState<Strategy>) {
      return false;
    } else {
      const std::lock_guard<std::mutex> lock(state_mu_);
      if (!state_dirty_) return false;
      out.swap(state_pending_);
      state_dirty_ = false;
      return true;
    }
  }
  // Bytes for restore() when the next ControlCommand::TakeOver arrives (a warm standby reads the
  // state file only once the session before it has written it last). Any thread, before the
  // message is pushed.
  void stage_strategy_state(std::string bytes) {
    if constexpr (KeepsState<Strategy>) {
      const std::lock_guard<std::mutex> lock(state_mu_);
      staged_state_ = std::move(bytes);
    }
  }
  [[nodiscard]] std::uint64_t state_captures() const noexcept { return state_captures_; }
  [[nodiscard]] bool state_restored() const noexcept { return state_restored_; }

  // ---- balances (core/balance_book.hpp) -------------------------------------------------------

  // One asset of a venue: the venue's last report moved by the engine's own orders and fills since.
  [[nodiscard]] Balance balance(VenueId v, std::string_view asset) const noexcept {
    return balances_->balance(v, asset);
  }
  [[nodiscard]] Margin margin(VenueId v) const noexcept { return balances_->margin(v); }
  // Largest quantity of `id` on `side` at `px` the balance covers; Qty::max() while the venue has
  // not reported the balance the side draws on. With a pool, the most any usable account of it
  // covers: what one order can be, since an order goes to one account.
  [[nodiscard]] Qty balance_room(InstrumentId id, Side side, Price px) const noexcept {
    if (!balances_live_ || !instruments_.contains(id)) return Qty::max();
    const Instrument& inst = instruments_.get(id);
    if (FASTMM_LIKELY(!pools_on_) || !cfg_.pools.pooled(inst.venue))
      return balances_->room(id, inst, side, px);
    // With placement = "home": what the one account the next order on the side goes to holds,
    // a piece's part at most (a piece's order never takes it through zero).
    bool homed = homes_on_ && cfg_.homes.of(inst.venue).homes();
    VenueId target{};
    Qty cap = Qty::max();
    if (FASTMM_UNLIKELY(lanes_on_) && side == Side::Sell && sweeping(inst)) {
      homed = true;  // a sweeping pool sells from the home (route)
      target = home_[id.value];
    } else if (homed) {
      target = reduce_route(inst, side, px, Qty{}, /*sizing=*/true);
      if (target.valid() && target != home_[id.value]) {
        const Qty held = holding(id, target);
        cap = Qty::from_raw(side == Side::Sell ? held.raw : -held.raw);
      } else if (!target.valid() && spills_busy(inst) && side == Side::Buy &&
                 home_[id.value].valid()) {
        homed = false;  // spill = "busy": the home or any account that covers it
      } else if (!target.valid()) {
        target = add_target(inst);
        if (!target.valid()) return Qty{};
      }
    }
    // The largest order one account can hold is its room plus what its open orders on the side
    // already hold (sent or resting: both are in its balance's holds and in open_qty); returned
    // net of the pool's open quantity, so room + open_qty (fit_to_balance) is that order.
    // Counting another account's orders on top would size a quote no account covers: it would be
    // cancelled on the ack and placed again, for ever.
    Qty open_on[PoolPlan::kVenues] = {};
    oms_.for_each_open_order(id, [&](Handle<Order>, const Order& o) {
      if (o.side == side && o.venue.value < PoolPlan::kVenues)
        open_on[o.venue.value] += o.leaves_qty();
    });
    Qty most{};
    for (const VenueId v : cfg_.pools.members(inst.venue)) {
      if (!account_usable(v) || (homed && v != target)) continue;
      const Qty room = balances_->room(id, inst, side, px, v);
      if (room == Qty::max() && cap == Qty::max()) return room;
      Qty holds = room == Qty::max() ? cap : room + open_on[v.value];
      if (holds > cap) holds = cap;
      if (holds > most) most = holds;
    }
    const Qty open = oms_.open_qty(id, side);
    return most > open ? most - open : Qty{};
  }
  // ... on one account of the instrument's pool (the instrument's own venue when `account` is
  // not one of them).
  [[nodiscard]] Qty balance_room(InstrumentId id,
                                 Side side,
                                 Price px,
                                 VenueId account) const noexcept {
    if (!balances_live_ || !instruments_.contains(id)) return Qty::max();
    return balances_->room(id, instruments_.get(id), side, px, account);
  }
  [[nodiscard]] const BalanceBook& balances() const noexcept { return *balances_; }
  // A venue has reported balances: the estimate and the check run from here on.
  [[nodiscard]] bool balances_live() const noexcept { return balances_live_; }

  // ---- account pools (core/account_pool.hpp) ---------------------------------------------------

  // The accounts that take orders for `primary`'s instruments, `primary` first; just `primary`
  // where it has no pool.
  [[nodiscard]] PoolMembers pool(VenueId primary) const noexcept {
    return cfg_.pools.members(primary);
  }
  // The venue whose instruments `v` takes orders for: its primary, or `v` itself.
  [[nodiscard]] VenueId pool_primary(VenueId v) const noexcept { return cfg_.pools.primary(v); }
  // The pool members whose order link is live and whose kill switch is not engaged, as the
  // automatic routing sees them now.
  [[nodiscard]] bool account_usable(VenueId v) const noexcept {
    return (order_link_down_ & venue_mask(v)) == 0 && !risk_.venue_killed(v);
  }

  // ---- homes (core/pool_homes.hpp) -------------------------------------------------------------

  // The instrument's home account; invalid without a [venues.<primary>.pool] table or before it
  // has one.
  [[nodiscard]] VenueId home(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments ? home_[id.value] : VenueId{};
  }
  // The pool's placement ([venues.<primary>.pool] placement; "balance" without the table).
  [[nodiscard]] Placement placement(VenueId primary) const noexcept {
    return homes_on_ ? cfg_.homes.of(cfg_.pools.primary(primary)).placement : Placement::Balance;
  }
  // What `account` holds of the instrument: a spot instrument's base balance there (the estimate:
  // the last report moved by the engine's own orders and fills since), else the account's position
  // from its own fills and position reports. Without a pool, the position on the instrument's
  // venue; zero on an account outside its pool.
  [[nodiscard]] Qty holding(InstrumentId id, VenueId account) const noexcept {
    if (!instruments_.contains(id)) return Qty{};
    const Instrument& inst = instruments_.get(id);
    if (!cfg_.pools.admits(inst.venue, account)) return Qty{};
    if (!inst.is_derivative() && balances_live_) {
      const std::int64_t total = balances_->base_total(id, account);
      if (total != BalanceBook::kUnreported) return Qty::from_raw(total);
    }
    if (held_ == nullptr || !cfg_.pools.pooled(inst.venue))
      return account == inst.venue ? positions_.get(id).qty : Qty{};
    return (*held_)[id.value][pool_index(inst.venue, account)];
  }
  // Accounts other than the home that hold a tradable amount of the instrument (at least the lot,
  // min_qty and, at the mid, min_notional): the pieces its reducing orders work off first.
  [[nodiscard]] std::uint32_t pieces(InstrumentId id) const noexcept {
    if (!homes_on_ || !instruments_.contains(id)) return 0;
    const Instrument& inst = instruments_.get(id);
    if (!cfg_.pools.pooled(inst.venue)) return 0;
    const Price mid = books_[id.value].mid();
    std::uint32_t n = 0;
    for (const VenueId v : cfg_.pools.members(inst.venue)) {
      if (v != home_[id.value] && tradable(inst, holding(id, v), mid)) ++n;
    }
    return n;
  }
  // ctx.set_home: the instrument's home becomes `account` (a ControlCommand::PoolHome of the
  // strategy, journaled). With placement = "home" the adding orders working on other accounts are
  // cancelled. False when the instrument's venue keeps no homes or `account` is not of its pool.
  bool set_home(InstrumentId id, VenueId account) noexcept {
    enter_api();
    const bool ok = apply_home(id, account, HomeSource::Strategy);
    flush_out();
    return ok;
  }
  // The planner's view of `primary`'s pool (core/pool_homes.hpp PoolLoad). Any thread: it reads
  // only what the engine thread publishes for it.
  void pool_load(VenueId primary, PoolLoad& out) const noexcept {
    out.count = 0;
    out.members = cfg_.pools.members(primary);
    const std::uint32_t unusable = unusable_pub_.load(std::memory_order_relaxed);
    for (std::size_t k = 0; k < out.members.size(); ++k) {
      const VenueId v = out.members[k];
      out.limit_10s[k] =
          v.value < kMaxVenues ? limit_pub_[v.value].load(std::memory_order_relaxed) : 0;
      out.usable[k] = (unusable & venue_mask(v)) == 0;
    }
    for (const Instrument& inst : instruments_) {
      if (inst.venue != primary || inst.id.value >= kMaxInstruments) continue;
      const std::size_t j = out.count++;
      out.instrument[j] = inst.id;
      out.orders[j] = inst_orders_[inst.id.value].load(std::memory_order_relaxed);
      out.home[j] = VenueId{home_pub_[inst.id.value].load(std::memory_order_relaxed)};
      out.pinned[j] = pinned_pub_[inst.id.value].load(std::memory_order_relaxed);
    }
  }

  // ctx.set_funding_need: the quote (in the instrument's quote or settlement asset) its bids want
  // resting. A pool treasury with targets = "demand" funds each account for the needs of the
  // instruments whose home it is (pool_funding). Zero or less: none.
  void set_funding_need(InstrumentId id, Notional quote) noexcept {
    if (!instruments_.contains(id)) return;
    need_pub_[id.value].store(std::max<std::int64_t>(quote.raw, 0), std::memory_order_relaxed);
  }
  [[nodiscard]] Notional funding_need(InstrumentId id) const noexcept {
    if (!instruments_.contains(id)) return {};
    return Notional::from_raw(need_pub_[id.value].load(std::memory_order_relaxed));
  }
  // ---- transfer lanes (core/transfer_lane.hpp) --------------------------------------------------

  // The pool sweeps now: its free base on other accounts goes to the homes, and its reducing spot
  // orders go to the home. False without a lane, in a dry run, while the breaker is open or once
  // the day's budget is used. Any thread.
  [[nodiscard]] bool sweeping(VenueId primary) const noexcept {
    const VenueId p = cfg_.pools.primary(primary);
    return p.value < 32 && (lanes_live_pub_.load(std::memory_order_relaxed) & (1U << p.value)) != 0;
  }
  // The pool's lane; null without one. Engine thread.
  [[nodiscard]] const TransferLane* lane(VenueId primary) const noexcept {
    const VenueId p = cfg_.pools.primary(primary);
    return p.value < kMaxVenues ? lanes_[p.value].get() : nullptr;
  }
  // The transfer worker's next message from the engine (its one consumer thread).
  bool pop_transfer_intent(LaneIntent& out) noexcept {
    return lane_out_ != nullptr && lane_out_->try_pop(out);
  }
  [[nodiscard]] bool has_lanes() const noexcept { return lanes_on_; }

  // A pool treasury's view of `primary`'s pool in `asset` (core/pool_homes.hpp PoolFunding): the
  // funding needs summed by home, and the payout held for a transfer. Any thread: it reads only
  // what the engine thread publishes for it.
  void pool_funding(VenueId primary, std::string_view asset, PoolFunding& out) const noexcept {
    out = PoolFunding{};
    out.members = cfg_.pools.members(primary);
    for (const Instrument& inst : instruments_) {
      if (inst.venue != primary || inst.id.value >= kMaxInstruments) continue;
      const std::int64_t need = need_pub_[inst.id.value].load(std::memory_order_relaxed);
      if (need <= 0) continue;
      const std::string_view ccy = inst.is_derivative() ? inst.settlement_ccy() : inst.quote.view();
      if (!same_currency(ccy, asset)) continue;
      out.declared = true;
      const VenueId home{home_pub_[inst.id.value].load(std::memory_order_relaxed)};
      for (std::size_t k = 0; k < out.members.size(); ++k) {
        if (out.members[k] == home) out.need[k] = out.need[k] + Notional::from_raw(need);
      }
    }
    if (primary.value < kMaxVenues) {
      out.hold_seq = hold_seq_pub_[primary.value].load(std::memory_order_acquire);
      out.held = Notional::from_raw(hold_amt_pub_[primary.value].load(std::memory_order_relaxed));
    }
  }

  // ---- perpetuals (core/perp_book.hpp) ---------------------------------------------------------

  // The venue's mark and index price of `id`, and its funding, with their age against [accounting]
  // stale_mark_ms / stale_funding_ms at the engine clock.
  [[nodiscard]] RefPrice mark(InstrumentId id) const noexcept { return perp_.mark(id, now()); }
  [[nodiscard]] RefPrice index(InstrumentId id) const noexcept { return perp_.index(id, now()); }
  [[nodiscard]] FundingView funding(InstrumentId id) const noexcept {
    return perp_.funding(id, now());
  }
  [[nodiscard]] const PerpBook& perps() const noexcept { return perp_; }

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
    return own_qty(id, side, px, book(id).last_update());
  }
  // The best level on `side` after taking our quantity out; levels that were only ours are
  // skipped. Level{} when nothing is left.
  [[nodiscard]] Level best_ex_self(InstrumentId id, Side side) const noexcept {
    const Book& b = book(id);
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
    queue_on_ = true;
    oms_.for_each_open_order([&](Handle<Order> h, const Order& o) {
      if (resting(o.state) && instruments_.contains(o.instrument))
        queue_.place(h, o, queue_shown(o));
    });
  }
  [[nodiscard]] const QueueTracker& queue() const noexcept { return queue_; }
  [[nodiscard]] const OwnQuantity* own_quantity() const noexcept { return own_.get(); }

  // ctx.capacity: what an action on `account` costs now ([capacity]; on is false when off).
  [[nodiscard]] CapacityQuote capacity(VenueId account) const noexcept {
    if (!capacity_on_) return CapacityQuote{};
    return capacity_->quote(account, pool_primary(account), now_.ns);
  }
  [[nodiscard]] const CapacityPrice* capacity_price() const noexcept { return capacity_.get(); }

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
    r.checkpoints = stats_.checkpoints;
    r.home_unavailable = stats_.home_unavailable;
    r.home_spills = stats_.home_spills;
    for (const std::unique_ptr<TransferLane>& lane : lanes_) {
      if (lane == nullptr) continue;
      const LaneStats& l = lane->stats();
      r.lane_planned += l.planned;
      r.lane_requested += l.requested;
      r.lane_done += l.done;
      r.lane_done_by_flows += l.done_by_flows;
      r.lane_failed += l.failed;
      r.lane_unknown += l.unknown;
      r.lane_late += l.late;
      r.lane_trips += l.trips;
      r.lane_moved_raw += l.moved_value;
    }
    r.checkpoints_skipped = stats_.checkpoints_skipped;
    r.checkpoint_bytes = stats_.checkpoint_bytes;
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
    r.balance_withheld = quotes_.stats().kept_balance;
    if (capacity_on_) {
      const CapacityMinute& c = capacity_->totals();
      r.capacity_priced_out = c.priced_out_new + c.priced_out_move;
      r.capacity_observed = c.observed_new + c.observed_move;
    }
    r.estimated_fills = stats_.synthetic_fills;
    r.estimates_pending = oms_.stats().synthetic_pending;
    r.estimates_forgotten = oms_.stats().synthetic_forgotten;
    r.kill_reason = kill_reason_;
    r.kill_ts_ns = kill_ts_.ns;
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
    const Purchase buy{static_cast<double>(req.value), false};
    auto r = submit_new(req, false, capacity_on_ && value_stated(req.value) ? &buy : nullptr);
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
    if (FASTMM_UNLIKELY(!instruments_.contains(id))) return false;
    note_asked(id, q);
    if (!quoting_enabled()) return false;
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
  // ctx.note_quote: the strategy's reason for what it asks on a side (empty: none) and the budget
  // it gave the side (core/quote_diag.hpp). Cut to kQuoteNoteLen - 1 bytes.
  void note_quote(InstrumentId id, Side side, std::string_view reason, Notional budget) noexcept {
    if (FASTMM_UNLIKELY(!instruments_.contains(id))) return;
    QuoteNote& n = quote_track_[track_index(id, side)].note;
    const std::size_t len = std::min(reason.size(), kQuoteNoteLen - 1);
    for (std::size_t i = 0; i < kQuoteNoteLen; ++i) n.reason[i] = i < len ? reason[i] : '\0';
    n.budget_raw = budget.raw;
    n.set_ns = now_.ns;
  }
  // ctx.metric: a number the strategy publishes by name (the first kMaxStrategyMetrics names;
  // later ones are dropped). Cut to kStrategyMetricNameLen - 1 bytes.
  void metric(std::string_view name, double value) noexcept {
    name = name.substr(0, kStrategyMetricNameLen - 1);
    for (std::size_t k = 0; k < metric_count_; ++k) {
      if (std::string_view(metrics_[k].name) == name) {
        metrics_[k].value = value;
        return;
      }
    }
    if (metric_count_ == kMaxStrategyMetrics) return;
    LiveMetric& m = metrics_[metric_count_++];
    for (std::size_t i = 0; i < kStrategyMetricNameLen; ++i)
      m.name[i] = i < name.size() ? name[i] : '\0';
    m.value = value;
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
  // The strategy's timers (ctx.every, ctx.once). The last kEngineTimerSlots of the wheel are the
  // engine's own (ack sweep, flatten, state file, checkpoint, max_param_age): a strategy that fills
  // the wheel gets an invalid id rather than leaving a flatten without its timer.
  static constexpr std::size_t kEngineTimerSlots = 8;
  [[nodiscard]] TimerId add_timer(Duration period,
                                  bool repeat,
                                  std::uint64_t user_data = 0) noexcept {
    if (timers_.size() + kEngineTimerSlots >= TimerWheel<>::kCapacity) return TimerId{};
    return timers_.add(now(), period, repeat, user_data);
  }
  bool cancel_timer(TimerId id) noexcept { return timers_.cancel(id); }

  // Injects an event as if it came from the feed (tests, control thread bypass in sim).
  void inject(const EventHeader* h) noexcept { process(h); }

 private:
  struct Placer {
    Engine* e;
    bool operator()(QuoteAction& a) noexcept { return e->place_quote(a); }
    // A resting order to move by cancel and new rather than replace (QuoteManager).
    [[nodiscard]] bool relocates(const Order& o, VenueId account) const noexcept {
      return e->relocates(o, account);
    }
  };
  // A sweeping pool's sell resting on an account other than the home, placed by the routing: its
  // next move is a cancel and a new order, which the routing sends home, so the base it locks can
  // be swept.
  [[nodiscard]] bool relocates(const Order& o, VenueId account) const noexcept {
    if (FASTMM_LIKELY(!lanes_on_) || account.valid() || o.side != Side::Sell ||
        !instruments_.contains(o.instrument))
      return false;
    return sweeping(instruments_.get(o.instrument)) && o.venue != home_[o.instrument.value];
  }

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
  // Risk rejects the hook that just ran collected, then the quoting change it made, then the
  // rejects on_quoting collected. A strategy that asks again from on_risk_reject and is refused
  // again gets a few more rounds, not an endless one.
  void after_hooks([[maybe_unused]] bool quoting_before) noexcept {
    if constexpr (has_hook(Hook::RiskReject)) deliver_risk_rejects();
    if constexpr (has_hook(Hook::Quoting)) notify_quoting(quoting_before);
    if constexpr (has_hook(Hook::RiskReject)) deliver_risk_rejects();
  }
  FASTMM_NOINLINE void deliver_risk_rejects() noexcept {
    for (int round = 0; round < kRiskRejectRounds && !risk_rejects_.empty(); ++round) {
      const StaticVector<RiskReject, kMaxPendingRiskRejects> batch = risk_rejects_;
      risk_rejects_.clear();
      for (const RiskReject& r : batch) strategy_.on_risk_reject(ctx_, r);
      flush_out();
    }
    risk_rejects_.clear();
  }
  FASTMM_NOINLINE void queue_risk_reject(RejectReason rr,
                                         const Instrument& inst,
                                         Side side,
                                         OrderType type,
                                         Price px,
                                         Qty qty,
                                         ClientOrderId order,
                                         std::uint32_t user_tag,
                                         bool replace) noexcept {
    if (risk_rejects_.full()) {
      ++stats_.risk_reject_notices_dropped;
      return;
    }
    risk_rejects_.push_back(
        RiskReject{inst.id, side, type, rr, replace, user_tag, px, qty, order, now_});
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
    const bool quoting_before = quoting_enabled();
    if (!renews_params) check_param_age();
    check_reconcile_hold();
    check_markouts();
    // Quotes held back for want of an order token go first, oldest first, before this event's own
    // requote can take the token.
    if (FASTMM_UNLIKELY(quotes_.starved())) retry_starved();
    if (FASTMM_UNLIKELY(quotes_.backoff_due(now_))) retry_backed_off();
    if (FASTMM_UNLIKELY(cluster_on_) && cluster_.lift_due(now_)) lift_fill_cluster();
    if (FASTMM_UNLIKELY(capacity_on_)) capacity_clock();
    dispatch(h);
    after_hooks(quoting_before);
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
      case EventType::OwnLevels:
        on_own_levels(msg_cast<OwnLevelsMsg>(h));
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
      case EventType::Balance:
        on_balance(msg_cast<BalanceMsg>(h));
        break;
      case EventType::PerpState:
        on_perp_state(msg_cast<PerpStateMsg>(h));
        break;
      case EventType::Timer: {
        const auto& t = msg_cast<TimerMsg>(h);
        if constexpr (kFeedReplaysTimers) {
          // The wheel ends or repeats the timer around its hooks as it did in the session, so the
          // timers they add get the session's ids.
          if (t.timer_id.valid()) {
            static_cast<void>(
                timers_.fire_recorded(t.timer_id, t.user_data, now_, [&](TimerId, std::uint64_t) {
                  on_recorded_timer(t);
                }));
            break;
          }
        }
        on_recorded_timer(t);
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
      case EventType::StrategyState:
      case EventType::AccountFlow:
        // The balances follow BalanceMsg; a flow confirms a lane's transfer.
        if (FASTMM_UNLIKELY(lanes_on_)) on_account_flow(msg_cast<AccountFlowMsg>(h));
        break;
      case EventType::Count:
        break;  // not consumed by the L2 engine
    }
  }

  // A journaled TimerMsg: what the timer, or the batch end, did in the session.
  void on_recorded_timer(const TimerMsg& t) noexcept {
    if (t.engine == kAckSweepTimer) {
      sweep_acks();  // replay of the engine's ack_timeout sweep
    } else if (t.engine == kFlattenTimer) {
      flatten_tick();  // replay of the engine's flatten sweep
    } else if (t.engine == kStateTimer) {
      if constexpr (KeepsState<Strategy>) capture_state();
    } else if (t.engine == kBatchEndTimer) {
      call_batch_end();  // replay of a batch end
    } else if (t.engine == kCheckpointTimer) {
      journal_checkpoint();  // replay of the journal's checkpoint
    } else if (t.engine == kSweepTimer) {
      sweep_tick();  // replay of the pools' sweep
    } else if (t.engine != 0) {
      check_param_age();  // replay of the engine's max_param_age timer
    } else {
      fire_strategy_timer(t.timer_id, t.user_data);
    }
  }

  // ---- market data --------------------------------------------------------------------------

  void on_book_delta(const BookDeltaMsg& d) noexcept {
    const InstrumentId id = d.hdr.instrument;
    if (FASTMM_UNLIKELY(!instruments_.contains(id))) return;
    Book& b = books_[id.value];
    const bool own = shows_own(id.value);
    if (own_books_ != nullptr) {
      if (own) hide_own(id.value);
      b.apply_delta(d);
      if (own) show_own(id.value);
    }
    // The touch box our orders' book is woken in (update_interest) follows the market's touch.
    if (own) interest_changed(id);
    ++stats_.book_updates;
    track_feed_lag(d.hdr);
    if (FASTMM_UNLIKELY(queue_on_)) queue_on_book(d, b);
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    book_changed(id, b, t2);
  }

  // Our quantity a simulated feed shows on an instrument (OwnLevelsMsg): the book and the tickers
  // show it from now on. It comes right ahead of a message of the instrument, with which the
  // strategy sees it.
  FASTMM_NOINLINE void on_own_levels(const OwnLevelsMsg& m) noexcept {
    const InstrumentId id = m.hdr.instrument;
    if (!instruments_.contains(id) || own_shown_ == nullptr) return;
    const std::uint32_t i = id.value;
    OwnShown& o = own_shown_[i];
    const bool had = shows_own(i);
    if (own_showing_) hide_own(i);
    for (const Side side : {Side::Buy, Side::Sell}) {
      auto& v = o.levels[static_cast<std::size_t>(side)];
      for (const Level& l : side == Side::Buy ? m.bids() : m.asks()) {
        auto it =
            std::find_if(v.begin(), v.end(), [&](const Level& x) { return x.price == l.price; });
        if (it != v.end()) {
          if (l.qty.is_positive()) {
            it->qty = l.qty;
          } else {
            v.erase_at(static_cast<std::size_t>(it - v.begin()));
          }
        } else if (l.qty.is_positive()) {
          static_cast<void>(v.push_back(l));  // past kOwnLevels prices: not shown
        }
      }
    }
    const bool has = shows_own(i);
    if (has && !had) static_cast<void>(own_shown_ids_.push_back(i));
    if (!has && had) {
      auto it = std::find(own_shown_ids_.begin(), own_shown_ids_.end(), i);
      own_shown_ids_.erase_at(static_cast<std::size_t>(it - own_shown_ids_.begin()));
    }
    if (own_showing_) show_own(i);
    interest_changed(id);
  }

  // What a change of an instrument's book does besides the book: the risk engine's mid, the
  // positions' marks, a crossed book's pull, the strategy.
  void book_changed(InstrumentId id, const Book& b, Cycles t2) noexcept {
    const Timestamp now = now_;
    const Instrument& inst = instruments_.get(id);
    if (FASTMM_LIKELY(b.is_valid())) {
      const Price mid = b.mid();
      // The engine clock, which the stale check compares against: recv_ts is the network thread's
      // wall clock, and a host clock step moves it relative to the engine's TscClock.
      risk_.on_book(id, mid, now);
      if (FASTMM_LIKELY(!perp_.marking()) || perp_.mid_marks(id, now))
        positions_.mark(id, mid, inst);
      // The max_loss budget's band for this instrument no longer holds once its mid leaves it.
      if (mid.raw <= risk_lo_[id.value] || mid.raw >= risk_hi_[id.value]) risk_band_stale_ = true;
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
      const Watch& w = watch_[id.value];
      if (w.on && (!w.quiet || !b.is_valid() || b.mid() <= w.lo || b.mid() >= w.hi)) {
        const Book& view = b;
        strategy_.on_book(ctx_, id, view);
        record_strategy_hop(t2);
      }
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

  // ---- the fill-cluster guard (core/fill_cluster.hpp) ----------------------------------------
  // Quote fills on `side` reached [risk] fill_cluster_names instruments within the window: the side
  // is held on every instrument, its resting quotes cancelled now, until the hold runs out.
  FASTMM_NOINLINE void on_fill_cluster(Side side) noexcept {
    const FillClusterConfig& c = cluster_.config();
    FASTMM_LOG_INFO("fill cluster: {} fills on {} instruments within {} ms; {} pulled for {} ms",
                    side == Side::Buy ? "buy" : "sell",
                    c.names,
                    c.window.millis(),
                    side == Side::Buy ? "bids" : "asks",
                    c.hold.millis());
    quotes_.hold_side(side, true);
    enter_api();
    Placer place{this};
    for (const Instrument& inst : instruments_) quotes_.pull_side(inst, side, oms_, now_, place);
    flush_out();
  }
  // A held side's hold ran out: the strategy's last quotes go back on every instrument that quotes.
  FASTMM_NOINLINE void lift_fill_cluster() noexcept {
    bool lifted = false;
    for (const Side side : {Side::Buy, Side::Sell}) {
      if (!cluster_.lift(side, now_)) continue;
      quotes_.hold_side(side, false);
      lifted = true;
    }
    if (!lifted) return;
    enter_api();
    Placer place{this};
    for (const Instrument& inst : instruments_) {
      if (quoting_enabled(inst.id)) quotes_.reapply(inst, oms_, now_, place);
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
      if (FASTMM_UNLIKELY(queue_on_)) queue_on_trade(t);
    }
    track_feed_lag(t.hdr);
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    if constexpr (has_hook(Hook::Trade)) {
      if (FASTMM_LIKELY(known_instrument && watch_[id.value].on)) {
        strategy_.on_trade(ctx_, id, t);
        record_strategy_hop(t2);
      }
    }
    flush_out();
  }

  void on_book_ticker(const BookTickerMsg& recorded) noexcept {
    // With our orders shown, the ticker shows our best orders too (show_own_top).
    const InstrumentId id = recorded.hdr.instrument;
    BookTickerMsg shown{};
    const bool own = instruments_.contains(id) && shows_own(id.value);
    if (FASTMM_UNLIKELY(own)) {
      shown = recorded;
      show_own_top(id.value, shown);
    }
    const BookTickerMsg& m = own ? shown : recorded;
    track_feed_lag(m.hdr);
    if (FASTMM_UNLIKELY(queue_on_)) queue_on_ticker(m);
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    if constexpr (has_hook(Hook::BookTicker)) {
      if (FASTMM_LIKELY(instruments_.contains(id) && watch_[id.value].on)) {
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
      if (FASTMM_LIKELY(instruments_.contains(id) && watch_[id.value].on)) {
        strategy_.on_option_ticker(ctx_, id, m);
        record_strategy_hop(t2);
      }
    }
    flush_out();
  }

  // A venue's mark, index and funding of one derivative. With [accounting] mark = "venue" a mark
  // values the position from now on (PerpBook::on_report), and counts against max_loss at once.
  FASTMM_NOINLINE void on_perp_state(const PerpStateMsg& m) noexcept {
    const InstrumentId id = m.hdr.instrument;
    if (FASTMM_UNLIKELY(!instruments_.contains(id))) return;
    if (perp_.on_report(m, now_)) {
      positions_.mark(id, m.mark_price, instruments_.get(id));
      if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
    }
    if constexpr (has_hook(Hook::PerpState)) strategy_.on_perp_state(ctx_, id, m);
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
    // A late fill changed an order that had already ended: only its cum_qty and state are known.
    if (u.action == OmsAction::LateFill)
      r.hdr.flags |= RecordHeader::kLate | RecordHeader::kTerminal;
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

  // RejectRecord folding, per instrument, side and account (record_reject).
  struct RejectFold {
    std::int64_t last_ns = std::numeric_limits<std::int64_t>::min() / 2;  // the last record
    std::int64_t seen_ns = std::numeric_limits<std::int64_t>::min() / 2;  // the last refusal
    std::int64_t hold_ns = RejectRecord::kFoldNs;  // refusals within this of last_ns are counted
    std::uint32_t pending = 0;
    RejectReason reason{};
    RejectSource source{};
    VenueId account{};
    std::uint8_t flags = 0;
    Price px{};
    Qty qty{};
  };
  // ---- quote diagnostics (core/quote_diag.hpp) --------------------------------------------------

  // Quote diagnostics per instrument and side (publish_quotes): what set_quotes was asked, the
  // last refusal, the strategy's note, and the obstacle last published with when it began.
  static constexpr std::int64_t kRefusedNs = 2'000'000'000;
  struct QuoteTrack {
    std::int64_t asked_px_raw = 0;
    std::int64_t asked_qty_raw = 0;
    std::int64_t refused_ns = 0;
    std::int64_t since_ns = 0;
    std::uint8_t asked_levels = 0;
    RejectReason refused = RejectReason::None;
    QuoteBlock block = QuoteBlock::NotWanted;
    QuoteNote note;
  };

  [[nodiscard]] static std::size_t track_index(InstrumentId id, Side side) noexcept {
    return (static_cast<std::size_t>(id.value) * 2) + (side == Side::Sell ? 1U : 0U);
  }
  // What set_quotes was asked on each side, whether or not it could apply it.
  void note_asked(InstrumentId id, const DesiredQuotes& q) noexcept {
    for (const Side side : {Side::Buy, Side::Sell}) {
      QuoteTrack& t = quote_track_[track_index(id, side)];
      const auto& levels = q.side(side);
      t.asked_levels = static_cast<std::uint8_t>(levels.size());
      t.asked_px_raw = levels.empty() ? 0 : levels[0].price.raw;
      std::int64_t qty = 0;
      for (const Level& l : levels) qty += l.qty.raw;
      t.asked_qty_raw = qty;
    }
  }
  void note_refusal(InstrumentId id, Side side, RejectReason reason) noexcept {
    if (id.value >= kMaxInstruments) return;
    QuoteTrack& t = quote_track_[track_index(id, side)];
    t.refused_ns = now_.ns;
    t.refused = reason;
  }
  // The first obstacle between what the strategy asked for on the side and orders working there.
  [[nodiscard]] QuoteBlock quote_block(const Instrument& inst,
                                       Side side,
                                       const QuoteTrack& t,
                                       std::int64_t working_qty) const noexcept {
    const InstrumentId id = inst.id;
    if (t.asked_qty_raw <= 0) return QuoteBlock::NotWanted;
    if (!quoting_enabled()) return QuoteBlock::QuotingOff;
    // A venue kill pulls the venue's quotes too; the kill is the cause to report.
    if (risk_.venue_killed(inst.venue)) return QuoteBlock::VenueKilled;
    if (inst_pulled_[id.value] || quotes_.pulled(id) || venue_is_pulled(inst.venue))
      return QuoteBlock::Pulled;
    if (health_.gated(inst.venue, now_)) return QuoteBlock::FeedLag;
    if (quotes_.side_held(side)) return QuoteBlock::FillCluster;
    // The QuoteManager keeps an order whose leaves cover min_qty_bps of the target.
    if (static_cast<Int128>(working_qty) * 10'000 >=
        static_cast<Int128>(t.asked_qty_raw) * quotes_.params().min_qty_bps)
      return QuoteBlock::Quoting;
    if (quotes_.backing_off(id, side, now_)) return QuoteBlock::Backoff;
    if (quotes_.starved(id)) return QuoteBlock::Starved;
    if (t.refused != RejectReason::None && now_.ns - t.refused_ns < kRefusedNs)
      return QuoteBlock::Refused;
    return QuoteBlock::Pending;
  }
  // Fills live.quotes; a side's obstacle keeps the time it began until it changes.
  void publish_quotes(EngineLiveStats& live) noexcept {
    std::uint32_t n = 0;
    for (const Instrument& inst : instruments_) {
      if (n == kMaxInstruments) break;
      LiveQuoteInstrument& q = live.quotes[n++];
      const Position& p = positions_.get(inst.id);
      q.instrument = inst.id.value;
      q.venue = inst.venue.value;
      q.position_raw = p.qty.raw;
      q.avg_px_raw = p.avg_px.raw;
      q.realized_raw = p.realized.raw;
      q.unrealized_raw = p.unrealized.raw;
      q.mid_raw = books_[inst.id.value].mid().raw;
      q.reduce_only = reduce_only_[inst.id.value] ? 1 : 0;
      q.arm = ab_arm_[inst.id.value];
      q.own_params = static_cast<std::uint8_t>(param_own_[inst.id.value].count());
      q.home = home_[inst.id.value].value;
      q.pieces = static_cast<std::uint8_t>(pieces(inst.id));
      for (const Side side : {Side::Buy, Side::Sell}) {
        QuoteTrack& t = quote_track_[track_index(inst.id, side)];
        LiveQuoteSide& out = q.sides[static_cast<std::size_t>(side)];
        out.desired_px_raw = t.asked_px_raw;
        out.desired_qty_raw = t.asked_qty_raw;
        out.desired_levels = t.asked_levels;
        std::int64_t working = 0;
        std::int64_t best = 0;
        std::uint16_t orders = 0;
        for (std::size_t k = 0; k < kQuoteDiagAccounts; ++k) out.account_qty_raw[k] = 0;
        for (std::uint32_t lvl = 0; lvl < kMaxQuoteLevels; ++lvl) {
          const Handle<Order> h = quotes_.slot_handle(inst.id, side, lvl);
          if (!h.valid() || !oms_.is_live(h)) continue;
          const Order& o = oms_.get(h);
          const std::int64_t leaves = o.leaves_qty().raw;
          if (leaves <= 0) continue;
          working += leaves;
          ++orders;
          if (best == 0 || (side == Side::Buy ? o.price.raw > best : o.price.raw < best))
            best = o.price.raw;
          const std::size_t a = pools_on_ ? pool_index(inst.venue, o.venue) : 0;
          if (a < kQuoteDiagAccounts) out.account_qty_raw[a] += leaves;
        }
        out.working_qty_raw = working;
        out.working_px_raw = best;
        out.working_orders = orders;
        const QuoteBlock block = quote_block(inst, side, t, working);
        out.reason = block == QuoteBlock::Refused ? t.refused : RejectReason::None;
        if (block != t.block || t.since_ns == 0) {
          t.since_ns = now_.ns;
          t.block = block;
        }
        out.block = block;
        out.block_since_ns = t.since_ns;
        out.note = t.note;
      }
    }
    live.quote_count = n;
  }
  // Which of an account's limits leaves no room (no_order_room was true).
  [[nodiscard]] static RejectSource room_refusal(const OrderBudget& b) noexcept {
    if (b.venue_paused) return RejectSource::AccountPaused;
    if (b.orders_10s.remaining() == 0) return RejectSource::AccountOrders10s;
    if (b.orders_1m.remaining() == 0) return RejectSource::AccountOrders1m;
    return RejectSource::AccountOrders1d;
  }
  // With the open orders on its side, an order of `qty` only takes the position towards zero.
  [[nodiscard]] bool reduces(InstrumentId id, Side side, Qty qty) const noexcept {
    if (!instruments_.contains(id)) return false;
    const std::int64_t q = positions_.get(id).qty.raw;
    if (q == 0 || (q > 0) == (side == Side::Buy)) return false;
    return oms_.open_qty(id, side).raw + qty.raw <= (q < 0 ? -q : q);
  }

  // One refusal for the store (RejectRecord), with the account's budget as the engine sees it
  // now. Refusals of the instrument, side and account with the same reason, source and flags
  // within the fold's hold of the last record are counted, not recorded; a different one first
  // records what was counted. The hold starts at kFoldNs and doubles with each record of a run of
  // the same refusal, up to kFoldMaxNs, so a strategy retrying a refused order on every event or
  // tick writes a few records a minute however long the run lasts; a refusal after kFoldMaxNs
  // without one, or a different one, starts again from kFoldNs. Each account folds on its own
  // (reject_fold_index): a pool's accounts refused in turn would otherwise all be counted under the
  // first one, and an exit under an order that adds.
  FASTMM_NOINLINE void record_reject(RejectReason reason,
                                     RejectSource source,
                                     InstrumentId id,
                                     VenueId account,
                                     Side side,
                                     Price px,
                                     Qty qty,
                                     std::uint8_t flags,
                                     ClientOrderId cl_ord_id = ClientOrderId{},
                                     std::int32_t venue_code = 0,
                                     std::string_view text = {}) noexcept {
    RejectFold* fold = nullptr;
    std::uint32_t folded = 0;
    if (id.value < kMaxInstruments && account.value < kMaxVenues) {
      fold = &reject_folds_[reject_fold_index(id, side, account)];
      const bool same = fold->reason == reason && fold->source == source && fold->flags == flags;
      const bool run = same && now_.ns - fold->seen_ns < RejectRecord::kFoldMaxNs;
      fold->seen_ns = now_.ns;
      if (run && now_.ns - fold->last_ns < fold->hold_ns) {
        ++fold->pending;
        reject_folds_pending_ = true;
        return;
      }
      if (same) {
        folded = fold->pending;  // this record stands for the ones counted since the last
      } else if (fold->pending != 0) {
        flush_reject_fold(*fold, id, side);
      }
      fold->hold_ns =
          run ? std::min(fold->hold_ns * 2, RejectRecord::kFoldMaxNs) : RejectRecord::kFoldNs;
      fold->pending = 0;
    }
    emit_reject(
        reason, source, id, account, side, px, qty, flags, cl_ord_id, venue_code, text, folded);
    if (fold != nullptr) {
      fold->last_ns = now_.ns;
      fold->reason = reason;
      fold->source = source;
      fold->account = account;
      fold->flags = flags;
      fold->px = px;
      fold->qty = qty;
    }
  }
  // The refusals a fold counted since its last record, as one record (the first of them is the
  // record, the rest `folded`), with the budget as it is now.
  void flush_reject_fold(RejectFold& f, InstrumentId id, Side side) noexcept {
    emit_reject(f.reason,
                f.source,
                id,
                f.account,
                side,
                f.px,
                f.qty,
                f.flags,
                ClientOrderId{},
                0,
                {},
                f.pending - 1);
    f.pending = 0;
    f.last_ns = now_.ns;
    f.hold_ns = std::min(f.hold_ns * 2, RejectRecord::kFoldMaxNs);
  }
  [[nodiscard]] static std::size_t reject_fold_index(InstrumentId id,
                                                     Side side,
                                                     VenueId account) noexcept {
    return (((static_cast<std::size_t>(id.value) * 2) + (side == Side::Sell ? 1U : 0U)) *
            kMaxVenues) +
           account.value;
  }
  // Folds that have counted refusals and whose hold has passed by `now`: recorded with each
  // periodic publication (publish_latency), rather than when that account is refused again or the
  // session ends. `now` is the publication's clock: engine time (now_) moves only with events.
  void flush_stale_reject_folds(Timestamp now) noexcept {
    if (!reject_folds_pending_) return;
    bool left = false;
    for (std::size_t k = 0; k < kRejectFolds; ++k) {
      RejectFold& f = reject_folds_[k];
      if (f.pending == 0) continue;
      if (now.ns - f.last_ns < f.hold_ns) {
        left = true;
        continue;
      }
      flush_reject_fold(f,
                        InstrumentId{static_cast<std::uint32_t>(k / (2 * kMaxVenues))},
                        (k / kMaxVenues) % 2 == 0 ? Side::Buy : Side::Sell);
    }
    reject_folds_pending_ = left;
  }
  void flush_reject_folds() noexcept {
    for (std::size_t k = 0; k < kRejectFolds; ++k) {
      if (reject_folds_[k].pending != 0)
        flush_reject_fold(reject_folds_[k],
                          InstrumentId{static_cast<std::uint32_t>(k / (2 * kMaxVenues))},
                          (k / kMaxVenues) % 2 == 0 ? Side::Buy : Side::Sell);
    }
  }
  void emit_reject(RejectReason reason,
                   RejectSource source,
                   InstrumentId id,
                   VenueId account,
                   Side side,
                   Price px,
                   Qty qty,
                   std::uint8_t flags,
                   ClientOrderId cl_ord_id,
                   std::int32_t venue_code,
                   std::string_view text,
                   std::uint32_t folded) noexcept {
    RejectRecord r;
    records_.init(r, RecordType::Reject, id, account, now_, now_);
    r.reason = reason;
    r.source = source;
    r.side = side;
    r.flags = flags;
    r.folded = folded;
    r.price = px;
    r.qty = qty;
    r.cl_ord_id = cl_ord_id;
    const OrderBudget b = account.valid() ? order_budget(account) : OrderBudget{};
    r.local_tokens = b.local_tokens;
    r.local_wait_ns = b.local_wait_ns;
    const auto admits = [](const RateWindow& w) { return w.known() ? w.admits() : 0; };
    r.orders_10s_used = b.orders_10s.used;
    r.orders_10s_admits = admits(b.orders_10s);
    r.orders_1m_used = b.orders_1m.used;
    r.orders_1m_admits = admits(b.orders_1m);
    r.orders_1d_used = b.orders_1d.used;
    r.orders_1d_admits = admits(b.orders_1d);
    r.weight_used = b.weight.used;
    r.weight_admits = admits(b.weight);
    r.venue_code = venue_code;
    r.budget_known = b.venue_known ? 1 : 0;
    r.paused = b.venue_paused ? 1 : 0;
    std::memset(r.pad0_, 0, sizeof r.pad0_);
    std::memset(r.pad_, 0, sizeof r.pad_);
    r.text = FixedString<40>(text);
    account_record(records_.put(r.hdr));
  }

  // A parameter update the engine applied, for the store's parameter history.
  void emit_param(const ParamUpdateMsg& m) noexcept {
    if (!records_.enabled()) return;
    ParamRecord r;
    records_.init(r, RecordType::Param, m.hdr.instrument, VenueId::invalid(), now_, now_);
    static_assert(sizeof(ParamRecord) - sizeof(RecordHeader) ==
                  sizeof(ParamUpdateMsg) - sizeof(EventHeader));
    std::memcpy(reinterpret_cast<std::byte*>(&r) + sizeof(RecordHeader),
                reinterpret_cast<const std::byte*>(&m) + sizeof(EventHeader),
                sizeof(ParamRecord) - sizeof(RecordHeader));
    account_record(records_.put(r.hdr));
  }

  // An operator's `limits` the engine applied, for the store's limit history.
  void emit_limits(LimitsRecord& r) noexcept {
    if (!records_.enabled()) return;
    records_.init(r, RecordType::Limits, InstrumentId{}, VenueId::invalid(), now_, now_);
    account_record(records_.put(r.hdr));
  }

  // The venue's execution replay ended complete: the records before this one hold every
  // execution it made until now.
  void emit_replayed(VenueId venue) noexcept {
    if (!records_.enabled()) return;
    ReplayedRecord r;
    records_.init(r, RecordType::Replayed, InstrumentId{}, venue, now_, now_);
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
                 const OrderFillMsg* msg,
                 Qty named = Qty{}) noexcept {
    if (!records_.enabled() || !instruments_.contains(id)) return;
    FillRecord r;
    // The account it filled on: a pool member's id where the order went to one.
    const VenueId venue = msg != nullptr          ? msg->hdr.venue
                          : u.order.venue.valid() ? u.order.venue
                                                  : instruments_.get(id).venue;
    records_.init(r, RecordType::Fill, id, venue, now_, now_);
    if (msg == nullptr) r.hdr.flags |= RecordHeader::kSynthetic;
    if (u.action == OmsAction::LateFill) r.hdr.flags |= RecordHeader::kLate;
    if (u.action == OmsAction::UnknownFill) r.hdr.flags |= RecordHeader::kUnknown;
    if (u.terminal) r.hdr.flags |= RecordHeader::kTerminal;
    r.cl_ord_id = msg != nullptr ? msg->cl_ord_id : u.order.cl_ord_id;
    r.price = px;
    r.qty = qty;
    r.booked_qty = booked;
    r.named_qty = named;
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
    r.arm = u.order.arm;
    r.venue_order_id = msg != nullptr ? msg->venue_order_id : u.order.venue_order_id;
    if (msg != nullptr) {
      r.exec_id = msg->exec_id;
      r.hdr.exch_ts = msg->hdr.exch_ts;
    }
    const bool put = records_.put(r.hdr);
    account_record(put);
    if (put && !markouts_.empty()) start_markouts(id, records_.written());
    emit_position(id);
  }

  // [engine] markout_ms: the mid now, and the fill queued for each horizon.
  void start_markouts(InstrumentId id, std::uint64_t fill_seq) noexcept {
    emit_markout(id, fill_seq, now_.ns, 0);
    for (MarkoutQueue& q : markouts_) {
      if (q.size == q.ring.size()) {
        ++stats_.markouts_dropped;
        continue;
      }
      const std::int64_t due = now_.ns + std::int64_t{q.horizon_ms} * 1'000'000;
      q.ring[(q.head + q.size) % q.ring.size()] = {due, now_.ns, fill_seq, id};
      ++q.size;
      if (due < markout_next_ns_) markout_next_ns_ = due;
    }
  }
  void emit_markout(InstrumentId id,
                    std::uint64_t fill_seq,
                    std::int64_t fill_ns,
                    std::int32_t horizon_ms) noexcept {
    const Price mid = books_[id.value].mid();
    if (!mid.is_positive()) return;
    MarkoutRecord r;
    records_.init(r, RecordType::Markout, id, instruments_.get(id).venue, now_, now_);
    r.fill_seq = fill_seq;
    r.fill_ts = Timestamp{fill_ns};
    r.mid = mid;
    r.horizon_ms = horizon_ms;
    account_record(records_.put(r.hdr));
  }
  // The markouts that have come due, before each event and timer.
  FASTMM_FORCE_INLINE void check_markouts() noexcept {
    if (FASTMM_LIKELY(now_.ns < markout_next_ns_)) return;
    std::int64_t next = std::numeric_limits<std::int64_t>::max();
    for (MarkoutQueue& q : markouts_) {
      while (q.size != 0 && q.ring[q.head].due_ns <= now_.ns) {
        const PendingMarkout& p = q.ring[q.head];
        emit_markout(p.instrument, p.fill_seq, p.fill_ns, q.horizon_ms);
        q.head = (q.head + 1) % q.ring.size();
        --q.size;
      }
      if (q.size != 0 && q.ring[q.head].due_ns < next) next = q.ring[q.head].due_ns;
    }
    markout_next_ns_ = next;
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
    // On Binance this is mostly a cancel ack that names a fill the private stream delivers a
    // fraction of a millisecond later; an estimate no execution names shows in
    // fastmm_estimates_pending and, once the order is forgotten, as an error.
    FASTMM_LOG_WARN(
        "order {} on {}:{}: the venue reports {} filled ahead of its executions, booked at the "
        "order's price {} until one names it",
        encode_cl_ord_id(u.order.cl_ord_id),
        venue_name(inst.venue),
        inst.symbol,
        u.missed_qty,
        u.order.price);
    const std::int64_t pos_before = positions_.get(u.order.instrument).qty.raw;
    positions_.on_fill(
        u.order.instrument, u.order.side, u.order.price, u.missed_qty, Notional{}, inst);
    note_held(u.order.instrument, u.order.venue, u.order.side, u.missed_qty);
    if (balances_live_)
      balances_->on_fill(u.order.instrument,
                         inst,
                         u.order.side,
                         u.order.price,
                         u.missed_qty,
                         Notional{},
                         FeeAsset::Quote,
                         pos_before,
                         positions_.get(u.order.instrument).qty.raw,
                         Timestamp{},
                         u.order.venue);
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
    queue_.on_book(
        d,
        b,
        [&](Side s, Price px) { return own_qty(id, s, px, b.last_update()); },
        [&](Side s, Price px, Qty qty) {
          if (own_shown_ != nullptr) {
            for (const typename OwnShown::Added& a : own_shown_[id.value].added) {
              if (a.side == s && a.px == px) qty += a.qty;
            }
          }
          return qty;
        });
  }
  FASTMM_NOINLINE void queue_on_trade(const TradeMsg& t) noexcept {
    queue_.on_trade(t, oms_, books_[t.hdr.instrument.value]);
  }
  FASTMM_NOINLINE void queue_on_ticker(const BookTickerMsg& m) noexcept {
    const InstrumentId id = m.hdr.instrument;
    if (!instruments_.contains(id)) return;
    queue_.on_ticker(
        m, books_[id.value], [&](Side s, Price p, Timestamp t) { return own_qty(id, s, p, t); });
  }
  // What the queue model places an order behind: the displayed quantity at its price less our
  // own, or the touch of a BookTicker newer than the depth book.
  [[nodiscard]] Qty queue_shown(const Order& o) const noexcept {
    const Book& b = book(o.instrument);
    const Qty shown = level_qty(b, o.side, o.price);
    const Qty own = own_qty(o.instrument, o.side, o.price);
    return queue_.at_placement(others_shown(shown, own), o, b, [&](Side s, Price p, Timestamp t) {
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
    if (queue_on_ && u.handle.valid() && resting(u.order.state) && !queue_.tracked(u.handle) &&
        instruments_.contains(u.order.instrument))
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

  // An estimate (book_missed_fill) outlived the OMS's memory of its order: kRecentlyTerminal orders
  // ended after it and no execution named it. Its price and fee stay estimates, and an execution
  // of that order the venue replays from now on would add its quantity to the position again.
  // OmsStats::synthetic_forgotten counts these (the status file, the metrics export).
  void report_forgotten(const OmsUpdate& u) noexcept {
    if (FASTMM_LIKELY(u.forgotten_qty.is_zero())) return;
    FASTMM_LOG_ERROR(
        "order {}: {} booked as an estimate was never named by an execution, and the order is no "
        "longer remembered; an execution of it the venue replays from now on would be booked "
        "again",
        encode_cl_ord_id(u.forgotten_id),
        u.forgotten_qty);
  }

  // `hold_done`: on_fill moved the order's hold already.
  void after_oms_update(const OmsUpdate& u, const EventHeader& h, bool hold_done = false) noexcept {
    if (FASTMM_UNLIKELY(balances_live_) && !hold_done && u.changed && u.slot.valid())
      update_hold(u, h.exch_ts);
    if (u.known) {
      const int delta =
          static_cast<int>(resting(u.order.state)) - static_cast<int>(resting(u.prev));
      if (delta != 0) presence_.on_live_change(u.order.instrument, u.order.side, delta, now_);
      if (u.changed) exec_view_update(u, h);
    }
    book_missed_fill(u);
    report_unresolved(u);
    report_forgotten(u);
    if (u.action == OmsAction::CancelUnknown) cancel_unknown(h, u);
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
    if (const Handle<Order> h = queue_on_ ? oms_.find(m.cl_ord_id) : Handle<Order>{};
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
    // An order in flight when reduce-only mode came on could not be cancelled then.
    if (FASTMM_UNLIKELY(ro_count_ != 0) && u.changed && u.handle.valid())
      cancel_if_adding(u.handle);
  }
  void on_order_reject(const OrderRejectMsg& m) noexcept {
    own_inbound(m.hdr);
    const OmsUpdate u = oms_.on_reject(m);
    if (u.changed) {
      ++stats_.venue_rejects;
      stats_.venue_rejects_by_reason.add(m.reason);
      // The connector's own limiter refused it (never sent): logged as the risk refusals are, at
      // most once per reason and reject_log_interval, so retries into a full window do not flood
      // the log.
      const bool local = m.text.view().starts_with("local rate limit");
      std::uint64_t suppressed = 0;
      if (!local) {
        FASTMM_LOG_WARN(
            "order {} rejected: {} ({})", encode_cl_ord_id(m.cl_ord_id), m.reason, m.venue_code);
      } else if (reject_log_.admit(m.reason, now_, suppressed)) {
        FASTMM_LOG_WARN("order {} rejected: {} ({}) ({} more suppressed)",
                        encode_cl_ord_id(m.cl_ord_id),
                        m.reason,
                        m.text.view(),
                        suppressed);
      }
      if (u.order.instrument.valid()) note_refusal(u.order.instrument, u.order.side, m.reason);
      if (FASTMM_UNLIKELY(records_.enabled()) && u.order.instrument.valid()) {
        const Order& o = u.order;
        std::uint8_t flags = RejectRecord::kFromVenue;
        if (o.has(Order::kReduceOnly)) flags |= RejectRecord::kReduceOnly;
        if (reduces(o.instrument, o.side, o.qty)) flags |= RejectRecord::kReduces;
        record_reject(m.reason,
                      local ? RejectSource::VenueLocal : RejectSource::Venue,
                      o.instrument,
                      o.venue,
                      o.side,
                      o.price,
                      o.qty,
                      flags,
                      m.cl_ord_id,
                      m.venue_code,
                      m.text.view());
      }
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
    if (FASTMM_UNLIKELY(queue_on_) && known_instrument && f.liquidity == Liquidity::Maker &&
        (f.flags & OrderFillMsg::kReplayed) == 0)
      queue_.on_own_fill(id, side, f.qty, f.hdr.exch_ts.valid() ? f.hdr.exch_ts : f.hdr.recv_ts);
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
      Qty named{};
      if (FASTMM_UNLIKELY(!u.corrected_qty.is_zero())) {
        const Qty corrected = u.corrected_qty < booked ? u.corrected_qty : booked;
        named = corrected;
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
      const std::int64_t pos_before = positions_.get(id).qty.raw;
      if (booked.is_positive()) positions_.on_fill(id, side, f.price, booked, fee, inst);
      if (FASTMM_UNLIKELY(pools_on_) && booked.is_positive())
        note_held(id, from_order ? u.order.venue : f.hdr.venue, side, booked);
      if (FASTMM_UNLIKELY(balances_live_)) {
        // The same for the assets: the quantity the synthetic fill moved already is only repriced,
        // the rest of the execution moves them (with the whole fee).
        const VenueId account = from_order ? u.order.venue : f.hdr.venue;
        balances_->correct_fill(
            id, inst, side, u.synthetic_px, f.price, u.corrected_qty, f.hdr.exch_ts, account);
        balances_->on_fill(id,
                           inst,
                           side,
                           f.price,
                           f.qty - u.corrected_qty,
                           f.fee,
                           fee_asset,
                           pos_before,
                           positions_.get(id).qty.raw,
                           f.hdr.exch_ts,
                           account);
      }
      emit_fill(id, side, f.price, f.qty, booked, exec_fee, fee_asset, u, &f, named);
      if (FASTMM_UNLIKELY(capacity_on_) && from_order && u.known) {
        capacity_->on_fill(u.order.venue,
                           id,
                           side,
                           inst.notional(u.order.price, u.order.qty).to_double(),
                           inst.notional(f.price, f.qty).to_double(),
                           (u.order.cum_qty - f.qty).raw <= 0,
                           now_.ns);
      }
      if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
      // The fill-cluster guard counts the maker fills of quotes (orders the QuoteManager placed).
      if (FASTMM_UNLIKELY(cluster_on_) && f.liquidity == Liquidity::Maker && u.known &&
          (f.flags & OrderFillMsg::kReplayed) == 0 &&
          QuoteManager::is_quote_tag(u.order.user_tag) && cluster_.on_fill(id, side, now_))
        on_fill_cluster(side);
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
    // The order's hold follows the fill before the strategy runs: its on_fill can place an order in
    // the pool slot a last fill just freed, and the holds are kept by slot.
    if (FASTMM_UNLIKELY(balances_live_) && u.changed && u.slot.valid())
      update_hold(u, f.hdr.exch_ts);
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
    after_oms_update(u, f.hdr, true);
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
      FASTMM_LOG_ERROR(
          "funding {} of {} {} on {}:{} is not in its settlement currency {}: not booked",
          m.funding_id.view(),
          m.amount,
          m.asset.view(),
          venue_name(inst.venue),
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
    set_reported_position(m.hdr.instrument, m.hdr.venue, m.qty, m.avg_px);
    emit_position(m.hdr.instrument);
    if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
  }

  // A venue's report of an instrument's position. With a pool each account reports its own, and
  // the instrument's position is their sum (the average price weighted by size); without one the
  // report is the position.
  void set_reported_position(InstrumentId id, VenueId account, Qty qty, Price avg_px) noexcept {
    const Instrument& inst = instruments_.get(id);
    if (FASTMM_LIKELY(!pools_on_) || !cfg_.pools.pooled(inst.venue)) {
      positions_.set(id, qty, avg_px, inst);
      return;
    }
    const PoolMembers members = cfg_.pools.members(inst.venue);
    std::size_t k = members.size();
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (members[i] == account) k = i;
    }
    if (k == members.size()) {
      positions_.set(id, qty, avg_px, inst);  // not an account of the pool: as reported
      return;
    }
    if (pool_pos_ == nullptr) pool_pos_ = std::make_unique<PoolPositions>();
    PoolPosition& mine = (*pool_pos_)[id.value][k];
    if (held_ != nullptr) (*held_)[id.value][k] = qty;
    mine.qty = qty;
    mine.avg_px = avg_px;
    mine.reported = true;
    std::int64_t sum = 0;
    Int128 weighted = 0;
    std::int64_t size = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
      const PoolPosition& p = (*pool_pos_)[id.value][i];
      if (!p.reported) continue;
      sum += p.qty.raw;
      const std::int64_t abs = p.qty.raw < 0 ? -p.qty.raw : p.qty.raw;
      weighted += static_cast<Int128>(p.avg_px.raw) * abs;
      size += abs;
    }
    const Price avg =
        size > 0 ? Price::from_raw(static_cast<std::int64_t>(weighted / size)) : Price{};
    positions_.set(id, Qty::from_raw(sum), avg, inst);
  }

  // ---- balances -------------------------------------------------------------------------------

  // A venue's report of one asset. The first one starts the estimate: from here on every open
  // order's hold is known (holds_) and moves with the order.
  FASTMM_NOINLINE void on_balance(const BalanceMsg& m) noexcept {
    if (!balances_live_) start_balances();
    balances_->on_report(m);
    if (FASTMM_UNLIKELY(homes_on_) && (m.flags & BalanceMsg::kSnapshotEnd) != 0)
      note_snapshot(m.hdr.venue);
    if constexpr (has_hook(Hook::Balance)) strategy_.on_balance(ctx_, m);
    flush_out();
  }
  FASTMM_NOINLINE void start_balances() noexcept {
    balances_live_ = true;
    oms_.for_each_open_order([&](Handle<Order> h, const Order& o) {
      if (!instruments_.contains(o.instrument)) return;
      const std::int64_t amount = order_hold(o);
      holds_[h.idx] = amount;
      unacked_[h.idx] = !acknowledged(o);
      balances_->move_hold(o.instrument, o.side, 0, amount, Timestamp{}, unacked_[h.idx], o.venue);
    });
  }
  // The venue has taken the order (its ack, or a fill of it, came).
  [[nodiscard]] static bool acknowledged(const Order& o) noexcept {
    return o.state != OrderState::PendingNew &&
           (o.state != OrderState::PendingCancel || !o.venue_order_id.empty());
  }
  // The price an order holds at: its own, or the book's mid for a market order.
  [[nodiscard]] Price hold_price(InstrumentId id, OrderType type, Price px) const noexcept {
    if (type != OrderType::Market) return px;
    const Book& b = book(id);
    return b.is_valid() ? b.mid() : Price{};
  }
  [[nodiscard]] std::int64_t order_hold(const Order& o) const noexcept {
    return balances_->hold(o.instrument,
                           instruments_.get(o.instrument),
                           o.side,
                           hold_price(o.instrument, o.type, o.price),
                           o.leaves_qty(),
                           o.has(Order::kReduceOnly),
                           o.venue);
  }
  // An order changed: its hold follows (zero once it is terminal), at the venue time of the event.
  // Its first ack (or fill) tells the balances the venue has it.
  FASTMM_NOINLINE void update_hold(const OmsUpdate& u, Timestamp venue_ts) noexcept {
    if (!instruments_.contains(u.order.instrument)) return;
    const std::int64_t now_hold = u.terminal ? 0 : order_hold(u.order);
    std::int64_t& held = holds_[u.slot.idx];
    bool& unacked = unacked_[u.slot.idx];
    if (unacked && !u.terminal && acknowledged(u.order)) {
      balances_->acknowledge(u.order.instrument, u.order.side, held, venue_ts, u.order.venue);
      unacked = false;
    }
    if (now_hold != held)
      balances_->move_hold(
          u.order.instrument, u.order.side, held, now_hold, venue_ts, unacked, u.order.venue);
    held = now_hold;
    if (u.terminal) unacked = false;
  }
  // [risk] check_balance for an order of `qty` at `px`; `replaced`: what the order it replaces
  // holds. False: the balance does not cover it.
  FASTMM_NOINLINE bool balance_covers(const Instrument& inst,
                                      VenueId account,
                                      Side side,
                                      OrderType type,
                                      Price px,
                                      Qty qty,
                                      std::int64_t replaced,
                                      bool reduce_only,
                                      Qty open_same_side) const noexcept {
    const std::int64_t q = positions_.get(inst.id).qty.raw;
    const bool reduces = q != 0 && (q > 0) != (side == Side::Buy) &&
                         open_same_side.raw + qty.raw <= (q < 0 ? -q : q);
    return balances_->covers(inst.id,
                             inst,
                             side,
                             hold_price(inst.id, type, px),
                             qty,
                             replaced,
                             reduce_only,
                             reduces,
                             account);
  }

  // ---- parameters -----------------------------------------------------------------------------

  void on_param_update(const ParamUpdateMsg& m) noexcept {
    ++stats_.param_updates;
    last_param_ = m;
    last_param_ts_ = now_;
    if (m.origin == ParamUpdateMsg::Origin::Control) control_param_seq_ = m.publish_seq;
    if (m.arm != ParamUpdateMsg::kArmKeep) {
      const std::uint8_t arm = m.arm == ParamUpdateMsg::kArmNone ? 0 : m.arm;
      if (m.all_instruments()) {
        ab_arm_.fill(arm);
      } else if (m.hdr.instrument.value < kMaxInstruments) {
        ab_arm_[m.hdr.instrument.value] = arm;
      }
    }
    // The instruments' own values, for the status file; the strategy keeps its own copy.
    apply_param_pairs(
        m, std::span<ParamMask>(param_own_), kMaxParams, [](std::size_t, std::size_t) {});
    emit_param(m);
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
    // A control update at once rather than with the next latency publication: fastmm-ctl param
    // --wait reads it. A strategy's own updates (a model, every event in a backtest) wait.
    if (m.origin == ParamUpdateMsg::Origin::Control) publish_live(latency_pub_.load());
  }

  // A reconciliation's End normally follows its Begin in the same batch (the connector sends the
  // snapshot whole). One still open kReconcileHold after its Begin (an End lost or held up) pulls
  // the held quotes, keeping them for End to place again: the strategy cannot move them meanwhile.
  FASTMM_FORCE_INLINE void check_reconcile_hold() noexcept {
    if (FASTMM_LIKELY(reconciling_ == 0) || reconcile_hold_over_ || now_ < reconcile_hold_until_)
      return;
    reconcile_hold_over_ = true;
    FASTMM_LOG_WARN(
        "a reconciliation has not ended {} ms after it began: its held quotes are pulled",
        kReconcileHold.millis());
    enter_api();
    Placer place{this};
    for (const Instrument& inst : instruments_) {
      if (quotes_.paused(inst.id)) quotes_.pull_quotes(inst, oms_, place, /*keep_desired=*/true);
    }
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
      case ControlCommand::ReduceOnly:
        set_reduce_only(c.hdr.instrument, c.hdr.venue, c.arg != 0);
        break;
      case ControlCommand::PoolHome:
        // The engine's own changes are in the journal as a record, not an input: a replay makes
        // them again.
        if ((c.hdr.flags & EventHeader::kSynthetic) != 0) break;
        enter_api();
        if (apply_home(c.hdr.instrument, pool_home_to(c.arg), pool_home_source(c.arg))) {
          if (pool_home_source(c.arg) == HomeSource::Operator)
            pin_home(c.hdr.instrument, (c.arg & kPoolHomeFree) == 0);
        } else {
          FASTMM_LOG_WARN(
              "pool home of instrument {} to venue {} ignored: not an account of its pool, or the "
              "pool has no [venues.<primary>.pool] table",
              c.hdr.instrument.value,
              pool_home_to(c.arg).value);
        }
        flush_out();
        break;
      case ControlCommand::SetLimits:
        // The payload sits past the ControlMsg prefix; a short record (another producer, an older
        // journal) is ignored rather than read out of bounds.
        if (c.hdr.len >= sizeof(ControlLimitsMsg)) {
          const auto& m = msg_cast<ControlLimitsMsg>(&c.hdr);
          LimitsRecord r{};
          r.before = risk_.limits();
          r.after = m.limits;
          r.check_balance_before = static_cast<std::uint8_t>(balances_->check_enabled());
          r.forced = static_cast<std::uint8_t>((m.arg & kLimitsForced) != 0);
          risk_.set_limits(m.limits, now_);
          risk_band_stale_ = true;
          feed_lag_limit_ = milliseconds(m.limits.max_feed_lag_ms);
          if ((m.arg & kLimitsCheckBalance) != 0)
            balances_->set_check((m.arg & kLimitsCheckBalanceOn) != 0);
          r.check_balance_after = static_cast<std::uint8_t>(balances_->check_enabled());
          for_each_changed_limit(r.before, r.after, [&](std::string_view k, auto from, auto to) {
            FASTMM_LOG_WARN("risk limit replaced by the operator: {} {} -> {}{}",
                            k,
                            from,
                            to,
                            r.forced != 0 ? " (--force)" : "");
          });
          if (r.check_balance_before != r.check_balance_after) {
            FASTMM_LOG_WARN("risk limit replaced by the operator: check_balance {} -> {}{}",
                            r.check_balance_before != 0,
                            r.check_balance_after != 0,
                            r.forced != 0 ? " (--force)" : "");
          }
          emit_limits(r);
        }
        break;
      case ControlCommand::SetUnderlyingLimit: {
        const auto& m = msg_cast<ControlUnderlyingMsg>(&c.hdr);
        const Qty max_net = Qty::from_raw(static_cast<std::int64_t>(m.arg));
        const Qty before = risk_.underlying_limit(m.underlying);
        if (risk_.set_underlying_limit(m.underlying, max_net)) {
          FASTMM_LOG_WARN("risk limit replaced by the operator: underlying.{}.max_net {} -> {}{}",
                          m.name.view(),
                          before,
                          max_net,
                          (m.flags & kLimitsForced) != 0 ? " (--force)" : "");
          LimitsRecord r{};
          r.kind = LimitsRecord::kUnderlying;
          r.before = r.after = risk_.limits();
          r.check_balance_before = r.check_balance_after =
              static_cast<std::uint8_t>(balances_->check_enabled());
          r.underlying_before = before.raw;
          r.underlying_after = max_net.raw;
          r.underlying = m.name;
          r.forced = static_cast<std::uint8_t>((m.flags & kLimitsForced) != 0);
          emit_limits(r);
        } else {
          FASTMM_LOG_WARN("underlying limit for index {} ({}) ignored: not in this session",
                          m.underlying,
                          m.name.view());
        }
        break;
      }
      case ControlCommand::TripKill:
        risk_.trip();
        on_kill(c.arg == kTripKillStoreFailure ? KillReason::StoreFailure : KillReason::Requested,
                c.arg == kTripKillPlannedStop);
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
          kill_ts_ = Timestamp{};
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
      case ControlCommand::TakeOver:
        take_over(Notional::from_raw(static_cast<std::int64_t>(c.arg)));
        break;
      case ControlCommand::Transfer:
        ++stats_.treasury_records;  // a treasury's record for the journal (ControlTransferMsg)
        if (c.hdr.len < sizeof(ControlTransferMsg)) break;
        if ((msg_cast<ControlTransferMsg>(&c.hdr).flags & ControlTransferMsg::kLane) != 0) {
          // The engine's own lane records are in the journal as records: a replay makes them again.
          if ((c.hdr.flags & EventHeader::kSynthetic) == 0)
            on_lane_record(msg_cast<ControlTransferMsg>(&c.hdr));
        } else {
          on_transfer(msg_cast<ControlTransferMsg>(&c.hdr));
        }
        break;
      case ControlCommand::PoolSweep:
        on_pool_sweep(c);
        break;
      case ControlCommand::ResetLossBudget:
        reset_loss_budget();
        break;
    }
  }

  // ControlCommand::Transfer with kHeld (a pool treasury with reserve = true): the payout held out
  // of the payer's balance, by the pool's primary. See ControlTransferMsg.
  FASTMM_NOINLINE void on_transfer(const ControlTransferMsg& m) noexcept {
    if ((m.flags & ControlTransferMsg::kHeld) == 0 || m.hdr.venue.value >= kMaxVenues) return;
    auto& h = transfer_holds_[m.hdr.venue.value];
    const bool mine = h.active && h.seq == m.seq;
    const auto amount = static_cast<std::int64_t>(m.arg);
    const std::string_view asset = m.asset.view();
    switch (static_cast<TreasuryEventKind>(m.event)) {
      case TreasuryEventKind::Reserve:
      case TreasuryEventKind::Sent: {
        if (!mine) {
          release_hold(m.hdr.venue);
          h.active = true;
          h.seq = m.seq;
          h.from = m.from;
          h.asset = m.asset;
          h.amount = 0;
        }
        // At most what the payer has free: a hold never takes what an order holds.
        const std::int64_t free = balances_->free_of(m.from, asset);
        const std::int64_t room =
            free == BalanceBook::kUnreported ? amount : h.amount + std::max<std::int64_t>(free, 0);
        const std::int64_t want =
            std::clamp<std::int64_t>(amount, 0, std::max<std::int64_t>(room, 0));
        if (want != h.amount && balances_->reserve(h.from, h.asset.view(), want - h.amount))
          h.amount = want;
        if (static_cast<TreasuryEventKind>(m.event) == TreasuryEventKind::Sent)
          h.sent = m.hdr.recv_ts;
        break;
      }
      case TreasuryEventKind::Done:
        // The amount left the payer and reached the receiver: the estimate has it until reports
        // stamped after it do. The receiver's credit is stamped at the send, so a report that may
        // already have it does not get it twice.
        {
          const Timestamp sent = mine && h.sent.valid() ? h.sent : m.hdr.recv_ts;
          if (mine) release_hold(m.hdr.venue);
          balances_->transfer(m.from, asset, -amount, m.hdr.recv_ts);
          balances_->transfer(m.to, asset, amount, sent);
        }
        break;
      case TreasuryEventKind::Failed:
      case TreasuryEventKind::Released:
        if (mine) release_hold(m.hdr.venue);
        break;
      case TreasuryEventKind::Planned:
      case TreasuryEventKind::TimedOut:
      case TreasuryEventKind::Request:
        break;
    }
    hold_amt_pub_[m.hdr.venue.value].store(h.active ? h.amount : 0, std::memory_order_relaxed);
    hold_seq_pub_[m.hdr.venue.value].store(h.active ? h.seq : 0, std::memory_order_release);
    publish_live(latency_pub_.load());
  }
  // The pool's hold goes back into the payer's free estimate.
  void release_hold(VenueId primary) noexcept {
    auto& h = transfer_holds_[primary.value];
    if (h.active && h.amount != 0)
      static_cast<void>(balances_->reserve(h.from, h.asset.view(), -h.amount));
    h = {};
  }

  // ControlCommand::ResetLossBudget: net_pnl() is zero again from here.
  FASTMM_NOINLINE void reset_loss_budget() noexcept {
    const Notional before = net_pnl();
    cfg_.pnl_carry = cfg_.pnl_carry - before;
    FASTMM_LOG_INFO("[risk] max_loss: a new period starts; net PnL {} measured from zero again",
                    before);
  }

  // ControlCommand::TakeOver: the loss budget the session before this one left, and its strategy
  // state. Orders stay refused until each venue's first reconciliation (await_reconcile), which
  // the venue starts once the private channels it held back are open.
  FASTMM_NOINLINE void take_over(Notional carry) noexcept {
    cfg_.pnl_carry = carry;
    FASTMM_LOG_WARN("taking over: carried PnL {}", carry);
    if constexpr (KeepsState<Strategy>) {
      std::string staged;
      {
        const std::lock_guard<std::mutex> lock(state_mu_);
        staged.swap(staged_state_);
      }
      if (!staged.empty()) {
        cfg_.initial_state.swap(staged);
        restore_state();
        // As a snapshot, not as Restored: a replay restores at the start, and this one came after
        // the warm-up (backtest/journal_state.hpp starts from it when nothing was restored then).
        if (journal_.enabled()) {
          journal_state(StrategyStateMsg::Kind::Snapshot,
                        cfg_.initial_state,
                        state_restored_ ? StrategyStateMsg::kAccepted : 0,
                        true);
        }
      }
      // What was captured during the warm-up is not this session's to write: the state file gets
      // the state as of now.
      capture_state();
    }
    if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
    publish_live(latency_pub_.load());
  }

  // A state naming an instrument is that book's alone (a per-symbol resync: Binance's depth sync,
  // BookSyncer), and only that book gets a new snapshot: clearing the venue's other books would
  // leave them empty until the channel itself drops.
  void on_connection_state(const ConnectionStateMsg& m) noexcept {
    const VenueId venue = m.hdr.venue;
    const InstrumentId only = m.hdr.instrument;
    const bool one = only.valid();
    // The venue's order link, for the pool routing: a member whose link is not live gets no new
    // orders until it is back.
    if (m.channel == 1 && !one) {
      if (m.state == ConnState::Live) {
        const bool was_down = (order_link_down_ & venue_mask(venue)) != 0;
        order_link_down_ &= ~venue_mask(venue);
        if (FASTMM_UNLIKELY(homes_on_) && was_down) account_back(venue);
      } else {
        order_link_down_ |= venue_mask(venue);
      }
    }
    if (m.state != ConnState::Live) {
      for (const Instrument& inst : instruments_) {
        if (inst.venue != venue || (one && inst.id != only)) continue;
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
        if (reconciling_ == 0) {
          reconcile_hold_until_ = now_ + kReconcileHold;
          reconcile_hold_over_ = false;
        }
        reconciling_ |= venue_mask(m.hdr.venue);
        reconcile_exact_ = (m.flags & ReconcileMsg::kExecutionsExact) != 0;
        if (reconcile_exact_) {
          ++stats_.exact_reconciles;
          emit_replayed(m.hdr.venue);
        } else {
          ++stats_.estimated_reconciles;
          FASTMM_LOG_WARN(
              "reconciling without the venue's executions: filled quantity this snapshot reports "
              "that no fill covered is an estimate, and an order it drops cannot be told from one "
              "that filled");
        }
        // The quotes hold where they rest until End: the snapshot was taken before Begin was
        // sent, so cancelling them here changes nothing it says, and End re-applies the desired
        // quotes to what it leaves, so an order that still matches stays. (Cancelling them sent
        // each one again identically at End, every quote of every account for one account's
        // reconciliation.) An End that does not follow within kReconcileHold pulls them
        // (check_reconcile_hold): quotes the strategy cannot move do not rest for long.
        for (const Instrument& inst : instruments_) quotes_.pause_quotes(inst.id);
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
          set_reported_position(m.hdr.instrument, m.hdr.venue, m.position_qty, m.avg_px);
          emit_position(m.hdr.instrument);  // a session killed before it trades keeps it too
          if (risk_.on_pnl(net_pnl())) on_kill(KillReason::MaxLoss);
        }
        break;
      case ReconcileMsg::Kind::End:
        // Orders the venue no longer has end like any other update, so the quote manager frees
        // their slots too.
        oms_.reconcile_end([&](const OmsUpdate& u) { after_oms_update(u, m.hdr); }, m.hdr.venue);
        reconciling_ &= ~venue_mask(m.hdr.venue);
        if ((awaiting_ & venue_mask(m.hdr.venue)) != 0) {
          awaiting_ &= ~venue_mask(m.hdr.venue);
          if (awaiting_ == 0)
            FASTMM_LOG_INFO(
                "every venue has reconciled its orders and executions since the start; orders "
                "are sent from now on");
        }
        if (reconciling()) break;  // another venue's is still in progress
        resume_quotes();
        break;
    }
  }

  // The strategy is not asked again after a reconciliation (BasicMM requotes on a mid move), so
  // the quotes paused at Begin are re-applied here to the orders resting now. An instrument that
  // cannot quote now (kill switch, quoting disabled, no valid book) is pulled for good instead: its
  // kept quotes are stale.
  void resume_quotes() noexcept {
    Placer place{this};
    const Timestamp now = now_;
    for (const Instrument& inst : instruments_) {
      if (!quotes_.resumable(inst.id)) {
        static_cast<void>(quotes_.resume(inst, oms_, now, place));  // ends the pause only
        continue;
      }
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

  // ControlCommand::ReduceOnly. Turning it on cancels the working orders on the side of each
  // instrument it now holds that takes the position away from zero (both sides when flat); an
  // order still in flight is cancelled at its ack. The strategy reads it with ctx.reduce_only.
  void set_reduce_only(InstrumentId id, VenueId v, bool on) noexcept {
    if (id.valid()) {
      if (!instruments_.contains(id)) return;
      ro_inst_[id.value] = on;
    } else if (v.valid()) {
      ro_venues_ = on ? ro_venues_ | venue_mask(v) : ro_venues_ & ~venue_mask(v);
    } else if (on) {
      ro_session_ = true;
    } else {
      ro_session_ = false;
      ro_venues_ = 0;
      ro_inst_.fill(false);
    }
    refresh_reduce_only();
    ++stats_.reduce_only_switches;
    FASTMM_LOG_INFO("reduce-only {} ({}): {} instruments held",
                    on ? "on" : "off",
                    id.valid()  ? instruments_.get(id).symbol.view()
                    : v.valid() ? venue_name(v)
                                : std::string_view("session"),
                    ro_count_);
    if (on) {
      enter_api();
      for (const Instrument& inst : instruments_) {
        if (!reduce_only_[inst.id.value]) continue;
        StaticVector<Handle<Order>, kMaxOpenOrders> handles;
        oms_.for_each_open_order(inst.id, [&](Handle<Order> h, const Order& o) {
          if (o.is_working() && adds(inst.id, o.side)) static_cast<void>(handles.push_back(h));
        });
        for (Handle<Order> h : handles) static_cast<void>(submit_cancel(h));
      }
      flush_out();
    }
    // At once rather than with the next latency publication: the status shows the switch.
    publish_live(latency_pub_.load());
  }
  void refresh_reduce_only() noexcept {
    ro_count_ = 0;
    for (const Instrument& inst : instruments_) {
      if (inst.id.value >= kMaxInstruments) continue;
      const bool held =
          ro_session_ || (ro_venues_ & venue_mask(inst.venue)) != 0 || ro_inst_[inst.id.value];
      reduce_only_[inst.id.value] = held;
      ro_count_ += held ? 1U : 0U;
    }
  }
  // An order on `side` takes the instrument's position away from zero (either side when flat).
  [[nodiscard]] bool adds(InstrumentId id, Side side) const noexcept {
    const std::int64_t q = positions_.get(id).qty.raw;
    return q == 0 || (q > 0) == (side == Side::Buy);
  }
  FASTMM_NOINLINE void cancel_if_adding(Handle<Order> h) noexcept {
    if (!oms_.is_live(h)) return;
    const Order& o = oms_.get(h);
    if (!o.is_working() || !reduce_only(o.instrument) || !adds(o.instrument, o.side)) return;
    enter_api();
    static_cast<void>(submit_cancel(h));
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
    // With placement = "home" each slice goes to one holder, no more than it holds.
    VenueId account{};
    if (FASTMM_UNLIKELY(homes_on_) && cfg_.homes.of(inst.venue).homes()) {
      for (const VenueId v : cfg_.pools.members(inst.venue)) {
        const Qty held = holding(inst.id, v);
        const Qty dir = Qty::from_raw(side == Side::Sell ? held.raw : -held.raw);
        if (!account_usable(v) || !inst.valid_qty(inst.round_qty(dir))) continue;
        account = v;
        if (dir < qty) qty = dir;
        break;
      }
    }
    qty = inst.round_qty(qty);
    if (!inst.valid_qty(qty)) {
      // Less than one lot (or than the venue's minimum) is left: no order can move it.
      FASTMM_LOG_WARN("flatten leaves {} on {}:{}: below the tradable minimum",
                      p.qty,
                      venue_name(inst.venue),
                      inst.symbol);
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
    r.account = account;
    if (const auto id = submit_new(r, /*flatten=*/true)) {
      flatten_order_[inst.id.value] = *id;
      ++stats_.flatten_orders;
    }
    return true;
  }

  // The global flag is already set. KillReason::Requested: the control thread asked for it
  // (shutdown, operator); any other reason is a risk limit or an internal failure, which is an
  // error (fastmm-live exits or keeps running per [engine] on_kill). The first reason is kept.
  void on_kill(KillReason reason, bool planned_stop = false) noexcept {
    risk_band_stale_ = true;
    ++stats_.kills;
    quoting_enabled_ = false;
    if (kill_reason_ == KillReason::None) {
      kill_reason_ = reason;
      kill_ts_ = now();
    }
    emit_kill(reason, VenueId::invalid(), /*per_venue=*/false);
    if (planned_stop) {
      FASTMM_LOG_INFO("stopping: pulling quotes and cancelling all");
    } else if (reason == KillReason::Requested) {
      FASTMM_LOG_WARN("kill switch requested (flags={:#x}); pulling quotes and cancelling all",
                      risk_.kill_flags());
    } else {
      FASTMM_LOG_ERROR("kill switch engaged ({}, flags={:#x}); pulling quotes and cancelling all",
                       reason,
                       risk_.kill_flags());
    }
    pull_all_quotes();
    mass_cancel();
    // A flatten keeps working the position off through a kill (its reduce-only orders pass the
    // kill switch), but not through the session's own shutdown.
    if (planned_stop && flatten_state_ == FlattenState::Working) end_flatten(FlattenState::Stopped);
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

  // ---- batch end ------------------------------------------------------------------------------

  // A replay feed (JournalFeed) hands the engine one event per step: the batch ends a strategy
  // asked for are the journal's, not the ones its steps would make.
  static constexpr bool kFeedReplaysBatches = requires { requires Feed::kReplaysBatches; };
  // It hands it the timers that fired, journaled (TimerMsg) in the order they did: the wheel is not
  // polled, and each one fires as the journal says (dispatch).
  static constexpr bool kFeedReplaysTimers = requires { requires Feed::kReplaysTimers; };

  // After a step's events and after its timers: the on_batch_end the strategy asked for, journaled
  // like a fired timer.
  void batch_end() noexcept {
    batch_end_hook();
    if (FASTMM_UNLIKELY(checkpoint_wanted_)) requested_checkpoint();
  }
  void batch_end_hook() noexcept {
    if constexpr (has_hook(Hook::BatchEnd) && !kFeedReplaysBatches) {
      if (FASTMM_LIKELY(!batch_end_wanted_)) return;
      latch_clock();
      set_event_origin(batch_t0_, batch_t1_);
      if (journal_.enabled()) {
        TimerMsg t{};
        init_header(t, EventType::Timer);
        t.engine = kBatchEndTimer;
        t.fire_ts = now_;
        t.hdr.flags |= EventHeader::kSynthetic;
        if (!journal_.record_at(t.hdr, now_)) {
          ++stats_.journal_overflows;
          on_journal_overflow();
        }
      }
      // As process() does for the journaled marker a replay reads back.
      const bool quoting_before = quoting_enabled();
      check_param_age();
      check_reconcile_hold();
      check_markouts();
      if (FASTMM_UNLIKELY(quotes_.starved())) retry_starved();
      if (FASTMM_UNLIKELY(quotes_.backoff_due(now_))) retry_backed_off();
      if (FASTMM_UNLIKELY(cluster_on_) && cluster_.lift_due(now_)) lift_fill_cluster();
      if (FASTMM_UNLIKELY(capacity_on_)) capacity_clock();
      call_batch_end();
      after_hooks(quoting_before);
      unlatch_clock();
    }
  }
  FASTMM_NOINLINE void requested_checkpoint() noexcept {
    checkpoint_wanted_ = false;
    latch_clock();
    TimerMsg t{};
    init_header(t, EventType::Timer);
    t.engine = kCheckpointTimer;
    t.fire_ts = now_;
    t.hdr.flags |= EventHeader::kSynthetic;
    if (!journal_.record_at(t.hdr, now_)) {
      ++stats_.journal_overflows;
      on_journal_overflow();
    }
    journal_checkpoint();
    unlatch_clock();
  }
  void call_batch_end() noexcept {
    if constexpr (has_hook(Hook::BatchEnd)) {
      batch_end_wanted_ = false;  // a request from inside the hook: the next batch_end()
      ++stats_.batch_ends;
      strategy_.on_batch_end(ctx_);
      flush_out();
    }
  }

  // ---- timers ---------------------------------------------------------------------------------

  void on_timer_fired(TimerId id, std::uint64_t user_data) noexcept {
    latch_clock();
    set_event_origin(Cycles{}, Cycles{});  // no inbound message: sends carry no T0
    const bool quoting_before = quoting_enabled();
    fire_timer(id, user_data);
    after_hooks(quoting_before);
    unlatch_clock();
  }
  void fire_timer(TimerId id, std::uint64_t user_data) noexcept {
    ++stats_.timers_fired;
    const bool ack_timer = ack_timer_.valid() && id == ack_timer_;
    const bool flatten_timer = flatten_timer_.valid() && id == flatten_timer_;
    const bool state_timer = state_timer_.valid() && id == state_timer_;
    const bool checkpoint_timer = checkpoint_timer_.valid() && id == checkpoint_timer_;
    const bool sweep_timer = sweep_timer_.valid() && id == sweep_timer_;
    const bool engine_timer = ack_timer || flatten_timer || state_timer || checkpoint_timer ||
                              sweep_timer || (param_timer_.valid() && id == param_timer_);
    // Journal a synthetic TimerMsg so replay reproduces the strategy's timer calls.
    if (journal_.enabled()) {
      TimerMsg t{};
      init_header(t, EventType::Timer);
      t.timer_id = id;
      t.engine = ack_timer          ? kAckSweepTimer
                 : flatten_timer    ? kFlattenTimer
                 : state_timer      ? kStateTimer
                 : checkpoint_timer ? kCheckpointTimer
                 : sweep_timer      ? kSweepTimer
                 : engine_timer     ? 1
                                    : 0;
      t.user_data = user_data;
      t.fire_ts = now_;
      t.hdr.flags |= EventHeader::kSynthetic;
      if (!journal_.record_at(t.hdr, now_)) {
        ++stats_.journal_overflows;
        on_journal_overflow();
      }
    }
    check_param_age();
    check_reconcile_hold();
    check_markouts();
    // As process() does for the journaled timer a replay reads back.
    if (FASTMM_UNLIKELY(quotes_.starved())) retry_starved();
    if (FASTMM_UNLIKELY(quotes_.backoff_due(now_))) retry_backed_off();
    if (FASTMM_UNLIKELY(cluster_on_) && cluster_.lift_due(now_)) lift_fill_cluster();
    if (FASTMM_UNLIKELY(capacity_on_)) capacity_clock();
    if (ack_timer) {
      sweep_acks();
    } else if (flatten_timer) {
      flatten_tick();
    } else if (state_timer) {
      if constexpr (KeepsState<Strategy>) capture_state();
    } else if (checkpoint_timer) {
      journal_checkpoint();
    } else if (sweep_timer) {
      sweep_tick();
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

  // Re-applies the quotes of instruments that ran out of order tokens, in the order they ran out,
  // while the bucket has a token for a quote. One that cannot quote now leaves the queue: its next
  // set_quotes starts it again.
  FASTMM_NOINLINE void retry_starved() noexcept {
    const std::int64_t keep = quotes_.params().send_reserve + quotes_.params().token_reserve;
    if (risk_.bucket().available(now_) <= keep) return;
    Placer place{this};
    for (std::size_t n = quotes_.starved_count(); n > 0 && quotes_.starved(); --n) {
      if (risk_.bucket().available(now_) <= keep) break;
      const InstrumentId id = quotes_.pop_starved();
      if (!quoting_enabled(id)) continue;
      quotes_.retry(instruments_.get(id), oms_, now_, place);
    }
    flush_out();
  }

  // Re-applies the quotes of instruments whose side backing off after a venue reject held back a
  // New, once that backoff has ended.
  FASTMM_NOINLINE void retry_backed_off() noexcept {
    Placer place{this};
    quotes_.take_backoff_due(now_, [&](InstrumentId id) {
      if (quoting_enabled(id)) quotes_.retry_backed_off(instruments_.get(id), oms_, now_, place);
    });
    flush_out();
  }

  // ---- the capacity price ([capacity], core/capacity_price.hpp) ---------------------------------

  // What an action passes to submit_new to be priced: its value (a move's: its gain; NaN: worth
  // any price) and whether it is a move.
  struct Purchase {
    double value;
    bool move;
  };

  FASTMM_FORCE_INLINE void capacity_clock() noexcept {
    if (FASTMM_UNLIKELY(capacity_->tick_due(now_.ns))) capacity_tick();
    if (FASTMM_UNLIKELY(quotes_.priced_out()) && capacity_->recheck_due(now_.ns))
      retry_priced_out();
  }

  // Re-applies the quotes of instruments with a level priced out, once a window has rolled over
  // or a fill gave a slot back. One still priced out queues itself again.
  FASTMM_NOINLINE void retry_priced_out() noexcept {
    capacity_->rechecked(now_.ns);
    Placer place{this};
    quotes_.take_priced([&](InstrumentId id) {
      if (quoting_enabled(id)) quotes_.retry_priced(instruments_.get(id), oms_, now_, place);
    });
    flush_out();
  }

  // Once a minute: the yields decay, the day prices are fitted, and each account's minute goes to
  // the record stream; one log line when something was priced out.
  FASTMM_NOINLINE void capacity_tick() noexcept {
    CapacityMinute all{};
    double lo = 0.0;
    double hi = 0.0;
    capacity_->tick(now_.ns, [&](VenueId v, const CapacityMinute& m) {
      if (records_.enabled()) {
        CapacityRecord r;
        records_.init(r, RecordType::Capacity, InstrumentId{}, v, now_, now_);
        r.decided = m.decided;
        r.sent = m.sent;
        r.priced_out_new = m.priced_out_new;
        r.priced_out_move = m.priced_out_move;
        r.observed_new = m.observed_new;
        r.observed_move = m.observed_move;
        r.peak_used = m.peak_used;
        r.mode = capacity_->mode();
        r.price_min = m.price_min;
        r.price_max = m.price_max;
        r.price_mean = m.decided != 0 ? m.price_sum / m.decided : 0.0;
        r.floor = capacity_->floor();
        r.day_price = capacity_->day_price(v);
        account_record(records_.put(r.hdr));
      }
      if (m.decided != 0) {
        lo = all.decided == 0 ? m.price_min : std::min(lo, m.price_min);
        hi = all.decided == 0 ? m.price_max : std::max(hi, m.price_max);
      }
      all.decided += m.decided;
      all.priced_out_new += m.priced_out_new;
      all.priced_out_move += m.priced_out_move;
      all.observed_new += m.observed_new;
      all.observed_move += m.observed_move;
    });
    const std::uint32_t out = all.priced_out_new + all.priced_out_move;
    const std::uint32_t seen = all.observed_new + all.observed_move;
    if (out + seen != 0) {
      FASTMM_LOG_INFO(
          "capacity: {} actions priced in the last minute at {:.3g} to {:.3g}; {} priced out "
          "({} new, {} moves), {} more would be (observed: {} new, {} moves)",
          all.decided,
          lo,
          hi,
          out,
          all.priced_out_new,
          all.priced_out_move,
          seen,
          all.observed_new,
          all.observed_move);
    }
  }

  // The value of a quote level of (id, side) at (px, qty): the strategy's, else the learned one
  // ([capacity] value = "learned"), else NaN (worth any price).
  [[nodiscard]] double level_value(
      const Instrument& inst, Side side, Price px, Qty qty, float stated) const noexcept {
    if (value_stated(stated)) return static_cast<double>(stated);
    if (capacity_->config().value == CapacityValue::Learned)
      return capacity_->learned_value(inst.id, side, inst.notional(px, qty).to_double());
    return std::numeric_limits<double>::quiet_NaN();
  }

  // Prices an action of `value` on `account`: whether it may go. A refusal that is enforced is
  // counted, shown on the side and recorded; one that is only observed is counted.
  [[nodiscard]] bool capacity_admits(const Instrument& inst,
                                     VenueId account,
                                     Side side,
                                     Price px,
                                     Qty qty,
                                     const Purchase& buy,
                                     bool replace) noexcept {
    if (!capacity_->prices(account)) return true;
    const double price = capacity_->price(account, inst.venue, now_.ns);
    const bool enforced = capacity_->enforces(buy.move);
    if (capacity_->decide(account, buy.value, price, buy.move, enforced, inst.id, side, now_.ns) ||
        !enforced)
      return true;
    ++stats_.priced_out;
    note_refusal(inst.id, side, RejectReason::PricedOut);
    if (FASTMM_UNLIKELY(records_.enabled())) {
      std::uint8_t flags = replace ? RejectRecord::kReplace : 0;
      if (reduces(inst.id, side, qty)) flags |= RejectRecord::kReduces;
      record_reject(
          RejectReason::PricedOut, RejectSource::Capacity, inst.id, account, side, px, qty, flags);
    }
    return false;
  }

  // A move of the resting order `o` to (a.price, a.qty): towards the touch it buys its gain, the
  // level's value less what the order is worth where it rests (the keep value; unstated, the value
  // less the part a move of full_move_bps would gain); away from the touch it is bought as a new
  // order. A replace that only lowers the quantity at the same price is free.
  [[nodiscard]] bool capacity_buys_move(const Order& o,
                                        const QuoteAction& a,
                                        bool cancel) noexcept {
    const Instrument& inst = instruments_.get(o.instrument);
    if (!cancel && a.price == o.price && a.qty <= o.leaves_qty()) return true;
    const double value = level_value(inst, o.side, a.price, a.qty, a.value);
    const bool toward = o.side == Side::Buy ? a.price > o.price : a.price < o.price;
    double gain = value;
    if (toward && std::isfinite(value)) {
      double keep = static_cast<double>(a.keep);
      if (!value_stated(a.keep)) {
        const double ref = o.price.to_double();
        const double moved = ref > 0.0 ? std::fabs(a.price.to_double() - ref) / ref * 1e4 : 0.0;
        const double d = capacity_->config().full_move_bps;
        keep = value * (d > 0.0 ? std::max(0.0, 1.0 - (moved / d)) : 0.0);
      }
      gain = value - keep;
    }
    return capacity_admits(inst, o.venue, o.side, a.price, a.qty, Purchase{gain, true}, !cancel);
  }

  // QuoteParams::room_reserve: a resting quote `o` is not moved to `target` when the move goes
  // towards the touch (a bid up, an ask down) and the account it rests on has no more than the
  // reserve left in its venue order windows (ctx.order_budget; paused: none). The resting order is
  // the less aggressive one, so keeping it costs a fill at worst; the order the move would take is
  // left for a level with no order and for moves away from the touch. A venue that publishes no
  // budget (a backtest, a replay) always has room.
  [[nodiscard]] bool holds_move(const Order& o, Price target) const noexcept {
    const std::int64_t reserve = quotes_.params().room_reserve;
    if (reserve < 0) return false;
    if (o.side == Side::Buy ? target <= o.price : target >= o.price) return false;
    const OrderBudget b = order_budget(o.venue);
    if (FASTMM_LIKELY(!b.venue_known)) return false;
    if (b.venue_paused) return true;
    return std::min({b.orders_10s.remaining(), b.orders_1m.remaining(), b.orders_1d.remaining()}) <=
           reserve;
  }

  // Whether the [risk] bucket has a token for a quote that leaves `open_after` working on `side`:
  // every quote leaves QuoteParams::send_reserve for the orders the strategy sends itself, and one
  // that does not take the position towards zero leaves token_reserve on top of it for the orders
  // that do. Cancels never need one.
  [[nodiscard]] bool quote_token(InstrumentId id, Side side, Qty open_after) const noexcept {
    const TokenBucket& b = risk_.bucket();
    if (FASTMM_LIKELY(!b.enabled())) return true;
    const std::int64_t have = b.available(now_) - quotes_.params().send_reserve;
    if (have > quotes_.params().token_reserve) return true;
    if (have <= 0) return false;
    const std::int64_t pos = positions_.get(id).qty.raw;
    if (pos == 0 || (pos > 0) == (side == Side::Buy)) return false;
    return open_after.raw <= (pos < 0 ? -pos : pos);
  }

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
        r.account = a.account;  // a level asked for on one pool account (DesiredQuotes::account)
        // A deferred New whose replaced order filled rather than cancelled: the balance it was
        // decided against is gone. Withheld, not refused; the strategy's next quotes decide.
        if (FASTMM_UNLIKELY(a.deferred && balances_live_) && balances_->check_enabled() &&
            !balance_covers(inst,
                            pools_on_ ? route(r, inst).account : inst.venue,
                            r.side,
                            r.type,
                            r.price,
                            r.qty,
                            0,
                            false,
                            oms_.open_qty(r.instrument, r.side))) {
          a.withheld = true;
          return false;
        }
        if (FASTMM_UNLIKELY(
                !quote_token(r.instrument, r.side, oms_.open_qty(r.instrument, r.side) + r.qty))) {
          a.rate_limited = true;
          return false;
        }
        // A New after a move that was bought is not priced again.
        Purchase buy{};
        const Purchase* priced = nullptr;
        if (FASTMM_UNLIKELY(capacity_on_) && !a.paid) {
          buy = Purchase{level_value(inst, r.side, r.price, r.qty, a.value), false};
          priced = &buy;
        }
        auto res = submit_new(r, false, priced);
        if (!res) {
          a.refused = res.error();
          a.priced_out = res.error() == RejectReason::PricedOut;
          return false;
        }
        a.cl_ord_id = *res;
        a.handle = oms_.find(*res);
        return a.handle.valid();
      }
      case QuoteActionKind::Cancel:
        // A cancel that moves the quote (it carries the target): the New after it needs room, and
        // a move towards the touch is bought here, so its New is not priced again.
        if (a.price.is_positive() && oms_.is_live(a.handle)) {
          const Order& o = oms_.get(a.handle);
          if (holds_move(o, a.price)) {
            a.held = true;
            return false;
          }
          if (FASTMM_UNLIKELY(capacity_on_) &&
              (o.side == Side::Buy ? a.price > o.price : a.price < o.price)) {
            if (!capacity_buys_move(o, a, true)) {
              a.priced_out = true;
              return false;
            }
            a.paid = true;
          }
        }
        return submit_cancel(a.handle).has_value();
      case QuoteActionKind::Replace: {
        if (!oms_.is_live(a.handle)) return false;
        const Order& o = oms_.get(a.handle);
        if (holds_move(o, a.price)) {
          a.held = true;
          return false;
        }
        if (FASTMM_UNLIKELY(capacity_on_) && !capacity_buys_move(o, a, false)) {
          a.priced_out = true;
          return false;
        }
        if (FASTMM_UNLIKELY(
                !quote_token(o.instrument,
                             o.side,
                             oms_.open_qty(o.instrument, o.side) - o.leaves_qty() + a.qty))) {
          a.rate_limited = true;
          return false;
        }
        auto res = submit_replace(a.handle, a.price, a.qty);
        if (!res) a.refused = res.error();
        return res.has_value();
      }
    }
    return false;
  }

  // `flatten`: the engine is working a position off (flatten_instrument). The order passes the
  // global kill switch (RiskInputs::flatten: a killed session can still work its inventory off)
  // and the same checks as any other except two, both expressed as missing RiskInputs: the position
  // cap (a reduce-only slice exists to get under it) and self-trade prevention (the flatten
  // cancelled every order of its scope before the first slice, so the only resting orders it could
  // cross are already cancelled -- and a cancel the venue never acknowledges must not leave the
  // position on). The price collar, the fat-finger band, the size, notional and rate limits all
  // still apply.
  // `buy`: price the order ([capacity]) on the account it goes to; refused PricedOut below it.
  Result<ClientOrderId, RejectReason> submit_new(const NewOrderRequest& req,
                                                 bool flatten = false,
                                                 const Purchase* buy = nullptr) noexcept {
    if (FASTMM_UNLIKELY(!instruments_.contains(req.instrument)))
      return fail(RejectReason::InstrumentDisabled);
    if (FASTMM_UNLIKELY(awaiting_ != 0)) return fail(RejectReason::NotReconciled);
    const Instrument& inst = instruments_.get(req.instrument);
    const Timestamp now = now_;
    // The account it goes from: the instrument's venue, or one of its pool (an explicit account
    // outside the pool is refused; the automatic choice falls back to the primary).
    VenueId account = inst.venue;
    bool window_full = false;
    bool budget_known = false;
    RejectSource window_source = RejectSource::RiskBucket;
    if (FASTMM_UNLIKELY(pools_on_)) {
      const Route rt = route(req, inst, buy);
      if (FASTMM_UNLIKELY(rt.refused != RejectReason::None)) {
        ++stats_.home_unavailable;
        note_refusal(req.instrument, req.side, rt.refused);
        return fail(rt.refused);
      }
      account = rt.account;
      if (!account.valid()) return fail(RejectReason::InvalidAccount);
      if (FASTMM_UNLIKELY(rt.spilled)) ++stats_.home_spills;
      if (FASTMM_UNLIKELY(rt.assigns))
        static_cast<void>(apply_home(req.instrument, account, rt.source));
      // The account's order window is full up to its connector's cap, or the connector is paused
      // (the one named, or the fallback when no account with room covers the order): refused
      // by the risk check with RateLimit, as the connector would refuse it, without reaching it
      // and without taking a [risk] token.
      const OrderBudget b = order_budget(account);
      window_full = no_order_room(b);
      budget_known = b.venue_known;
      if (FASTMM_UNLIKELY(window_full)) window_source = room_refusal(b);
    } else if (FASTMM_UNLIKELY(req.account.valid() && req.account != inst.venue)) {
      return fail(RejectReason::InvalidAccount);
    }
    if (FASTMM_UNLIKELY(buy != nullptr) &&
        !capacity_admits(inst, account, req.side, req.price, req.qty, *buy, false))
      return fail(RejectReason::PricedOut);
    OrderIntent oi{req.instrument, account, req.side, req.type, req.price, req.qty, req.tif};
    RiskInputs in{now,
                  flatten ? nullptr : &positions_.get(req.instrument),
                  positions_.gross_exposure(),
                  positions_.net_exposure(),
                  oms_.open_qty(req.instrument, req.side),
                  oms_.open_count(req.instrument),
                  flatten ? Price{} : oms_.best_own_px(req.instrument, opposite(req.side)),
                  health_.gated(inst.venue, now)};
    if (FASTMM_UNLIKELY(risk_.underlying_on()) && !flatten)
      in.underlying = underlying_inputs(req.instrument, req.side);
    if (FASTMM_UNLIKELY(balances_live_) && balances_->check_enabled())
      in.balance_short = !balance_covers(inst,
                                         account,
                                         req.side,
                                         req.type,
                                         req.price,
                                         req.qty,
                                         0,
                                         req.reduce_only,
                                         in.open_same_side);
    // A member of a killed primary trades nothing either: the primary's kill is the pool's.
    in.primary_killed = account != inst.venue && risk_.venue_killed(inst.venue);
    in.account_full = window_full;
    in.flatten = flatten;
    in.reduce_only = reduce_only_[req.instrument.value];
    const RejectReason rr = risk_.check_new(oi, inst, in);
    if (FASTMM_UNLIKELY(rr != RejectReason::None)) {
      ++stats_.risk_rejects;
      stats_.risk_rejects_by_reason.add(rr);
      log_risk_reject(rr, inst, req.side, req.price, req.qty, false);
      if constexpr (has_hook(Hook::RiskReject)) {
        queue_risk_reject(
            rr, inst, req.side, req.type, req.price, req.qty, ClientOrderId{}, req.user_tag, false);
      }
      note_refusal(req.instrument, req.side, rr);
      if (FASTMM_UNLIKELY(records_.enabled())) {
        RejectSource source = RejectSource::Risk;
        if (rr == RejectReason::RateLimit)
          source = in.account_full ? window_source : RejectSource::RiskBucket;
        std::uint8_t flags = req.reduce_only ? RejectRecord::kReduceOnly : 0;
        if (reduces(req.instrument, req.side, req.qty)) flags |= RejectRecord::kReduces;
        record_reject(rr, source, req.instrument, account, req.side, req.price, req.qty, flags);
      }
      return fail(rr);
    }
    const ClientOrderId id = oms_.next_cl_ord_id();
    if (FASTMM_UNLIKELY(!id.valid())) return fail(on_ids_exhausted());
    NewOrderRequest r = req;
    r.venue = account;
    r.account = account;
    auto h = oms_.submit(r, id, now);
    if (FASTMM_UNLIKELY(!h)) return fail(h.error());
    const std::uint8_t arm = ab_arm_[req.instrument.value];  // submit checked the id
    oms_.get(*h).arm = arm;
    if (FASTMM_UNLIKELY(balances_live_)) {
      const std::int64_t amount = order_hold(oms_.get(*h));
      holds_[h->idx] = amount;
      unacked_[h->idx] = true;
      balances_->move_hold(req.instrument, req.side, 0, amount, Timestamp{}, true, account);
    }
    OutNewOrderMsg m{};
    init_header(m, EventType::OutNewOrder, req.instrument, account);
    m.hdr.recv_ts = now;
    m.cl_ord_id = id;
    m.price = req.price;
    m.qty = req.qty;
    m.side = req.side;
    m.type = req.post_only && req.type == OrderType::Limit ? OrderType::PostOnly : req.type;
    m.tif = req.tif;
    m.reduce_only = req.reduce_only ? 1 : 0;
    m.arm = arm;
    own_outbound(m.hdr);
    queue_out(m.hdr);
    ++stats_.orders_sent;
    note_sent_order(account);
    note_inst_order(req.instrument);
    if (FASTMM_UNLIKELY(capacity_on_)) {
      capacity_->on_send(account,
                         inst.venue,
                         req.instrument,
                         req.side,
                         inst.notional(req.price, req.qty).to_double(),
                         now.ns);
    }
    // The next automatic choice of the pool starts after this account: accounts with the same
    // room take turns rather than the first in pool order taking them all.
    if (FASTMM_UNLIKELY(pools_on_) && budget_known && !req.account.valid())
      pool_turn_[inst.venue.value] = static_cast<std::uint8_t>(pool_index(inst.venue, account) + 1);
    return id;
  }

  // The connector would refuse a new order now: paused, or one of its order windows at the cap
  // its rate limiter refuses at (RateWindow::cap, below the venue's limit), which remaining()
  // already counts against.
  [[nodiscard]] static bool no_order_room(const OrderBudget& b) noexcept {
    return b.venue_paused || b.orders_10s.remaining() == 0 || b.orders_1m.remaining() == 0 ||
           b.orders_1d.remaining() == 0;
  }

  // One more new order or replace on its way to `v`'s connector (order_budget).
  FASTMM_FORCE_INLINE void note_sent_order(VenueId v) noexcept {
    if (FASTMM_LIKELY(v.value < kMaxVenues)) ++sent_orders_[v.value];
  }
  // `account`'s place in the pool of `primary`.
  [[nodiscard]] std::size_t pool_index(VenueId primary, VenueId account) const noexcept {
    const PoolMembers members = cfg_.pools.members(primary);
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (members[i] == account) return i;
    }
    return 0;
  }

  // [risk.underlying]: the position and same-side open orders of the order's underlying, over its
  // instruments. Out of line: without a limit the order path does not reach it.
  FASTMM_NOINLINE UnderlyingInputs underlying_inputs(InstrumentId id, Side side) const noexcept {
    return risk_.underlying_inputs(
        id,
        instruments_,
        now_,
        [this](InstrumentId j) { return positions_.get(j).qty; },
        [this, side](InstrumentId j) { return oms_.open_qty(j, side); });
  }

  // ---- homes (core/pool_homes.hpp) -------------------------------------------------------------

  // Where a new order goes: an account and, with placement = "home", the reason it goes nowhere
  // (HomeUnavailable) and the first home or spill it makes.
  struct Route {
    VenueId account{};
    RejectReason refused = RejectReason::None;
    bool assigns = false;  // the order gives the instrument this home (source)
    HomeSource source = HomeSource::First;
    bool spilled = false;  // spill = "busy": the home could not take it
  };
  // `buy`: the order's price for [capacity] (spill = "busy" spills an order its home prices out).
  [[nodiscard]] Route route(const NewOrderRequest& req,
                            const Instrument& inst,
                            const Purchase* buy = nullptr) const noexcept {
    // A sweeping pool keeps the base on the home: a spot sell goes there, a piece is swept.
    if (FASTMM_UNLIKELY(lanes_on_) && !req.account.valid() && req.side == Side::Sell &&
        sweeping(inst))
      return Route{home_[inst.id.value]};
    if (FASTMM_LIKELY(!homes_on_) || req.account.valid() || !cfg_.homes.of(inst.venue).homes() ||
        !cfg_.pools.pooled(inst.venue))
      return Route{choose_account(req, inst)};
    // A holder this order takes towards zero, a piece first; else the home.
    const VenueId holder = reduce_route(inst, req.side, req.price, req.qty, /*sizing=*/false);
    if (holder.valid()) return Route{holder};
    if (!inst.is_derivative() && req.side == Side::Sell) return Route{sell_fallback(inst)};
    const VenueId h = home_[inst.id.value];
    if (!h.valid()) {
      const VenueId first = first_home(inst);
      if (!first.valid()) return Route{{}, RejectReason::HomeUnavailable};
      return Route{first, RejectReason::None, true, HomeSource::First};
    }
    if (spills_busy(inst)) {
      if (home_takes(req, inst, h, buy)) return Route{h};
      const VenueId to = pick_account(req, inst);
      if (to.valid() && to != h) {
        Route r{to};
        r.spilled = true;
        return r;
      }
      return account_usable(h) ? Route{h} : Route{h, RejectReason::HomeUnavailable};
    }
    if (account_usable(h)) return Route{h};
    if (spills(inst)) {
      const VenueId to = roomiest_account(inst.venue);
      if (to.valid()) return Route{to, RejectReason::None, true, HomeSource::Spill};
    }
    return Route{h, RejectReason::HomeUnavailable};
  }
  // This instrument moves off an unusable home: spill = "flat" (or "busy" for a derivative), flat
  // over the pool and not pinned.
  [[nodiscard]] bool spills(const Instrument& inst) const noexcept {
    const HomeSpill sp = cfg_.homes.of(inst.venue).spill;
    return (sp == HomeSpill::Flat || (sp == HomeSpill::Busy && inst.is_derivative())) &&
           positions_.get(inst.id).qty.is_zero() && !pinned_[inst.id.value];
  }
  // spill = "busy" on a spot instrument: an adding order its home cannot take goes elsewhere.
  [[nodiscard]] bool spills_busy(const Instrument& inst) const noexcept {
    return cfg_.homes.of(inst.venue).spill == HomeSpill::Busy && !inst.is_derivative();
  }
  // spill = "busy": the home takes this adding order now. It is usable, its windows have room and
  // its 10 s window is below spill_at of what its connector admits, its balance covers the order,
  // and [capacity] does not price its slot above the order's value.
  [[nodiscard]] bool home_takes(const NewOrderRequest& req,
                                const Instrument& inst,
                                VenueId h,
                                const Purchase* buy) const noexcept {
    if (!account_usable(h)) return false;
    const OrderBudget b = order_budget(h);
    if (no_order_room(b)) return false;
    if (b.orders_10s.known() &&
        static_cast<double>(b.orders_10s.used) >=
            cfg_.homes.of(inst.venue).spill_at * static_cast<double>(b.orders_10s.admits()))
      return false;
    if (balances_live_ && !balance_covers(inst,
                                          h,
                                          req.side,
                                          req.type,
                                          req.price,
                                          req.qty,
                                          0,
                                          req.reduce_only,
                                          oms_.open_qty(req.instrument, req.side)))
      return false;
    if (buy != nullptr && capacity_on_ && capacity_->prices(h)) {
      const double value =
          buy->value != buy->value ? std::numeric_limits<double>::infinity() : buy->value;
      if (value < capacity_->price(h, inst.venue, now_.ns)) return false;
    }
    return true;
  }
  // Where the next adding order goes for balance_room: the home, a first home, or the spill's
  // account; invalid when none would take it.
  [[nodiscard]] VenueId add_target(const Instrument& inst) const noexcept {
    const VenueId h = home_[inst.id.value];
    if (!h.valid()) return first_home(inst);
    if (account_usable(h)) return h;
    return spills(inst) ? roomiest_account(inst.venue) : VenueId{};
  }
  // A spot sell no holder takes: an account already carrying a sell, else the home (whose
  // balance check refuses it), else the primary.
  [[nodiscard]] VenueId sell_fallback(const Instrument& inst) const noexcept {
    VenueId carrier{};
    oms_.for_each_open_order(inst.id, [&](Handle<Order>, const Order& o) {
      if (o.side == Side::Sell && !carrier.valid()) carrier = o.venue;
    });
    if (carrier.valid()) return carrier;
    return home_[inst.id.value].valid() ? home_[inst.id.value] : inst.venue;
  }
  // At least a lot, min_qty and (at `px`, when known) min_notional of the instrument.
  [[nodiscard]] static bool tradable(const Instrument& inst, Qty q, Price px) noexcept {
    const std::int64_t a = q.raw < 0 ? -q.raw : q.raw;
    if (a <= 0 || a < inst.min_qty.raw || a < inst.lot.raw) return false;
    return !inst.min_notional.is_positive() || !px.is_positive() ||
           inst.notional(px, Qty::from_raw(a)) >= inst.min_notional;
  }
  // The account an order on `side` of `qty` takes towards zero: of the usable accounts whose own
  // part of the position that side reduces (a tradable amount), the pieces in pool order, then
  // the home. At most reduce_accounts of them carry orders on the side at once, except that a
  // piece may join while only the home carries one. A piece takes an order only if what its open
  // orders on the side leave of its part covers all of it, so a piece never goes through zero.
  // `sizing` (balance_room): the first account allowed, whatever it has left. Invalid: no holder,
  // or a derivative order that no piece takes whole.
  [[nodiscard]] VenueId reduce_route(
      const Instrument& inst, Side side, Price px, Qty qty, bool sizing) const noexcept {
    const InstrumentId id = inst.id;
    const PoolMembers m = cfg_.pools.members(inst.venue);
    const std::size_t n = m.size();
    const VenueId h = home_[id.value];
    std::array<std::int64_t, PoolMembers::kMax> open_on{};
    std::uint32_t carrying = 0;
    oms_.for_each_open_order(id, [&](Handle<Order>, const Order& o) {
      if (o.side != side) return;
      const std::size_t k = pool_index(inst.venue, o.venue);
      open_on[k] += o.leaves_qty().raw;
      carrying |= 1U << k;
    });
    const auto carriers = static_cast<std::uint32_t>(std::popcount(carrying));
    const std::uint32_t limit = cfg_.homes.of(inst.venue).reduce_accounts;
    const std::uint32_t home_bit = h.valid() ? 1U << pool_index(inst.venue, h) : 0;
    const bool only_home_carries = carrying != 0 && carrying == home_bit;
    VenueId first{};
    for (int pass = 0; pass < 2; ++pass) {
      for (std::size_t k = 0; k < n; ++k) {
        const VenueId v = m[k];
        if ((pass == 0) == (v == h) || !account_usable(v)) continue;
        const Qty held = holding(id, v);
        const std::int64_t dir = side == Side::Sell ? held.raw : -held.raw;
        if (dir <= 0 || !tradable(inst, Qty::from_raw(dir), px)) continue;
        const bool allowed =
            (carrying & (1U << k)) != 0 || carriers < limit || (pass == 0 && only_home_carries);
        if (!allowed) continue;
        if (sizing) return v;
        if (!first.valid()) first = v;
        if (pass == 0 && dir - open_on[k] < qty.raw) continue;
        return v;
      }
    }
    // A derivative order no piece takes whole is the home's, adding or not (route).
    return inst.is_derivative() ? VenueId{} : first;
  }
  // The home an instrument without one takes at its first adding order: the usable account that
  // holds the most of it, else the usable account at its id's turn round the pool.
  [[nodiscard]] VenueId first_home(const Instrument& inst) const noexcept {
    const PoolMembers m = cfg_.pools.members(inst.venue);
    const Price mid = books_[inst.id.value].mid();
    VenueId best{};
    std::int64_t most = 0;
    for (const VenueId v : m) {
      if (!account_usable(v)) continue;
      const Qty q = holding(inst.id, v);
      const std::int64_t a = q.raw < 0 ? -q.raw : q.raw;
      if (a > most && tradable(inst, q, mid)) {
        best = v;
        most = a;
      }
    }
    if (best.valid()) return best;
    const std::size_t n = m.size();
    for (std::size_t k = 0; k < n; ++k) {
      const VenueId v = m[(inst.id.value + k) % n];
      if (account_usable(v)) return v;
    }
    return {};
  }
  // The usable account of the pool whose connector would take an order now with the most of its
  // 10 s window left (pool order on a tie).
  [[nodiscard]] VenueId roomiest_account(VenueId primary) const noexcept {
    VenueId best{};
    std::int64_t left = -1;
    for (const VenueId v : cfg_.pools.members(primary)) {
      if (!account_usable(v)) continue;
      const OrderBudget b = order_budget(v);
      if (no_order_room(b)) continue;
      if (b.orders_10s.remaining() > left) {
        best = v;
        left = b.orders_10s.remaining();
      }
    }
    return best;
  }
  // The instrument's home becomes `to`. The engine's own changes are journaled here; the planner's
  // and the operator's reached it as input and are journaled with it. With placement = "home" a
  // change by the operator, the planner, the strategy or the reconciliation cancels the adding
  // orders working on other accounts (a first home has none, a spill's old home cannot be
  // reached).
  bool apply_home(InstrumentId id, VenueId to, HomeSource source) noexcept {
    if (!homes_on_ || !instruments_.contains(id)) return false;
    const Instrument& inst = instruments_.get(id);
    const PoolPlacement& pp = cfg_.homes.of(inst.venue);
    if (!pp.configured || !cfg_.pools.pooled(inst.venue) || !cfg_.pools.admits(inst.venue, to))
      return false;
    const VenueId from = home_[id.value];
    if (from == to) return true;
    home_[id.value] = to;
    home_pub_[id.value].store(to.value, std::memory_order_relaxed);
    ++stats_.home_changes;
    FASTMM_LOG_INFO("{}:{} home {} -> {} ({})",
                    venue_name(inst.venue),
                    inst.symbol,
                    from.valid() ? venue_name(from) : std::string_view("none"),
                    venue_name(to),
                    to_string(source));
    if (source != HomeSource::Operator && source != HomeSource::Planner && journal_.enabled()) {
      ControlMsg m{};
      init_header(m, EventType::Control, id, inst.venue);
      m.hdr.flags |= EventHeader::kSynthetic;
      m.hdr.recv_ts = now_;
      m.command = ControlCommand::PoolHome;
      m.arg = pool_home_arg(to, source, from);
      if (!journal_.record(m.hdr)) ++stats_.journal_overflows;
    }
    if (pp.homes() && source != HomeSource::First && source != HomeSource::Spill)
      cancel_adding_off_home(id);
    return true;
  }
  // The adding orders of the instrument working on an account other than its home: cancelled.
  void cancel_adding_off_home(InstrumentId id) noexcept {
    const Instrument& inst = instruments_.get(id);
    const VenueId h = home_[id.value];
    StaticVector<Handle<Order>, kMaxOpenOrders> handles;
    oms_.for_each_open_order(id, [&](Handle<Order> hd, const Order& o) {
      if (!o.is_working() || o.venue == h) return;
      const bool adding = inst.is_derivative() ? !o.has(Order::kReduceOnly) && adds(id, o.side)
                                               : o.side == Side::Buy;
      if (adding) static_cast<void>(handles.push_back(hd));
    });
    for (const Handle<Order> hd : handles) static_cast<void>(submit_cancel(hd));
  }
  void pin_home(InstrumentId id, bool on) noexcept {
    if (id.value >= kMaxInstruments) return;
    pinned_[id.value] = on;
    pinned_pub_[id.value].store(on, std::memory_order_relaxed);
  }
  // An account's order link is back: with placement = "home", the adding orders resting on it
  // for instruments it is not home to (a spill moved them off while it was down) are cancelled.
  FASTMM_NOINLINE void account_back(VenueId v) noexcept {
    const VenueId primary = cfg_.pools.primary(v);
    // spill = "busy" moved no home: the orders it sent elsewhere stay.
    if (!cfg_.homes.of(primary).homes() || cfg_.homes.of(primary).spill == HomeSpill::Busy) return;
    enter_api();
    for (const Instrument& inst : instruments_) {
      if (inst.venue == primary && inst.id.value < kMaxInstruments &&
          home_[inst.id.value].valid() && home_[inst.id.value] != v)
        cancel_adding_off_home(inst.id);
    }
    flush_out();
  }
  // [venues.<primary>.pool] homes, at the start.
  void apply_pins() noexcept {
    for (const Instrument& inst : instruments_) {
      if (inst.id.value >= kMaxInstruments) continue;
      const VenueId pin = cfg_.homes.pin[inst.id.value];
      if (pin.valid() && apply_home(inst.id, pin, HomeSource::Pin)) pin_home(inst.id, true);
    }
  }
  // A venue ended a balance snapshot. Once every account of a pool has, its homes are checked
  // against what each account holds, once: an instrument held on one account only gets that
  // account, one held on several keeps its home (else the largest holder becomes it) and the
  // others are its pieces; a pinned instrument keeps its pin. With placement = "home" the adding
  // orders working off the home are cancelled.
  FASTMM_NOINLINE void note_snapshot(VenueId v) noexcept {
    snapshot_seen_ |= venue_mask(v);
    const VenueId primary = cfg_.pools.primary(v);
    if (!cfg_.homes.of(primary).configured || (homes_reconciled_ & venue_mask(primary)) != 0)
      return;
    for (const VenueId a : cfg_.pools.members(primary)) {
      if ((snapshot_seen_ & venue_mask(a)) == 0) return;
    }
    homes_reconciled_ |= venue_mask(primary);
    enter_api();
    for (const Instrument& inst : instruments_) {
      if (inst.venue != primary || inst.id.value >= kMaxInstruments) continue;
      const InstrumentId id = inst.id;
      const Price mid = books_[id.value].mid();
      VenueId only{};
      VenueId largest{};
      std::int64_t most = 0;
      std::uint32_t holders = 0;
      for (const VenueId a : cfg_.pools.members(primary)) {
        const Qty q = holding(id, a);
        if (!tradable(inst, q, mid)) continue;
        ++holders;
        only = a;
        const std::int64_t abs = q.raw < 0 ? -q.raw : q.raw;
        if (abs > most) {
          largest = a;
          most = abs;
        }
      }
      VenueId to = home_[id.value];
      if (pinned_[id.value]) {
        // a pin stays: the other holders are its pieces
      } else if (holders == 1 && account_usable(only)) {
        to = only;
      } else if (holders > 1 && !to.valid() && account_usable(largest)) {
        to = largest;
      }
      if (to.valid()) static_cast<void>(apply_home(id, to, HomeSource::Reconcile));
      if (cfg_.homes.of(primary).homes() && home_[id.value].valid()) cancel_adding_off_home(id);
    }
    flush_out();
  }
  // An account's fill moved its part of the position (held_).
  FASTMM_FORCE_INLINE void note_held(InstrumentId id,
                                     VenueId account,
                                     Side side,
                                     Qty qty) noexcept {
    if (FASTMM_LIKELY(held_ == nullptr) || !instruments_.contains(id)) return;
    const VenueId primary = instruments_.get(id).venue;
    if (!cfg_.pools.pooled(primary)) return;
    Qty& h = (*held_)[id.value][pool_index(primary, account)];
    h = side == Side::Buy ? h + qty : h - qty;
  }
  // One more new order or replace of the instrument (pool_load).
  FASTMM_FORCE_INLINE void note_inst_order(InstrumentId id) noexcept {
    if (FASTMM_LIKELY(!homes_on_) || id.value >= kMaxInstruments) return;
    std::atomic<std::uint64_t>& c = inst_orders_[id.value];
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }
  // What pool_load reads of the accounts: each one's 10 s order limit and whether it is usable.
  void publish_pool() noexcept {
    if (!homes_on_) return;
    std::uint32_t unusable = 0;
    for (std::uint32_t v = 0; v < kMaxVenues; ++v) {
      const VenueId id{static_cast<std::uint8_t>(v)};
      if (!account_usable(id)) unusable |= venue_mask(id);
      const OrderBudget b = order_budget(id);
      limit_pub_[v].store(b.venue_known ? b.orders_10s.limit : 0, std::memory_order_relaxed);
    }
    unusable_pub_.store(unusable, std::memory_order_relaxed);
  }

  // ---- transfer lanes (core/transfer_lane.hpp) ------------------------------------------------

  // The sweep, every kSweepPeriod: late transfers, the breaker, then each sweeping pool's pieces.
  FASTMM_NOINLINE void sweep_tick() noexcept {
    std::uint32_t live = 0;
    for (std::size_t v = 0; v < kMaxVenues; ++v) {
      TransferLane* lane = lanes_[v].get();
      if (lane == nullptr) continue;
      note_late(*lane);
      const BreakerState b = lane->breaker(now_);
      const bool open = b == BreakerState::Closed && !lane->day_spent(now_);
      if (open && !lane->config().dry_run) live |= 1U << v;
      if (!open || !balances_live_ || risk_.killed() || risk_.venue_killed(lane->primary()))
        continue;
      sweep_pool(*lane);
    }
    set_lanes_live(live);
  }
  void set_lanes_live(std::uint32_t live) noexcept {
    lanes_live_ = live;
    lanes_live_pub_.store(live, std::memory_order_relaxed);
  }
  // The pool's lane sweeps for this instrument: a spot instrument whose base the venue transfers,
  // with a usable home.
  [[nodiscard]] bool sweeping(const Instrument& inst) const noexcept {
    if (FASTMM_LIKELY(!lanes_on_) || inst.is_derivative() || inst.venue.value >= kMaxVenues ||
        (lanes_live_ & (1U << inst.venue.value)) == 0)
      return false;
    const VenueId h = home_[inst.id.value];
    return h.valid() && account_usable(h) && !lanes_[inst.venue.value]->blocked(inst.base.view());
  }
  void note_late(TransferLane& lane) noexcept {
    for (TransferLane::Slot& t : lane.slots()) {
      if (!t.active || t.late || now_ - t.created < lane.config().timeout) continue;
      t.late = true;
      ++lane.stats().late;
      FASTMM_LOG_WARN("transfer {} of {} {} -> {} unresolved after {} s: still held",
                      t.seq,
                      t.asset.view(),
                      venue_name(t.from),
                      venue_name(t.to),
                      (now_ - t.created).ns / 1'000'000'000);
    }
  }
  // One pool: its pieces due a transfer, the largest first within the budget.
  void sweep_pool(TransferLane& lane) noexcept {
    const VenueId primary = lane.primary();
    const LaneConfig& lc = lane.config();
    const PoolMembers members = cfg_.pools.members(primary);
    std::array<SweepCandidate, 64> cands{};
    std::size_t n = 0;
    std::array<FixedString<8>, kMaxInstruments> seen{};
    std::size_t n_seen = 0;
    for (const Instrument& inst : instruments_) {
      if (inst.venue != primary || inst.is_derivative() || inst.id.value >= kMaxInstruments)
        continue;
      const InstrumentId id = inst.id;
      // One instrument per base asset: a second one with the same base shares the first's sweep.
      bool dup = false;
      for (std::size_t k = 0; k < n_seen; ++k)
        dup = dup || same_currency(seen[k].view(), inst.base.view());
      if (dup) continue;
      seen[n_seen++] = inst.base;
      const VenueId h = home_[id.value];
      const bool now_asked = sweep_now_[id.value];
      sweep_now_[id.value] = false;
      if (!h.valid() || !account_usable(h) || lane.blocked(inst.base.view())) continue;
      const Price mid = books_[id.value].mid();
      const bool home_sells = tradable(inst, holding(id, h), mid);
      for (std::size_t k = 0; k < members.size(); ++k) {
        const VenueId from = members[k];
        if (from == h) continue;
        SweepCell& cell = (*sweep_cells_)[id.value][k];
        const std::int64_t total = balances_->base_total(id, from);
        if (total == BalanceBook::kUnreported) continue;
        if (total != cell.total) {
          cell.total = total;
          cell.changed = now_;
        }
        const std::int64_t free = balances_->free_of(from, inst.base.view());
        if (free == BalanceBook::kUnreported || free <= 0 || now_ < cell.retry) continue;
        if (lc.dry_run && cell.planned == total) continue;
        const Duration age = now_ - cell.changed;
        const bool dust = !tradable(inst, Qty::from_raw(free), mid);
        const bool due =
            now_asked || (dust ? age >= lc.sweep_dust || (home_sells && age >= lc.sweep_delay)
                               : age >= lc.sweep_delay);
        if (!due || n == cands.size()) continue;
        SweepCandidate& c = cands[n++];
        c.instrument = id;
        c.from = from;
        c.to = h;
        c.asset = inst.base;
        c.amount = free;
        c.value = mid.is_positive() ? inst.notional(mid, Qty::from_raw(free)).to_double() : 0.0;
        c.dust = dust;
      }
    }
    if (n == 0) return;
    const std::size_t room = lane.room(now_);
    const std::size_t take = plan_sweeps(std::span<SweepCandidate>(cands.data(), n),
                                         room,
                                         [&](std::string_view a) { return lane.busy(a); });
    for (std::size_t i = 0; i < take; ++i) request_transfer(lane, cands[i]);
  }
  // A transfer of the candidate: held out of the payer's free estimate, journaled and handed to the
  // worker (dry run: journaled as planned, nothing held or sent).
  void request_transfer(TransferLane& lane, const SweepCandidate& c) noexcept {
    const std::uint64_t seq = lane.next_seq(lane_start_);
    lane.count(now_);
    const std::size_t k = pool_index(lane.primary(), c.from);
    SweepCell& cell = (*sweep_cells_)[c.instrument.value][k];
    const auto value = static_cast<std::int64_t>(c.value * static_cast<double>(kFixedScale));
    if (lane.config().dry_run) {
      cell.planned = cell.total;
      ++lane.stats().planned;
      journal_lane(
          lane.primary(), TreasuryEventKind::Planned, seq, c.from, c.to, c.asset, c.amount);
      return;
    }
    TransferLane::Slot* t = lane.open(seq);
    if (t == nullptr) return;  // room() counted the slots
    t->instrument = c.instrument;
    t->from = c.from;
    t->to = c.to;
    t->asset = c.asset;
    t->amount = c.amount;
    t->value = value;
    t->created = now_;
    t->phase = TransferLane::Phase::Requested;
    hold_lane(*t, c.amount);
    ++lane.stats().requested;
    journal_lane(lane.primary(), TreasuryEventKind::Request, seq, c.from, c.to, c.asset, c.amount);
    LaneIntent in;
    in.kind = LaneIntent::Kind::Send;
    in.primary = lane.primary();
    in.from = c.from;
    in.to = c.to;
    in.asset = c.asset;
    in.amount = c.amount;
    in.seq = seq;
    in.created_ns = now_.ns;
    if (!lane_out_->try_push(in)) ++lane.stats().worker_full;
    FASTMM_LOG_INFO("transfer {} {} {} {} -> {} ({})",
                    seq,
                    Notional::from_raw(c.amount),
                    c.asset.view(),
                    venue_name(c.from),
                    venue_name(c.to),
                    c.dust ? "dust" : "piece");
  }
  // The payer's hold: at most its free estimate.
  void hold_lane(TransferLane::Slot& t, std::int64_t amount) noexcept {
    const std::int64_t free = balances_->free_of(t.from, t.asset.view());
    const std::int64_t want =
        free == BalanceBook::kUnreported ? amount : std::clamp<std::int64_t>(amount, 0, free);
    if (want > 0 && balances_->reserve(t.from, t.asset.view(), want)) {
      t.hold = want;
      t.held = true;
    }
  }
  void release_lane(TransferLane::Slot& t) noexcept {
    if (t.held && t.hold != 0)
      static_cast<void>(balances_->reserve(t.from, t.asset.view(), -t.hold));
    t.held = false;
    t.hold = 0;
  }
  // The engine's own lane record, for the journal (kSynthetic: a replay makes it again).
  void journal_lane(VenueId primary,
                    TreasuryEventKind kind,
                    std::uint64_t seq,
                    VenueId from,
                    VenueId to,
                    const FixedString<8>& asset,
                    std::int64_t amount,
                    TransferState state = TransferState::Unknown) noexcept {
    if (!journal_.enabled()) return;
    ControlTransferMsg m{};
    init_header(m, EventType::Control, InstrumentId::invalid(), primary);
    m.hdr.flags |= EventHeader::kSynthetic;
    m.hdr.recv_ts = now_;
    m.command = ControlCommand::Transfer;
    m.event = static_cast<std::uint8_t>(kind);
    m.state = static_cast<std::uint8_t>(state);
    m.from = from;
    m.to = to;
    m.flags = ControlTransferMsg::kLane | ControlTransferMsg::kHeld;
    m.arg = static_cast<std::uint64_t>(amount);
    m.asset = asset;
    m.seq = seq;
    lane_client_id(m.client_id, primary, seq);
    if (!journal_.record(m.hdr)) ++stats_.journal_overflows;
  }
  // The worker's answer about a lane's transfer (an input).
  FASTMM_NOINLINE void on_lane_record(const ControlTransferMsg& m) noexcept {
    const VenueId primary = m.hdr.venue;
    if (primary.value >= kMaxVenues || lanes_[primary.value] == nullptr) return;
    TransferLane& lane = *lanes_[primary.value];
    const auto kind = static_cast<TreasuryEventKind>(m.event);
    TransferLane::Slot* t = lane.find(m.seq);
    if (kind == TreasuryEventKind::Reserve) {
      // A transfer an earlier process left in flight: held again until it resolves.
      if (t != nullptr || lane.by_asset(m.asset.view()) != nullptr) return;
      t = lane.open(m.seq);
      if (t == nullptr) return;
      t->from = m.from;
      t->to = m.to;
      t->asset = m.asset;
      t->amount = static_cast<std::int64_t>(m.arg);
      t->created = m.hdr.recv_ts;
      t->phase = TransferLane::Phase::Unknown;
      for (const Instrument& inst : instruments_) {
        if (inst.venue == primary && same_currency(inst.base.view(), m.asset.view()))
          t->instrument = inst.id;
      }
      hold_lane(*t, t->amount);
      FASTMM_LOG_WARN("transfer {} of an earlier process held again: {} {} {} -> {}",
                      m.seq,
                      Notional::from_raw(t->amount),
                      m.asset.view(),
                      venue_name(m.from),
                      venue_name(m.to));
      return;
    }
    if (t == nullptr) return;  // resolved already (flows), or not this session's
    const auto state = static_cast<TransferState>(m.state);
    switch (kind) {
      case TreasuryEventKind::Sent:
        if (state == TransferState::Done) {
          finish_lane(lane, *t, m.hdr.recv_ts, false);
        } else if (state == TransferState::Unknown) {
          t->phase = TransferLane::Phase::Unknown;
          ++lane.stats().unknown;
          if (lane.note_unknown(now_)) on_lane_trip(lane);
        } else {
          t->phase = TransferLane::Phase::Sent;
          t->sent = m.hdr.recv_ts;
          ++lane.stats().sent;
        }
        break;
      case TreasuryEventKind::Done:
        finish_lane(lane, *t, m.hdr.recv_ts, false);
        break;
      case TreasuryEventKind::Failed: {
        ++lane.stats().failed;
        release_lane(*t);
        if (t->instrument.valid() && t->instrument.value < kMaxInstruments) {
          (*sweep_cells_)[t->instrument.value][pool_index(primary, t->from)].retry =
              now_ + lane.config().sweep_retry;
        }
        const bool rate = m.http_status == 429 || m.code == -1003;
        // Binance -9000: the payer has less free than asked, with the engine's hold in place.
        if (lane.note_asset_refusal(t->asset.view(), m.code == -9000)) {
          FASTMM_LOG_ERROR(
              "transfers of {} refused {} times in a row for want of balance: the "
              "pool leaves its pieces to trade off for the session",
              t->asset.view(),
              TransferLane::kAssetStrikes);
        }
        FASTMM_LOG_WARN("transfer {} {} {} -> {} refused (code {}, HTTP {})",
                        t->seq,
                        t->asset.view(),
                        venue_name(t->from),
                        venue_name(t->to),
                        m.code,
                        m.http_status);
        lane.close(*t);
        if (lane.note_failure(now_, rate)) on_lane_trip(lane);
        break;
      }
      default:
        break;
    }
  }
  void on_lane_trip(TransferLane& lane) noexcept {
    const BreakerState b = lane.breaker(now_);
    if (b == BreakerState::Off) {
      FASTMM_LOG_ERROR(
          "{}: the sweep's breaker tripped again: off until fastmm-ctl pool sweep on; "
          "pieces trade off on their accounts",
          venue_name(lane.primary()));
    } else {
      FASTMM_LOG_ERROR(
          "{}: the sweep's breaker tripped: paused for {} s; pieces trade off on their "
          "accounts meanwhile",
          venue_name(lane.primary()),
          lane.config().breaker.ns / 1'000'000'000);
    }
    set_lanes_live(lanes_live_ & ~(1U << lane.primary().value));
  }
  // The transfer moved: what the flows did not apply yet moves the estimates now. The payer's at
  // `at`, the receiver's at the send (a report that already has it does not get it twice).
  void finish_lane(TransferLane& lane,
                   TransferLane::Slot& t,
                   Timestamp at,
                   bool by_flows) noexcept {
    if (!t.out_seen) {
      release_lane(t);
      balances_->transfer(t.from, t.asset.view(), -t.amount, at);
    }
    if (!t.in_seen) {
      const Timestamp credit = t.sent.valid() ? t.sent : (t.created.valid() ? t.created : at);
      balances_->transfer(t.to, t.asset.view(), t.amount, credit);
    }
    ++lane.stats().done;
    lane.stats().moved_value += t.value;
    lane.note_asset_done(t.asset.view());
    if (by_flows) {
      ++lane.stats().done_by_flows;
      journal_lane(lane.primary(),
                   TreasuryEventKind::Done,
                   t.seq,
                   t.from,
                   t.to,
                   t.asset,
                   t.amount,
                   TransferState::Done);
      LaneIntent in;
      in.kind = LaneIntent::Kind::Resolved;
      in.primary = lane.primary();
      in.seq = t.seq;
      if (!lane_out_->try_push(in)) ++lane.stats().worker_full;
    }
    lane.close(t);
  }
  // A flow of money on an account: the out or in leg of a lane's transfer of that asset and amount.
  FASTMM_NOINLINE void on_account_flow(const AccountFlowMsg& m) noexcept {
    const VenueId primary = cfg_.pools.primary(m.hdr.venue);
    if (primary.value >= kMaxVenues || lanes_[primary.value] == nullptr) return;
    TransferLane& lane = *lanes_[primary.value];
    TransferLane::Slot* t = lane.by_asset(m.asset.view());
    if (t == nullptr) return;
    const Timestamp at = m.hdr.exch_ts.valid() ? m.hdr.exch_ts : m.hdr.recv_ts;
    if (m.hdr.venue == t->from && m.amount.raw == -t->amount && !t->out_seen) {
      t->out_seen = true;
      release_lane(*t);
      balances_->transfer(t->from, t->asset.view(), -t->amount, at);
    } else if (m.hdr.venue == t->to && m.amount.raw == t->amount && !t->in_seen) {
      t->in_seen = true;
      balances_->transfer(t->to, t->asset.view(), t->amount, at);
    } else {
      return;
    }
    if (t->out_seen && t->in_seen) finish_lane(lane, *t, at, true);
  }
  // ControlCommand::PoolSweep: the operator turns a pool's sweep on, off or pauses it, or sweeps
  // one instrument at the next look.
  void on_pool_sweep(const ControlMsg& c) noexcept {
    const VenueId primary = cfg_.pools.primary(c.hdr.venue);
    if (primary.value >= kMaxVenues || lanes_[primary.value] == nullptr) {
      FASTMM_LOG_WARN("pool sweep for venue {} ignored: its pool has no sweep", c.hdr.venue.value);
      return;
    }
    TransferLane& lane = *lanes_[primary.value];
    switch (c.arg) {
      case kSweepOff:
        lane.set_off();
        set_lanes_live(lanes_live_ & ~(1U << primary.value));
        break;
      case kSweepOn:
        lane.set_on();
        break;
      case kSweepPause:
        lane.pause(now_);
        set_lanes_live(lanes_live_ & ~(1U << primary.value));
        break;
      case kSweepNow:
        if (instruments_.contains(c.hdr.instrument)) sweep_now_[c.hdr.instrument.value] = true;
        break;
      default:
        return;
    }
    FASTMM_LOG_WARN("{}: pool sweep {} by the operator (breaker {})",
                    venue_name(primary),
                    c.arg == kSweepOff     ? "off"
                    : c.arg == kSweepOn    ? "on"
                    : c.arg == kSweepPause ? "paused"
                                           : "now",
                    to_string(lane.breaker(now_)));
  }

  // The pool account a new order on `inst` goes from (EngineConfig::pools). The request's own
  // account when it names one of the pool, invalid when it names another venue. Otherwise the
  // usable members (order link live, no kill) whose balance covers the order (every member for a
  // derivative) whose connector would take an order now (no_order_room), and of those the one
  // with the most room in the 10 s order window, then in the daily one, room counted up to the
  // connector's cap (ctx.order_budget, which counts the orders sent since the venue's last
  // publication). Ties go round the pool: the search starts after the account the last automatic
  // choice with a known budget took, so a backtest without limits, which ties them all, keeps the
  // first in pool order. None: the primary, which refuses with the venue's own balance error.
  FASTMM_NOINLINE VenueId choose_account(const NewOrderRequest& req,
                                         const Instrument& inst) const noexcept {
    if (req.account.valid())
      return cfg_.pools.admits(inst.venue, req.account) ? req.account : VenueId{};
    if (cfg_.pools.members(inst.venue).size() == 1) return inst.venue;
    const VenueId best = pick_account(req, inst);
    return best.valid() ? best : inst.venue;
  }
  // choose_account's choice among the pool's accounts; invalid when none covers the order and has
  // room for it.
  [[nodiscard]] VenueId pick_account(const NewOrderRequest& req,
                                     const Instrument& inst) const noexcept {
    const PoolMembers members = cfg_.pools.members(inst.venue);
    const bool by_balance = balances_live_ && !inst.is_derivative();
    const Qty open = oms_.open_qty(req.instrument, req.side);
    VenueId best{};
    std::int64_t best_10s = -1;
    std::int64_t best_1d = -1;
    const std::size_t n = members.size();
    const std::size_t start = pool_turn_[inst.venue.value] % n;
    for (std::size_t k = 0; k < n; ++k) {
      const VenueId v = members[(start + k) % n];
      if (!account_usable(v)) continue;
      if (by_balance &&
          !balance_covers(
              inst, v, req.side, req.type, req.price, req.qty, 0, req.reduce_only, open))
        continue;
      const OrderBudget b = order_budget(v);
      if (no_order_room(b)) continue;  // its connector would refuse it: another account
      const std::int64_t left_10s = b.orders_10s.remaining();
      const std::int64_t left_1d = b.orders_1d.remaining();
      if (left_10s > best_10s || (left_10s == best_10s && left_1d > best_1d)) {
        best = v;
        best_10s = left_10s;
        best_1d = left_1d;
      }
    }
    return best;
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
    if (FASTMM_UNLIKELY(capacity_on_) && instruments_.contains(o.instrument))
      capacity_->on_request(instruments_.get(o.instrument).venue, now_.ns);
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
    if (FASTMM_UNLIKELY(risk_.underlying_on()))
      in.underlying = underlying_inputs(o.instrument, o.side);
    if (FASTMM_UNLIKELY(balances_live_) && balances_->check_enabled())
      in.balance_short = !balance_covers(inst,
                                         o.venue,
                                         o.side,
                                         o.type,
                                         px,
                                         qty,
                                         holds_[h.idx],
                                         o.has(Order::kReduceOnly),
                                         in.open_same_side - o.leaves_qty());
    in.reduce_only = reduce_only_[o.instrument.value];
    const RejectReason rr = risk_.check_replace(oi, o, inst, in);
    if (FASTMM_UNLIKELY(rr != RejectReason::None)) {
      ++stats_.risk_rejects;
      stats_.risk_rejects_by_reason.add(rr);
      log_risk_reject(rr, inst, o.side, px, qty, true);
      if constexpr (has_hook(Hook::RiskReject))
        queue_risk_reject(rr, inst, o.side, o.type, px, qty, o.cl_ord_id, o.user_tag, true);
      note_refusal(o.instrument, o.side, rr);
      if (FASTMM_UNLIKELY(records_.enabled())) {
        std::uint8_t flags = RejectRecord::kReplace;
        if (o.has(Order::kReduceOnly)) flags |= RejectRecord::kReduceOnly;
        if (reduces(o.instrument, o.side, qty)) flags |= RejectRecord::kReduces;
        record_reject(rr,
                      rr == RejectReason::RateLimit ? RejectSource::RiskBucket : RejectSource::Risk,
                      o.instrument,
                      o.venue,
                      o.side,
                      px,
                      qty,
                      flags,
                      o.cl_ord_id);
      }
      return fail(rr);
    }
    const ClientOrderId new_id = oms_.next_cl_ord_id();
    if (FASTMM_UNLIKELY(!new_id.valid())) return fail(on_ids_exhausted());
    auto r = oms_.request_replace(h, new_id, px, qty, now);
    if (!r) return r;
    const std::uint8_t arm = ab_arm_[o.instrument.value];
    oms_.get(h).arm = arm;
    OutReplaceMsg m{};
    init_header(m, EventType::OutReplace, o.instrument, o.venue);
    m.hdr.recv_ts = now;
    m.cl_ord_id = new_id;
    m.orig_cl_ord_id = o.cl_ord_id;
    m.venue_order_id = o.venue_order_id;
    m.price = px;
    m.qty = qty;
    m.arm = arm;
    own_outbound(m.hdr);
    queue_out(m.hdr);
    ++stats_.replaces_sent;
    note_sent_order(o.venue);
    note_inst_order(o.instrument);
    if (FASTMM_UNLIKELY(capacity_on_)) {
      capacity_->on_send(
          o.venue, inst.venue, o.instrument, o.side, inst.notional(px, qty).to_double(), now.ns);
    }
    return {};
  }

  // The strategy's state, copied for whoever writes EngineConfig::state_file (take_strategy_state).
  // Once per state_interval and at the end: a copy and an uncontended lock.
  FASTMM_NOINLINE void capture_state() noexcept {
    if constexpr (KeepsState<Strategy>) {
      const std::string_view bytes = strategy_.state();
      // No bytes (a StateWriter that ran out of room): the file keeps the state written before.
      if (bytes.empty()) {
        warn_empty_state();
        return;
      }
      const std::lock_guard<std::mutex> lock(state_mu_);
      state_pending_.assign(bytes.data(), bytes.size());
      state_dirty_ = true;
      ++state_captures_;
    }
  }
  // A checkpoint into the journal (StrategyStateMsg::Kind::Snapshot): the strategy's state() when
  // it keeps state (no bytes otherwise), the positions and the open orders.
  FASTMM_NOINLINE void journal_checkpoint() noexcept {
    if (!journal_.enabled()) return;
    static constexpr char kNoState[] = "";
    std::string_view state{kNoState};
    if constexpr (KeepsState<Strategy>) {
      state = strategy_.state();
      if (state.empty()) warn_empty_state();
    }
    if (!checkpoint_fits(state.size())) return;
    journal_state(StrategyStateMsg::Kind::Snapshot, state, 0, true);
  }
  // One state as StrategyStateMsg records: the strategy's bytes, then with `checkpoint` every
  // instrument's non-zero position (on its venue) and every open order. Records are copied straight
  // from `state` and from the buffers start() made.
  FASTMM_NOINLINE void journal_state(StrategyStateMsg::Kind kind,
                                     std::string_view state,
                                     std::uint8_t flags,
                                     bool checkpoint) noexcept {
    const std::uint64_t id = ++state_records_;
    write_state_part(id,
                     kind,
                     StrategyStateMsg::kStatePart,
                     flags,
                     reinterpret_cast<const std::byte*>(state.data()),
                     state.size());
    if (!checkpoint || !position_buf_ || !order_buf_) return;
    std::size_t n = 0;
    for (const Instrument& inst : instruments_) {
      const Position& p = positions_.get(inst.id);
      if (p.qty.is_zero()) continue;
      position_buf_[n++] =
          StatePosition{inst.id.value, inst.venue.value, {}, p.qty.raw, p.avg_px.raw};
    }
    write_state_part(id,
                     kind,
                     StrategyStateMsg::kPositionsPart,
                     flags,
                     reinterpret_cast<const std::byte*>(position_buf_.get()),
                     n * sizeof(StatePosition));
    std::size_t k = 0;
    oms_.for_each_open_order([&](Handle<Order>, const Order& o) {
      if (k == kMaxOpenOrders) return;
      order_buf_[k++] = StateOrder{o.cl_ord_id.value,
                                   o.instrument.value,
                                   o.venue.value,
                                   static_cast<std::uint8_t>(o.side),
                                   static_cast<std::uint8_t>(o.state),
                                   o.flags,
                                   o.price.raw,
                                   o.leaves_qty().raw};
    });
    write_state_part(id,
                     kind,
                     StrategyStateMsg::kOrdersPart,
                     flags,
                     reinterpret_cast<const std::byte*>(order_buf_.get()),
                     k * sizeof(StateOrder));
    ++stats_.checkpoints;
    stats_.checkpoint_bytes = state.size() + n * sizeof(StatePosition) + k * sizeof(StateOrder);
  }
  FASTMM_NOINLINE void warn_empty_state() noexcept {
    if (empty_state_warned_) return;
    empty_state_warned_ = true;
    FASTMM_LOG_WARN(
        "strategy {} returned no state: the state file keeps what it had, and "
        "checkpoints carry no strategy bytes (warned once)",
        strategy_name());
  }
  // A periodic or requested checkpoint goes into the journal ring only when it leaves a quarter of
  // the ring free for the events behind it: a full ring trips the kill switch, a skipped checkpoint
  // costs only itself. A take-over's is always written: its replay restores from it.
  [[nodiscard]] bool checkpoint_fits(std::size_t state_bytes) noexcept {
    const std::size_t bytes = state_bytes + instruments_.size() * sizeof(StatePosition) +
                              oms_.open_count() * sizeof(StateOrder);
    // Each record: the 128-byte head and up to 63 bytes of padding.
    const std::size_t need = bytes + (bytes / StrategyStateMsg::kChunkBytes + 3) * 192;
    const std::size_t room = journal_.room();
    if (need + journal_.capacity() / 4 <= room) return true;
    ++stats_.checkpoints_skipped;
    FASTMM_LOG_WARN(
        "checkpoint skipped: {} bytes of state, {} bytes free in the journal ring of {}",
        state_bytes,
        room,
        journal_.capacity());
    return false;
  }
  void write_state_part(std::uint64_t id,
                        StrategyStateMsg::Kind kind,
                        std::uint8_t part,
                        std::uint8_t flags,
                        const std::byte* bytes,
                        std::size_t total) noexcept {
    StrategyStateMsg m{};
    init_header(m, EventType::StrategyState);
    m.hdr.flags |= EventHeader::kSynthetic;
    m.hdr.recv_ts = now_;
    m.id = id;
    m.total = static_cast<std::uint32_t>(total);
    m.kind = kind;
    m.part = part;
    m.flags = flags;
    m.version = state_version_of(strategy_);
    std::size_t off = 0;
    do {
      const std::size_t n = std::min<std::size_t>(total - off, StrategyStateMsg::kChunkBytes);
      m.offset = static_cast<std::uint32_t>(off);
      m.bytes = static_cast<std::uint32_t>(n);
      m.hdr.len = StrategyStateMsg::size_for(m.bytes);
      if (!journal_.record_with(m.hdr, bytes + off, n)) {
        ++stats_.journal_overflows;
        on_journal_overflow();
        return;
      }
      off += n;
    } while (off < total);
  }
  // EngineConfig::initial_state, once, after on_start: bytes the strategy does not take leave it
  // as it started.
  FASTMM_NOINLINE void restore_state() noexcept {
    if constexpr (KeepsState<Strategy>) {
      if (cfg_.initial_state.empty()) return;
      const std::string_view from =
          cfg_.state_file.empty() ? std::string_view("the journal") : cfg_.state_file;
      if (strategy_.restore(cfg_.initial_state)) {
        state_restored_ = true;
        FASTMM_LOG_INFO("strategy {} restored {} bytes of state from {}",
                        strategy_name(),
                        cfg_.initial_state.size(),
                        from);
      } else {
        FASTMM_LOG_ERROR(
            "strategy {} did not take the {} bytes of state from {}; it runs on with "
            "what restore() left",
            strategy_name(),
            cfg_.initial_state.size(),
            from);
      }
      flush_out();
    }
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
      FASTMM_LOG_WARN("risk reject {} on {} order: {}:{} {} {} @ {}",
                      rr,
                      what,
                      venue_name(inst.venue),
                      inst.symbol,
                      side,
                      qty,
                      px);
    } else {
      FASTMM_LOG_WARN("risk reject {} on {} order: {}:{} {} {} @ {} ({} more suppressed)",
                      rr,
                      what,
                      venue_name(inst.venue),
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

  // An order of ours: a FastMM client order id from this engine's epochs, this session's or an
  // earlier one's.
  [[nodiscard]] bool own_order_id(ClientOrderId id) const noexcept {
    const std::uint16_t epoch = cl_ord_id_epoch(id);
    return id.valid() && epoch != 0 && epoch <= cfg_.session_epoch;
  }

  void cancel_unknown(const EventHeader& h, const OmsUpdate&) noexcept {
    // Never leave an unknown live order at the venue (5.8). We do not have an OMS record,
    // so send a bare cancel keyed by the venue's ids from the message. With [engine]
    // cancel_unknown = "own" an order whose id is not ours (a manual order, another program's)
    // stays.
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
    if (cfg_.cancel_unknown_own && !own_order_id(m.cl_ord_id)) {
      ++stats_.unknown_orders_kept;
      FASTMM_LOG_INFO(
          "leaving open order {} alone: not FastMM's ([engine] cancel_unknown = \"own\")",
          m.venue_order_id.view());
      return;
    }
    ++stats_.unknown_order_cancels;
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

  // Without a wire timestamp (the simulator, a replay) there is nothing to record: that test stays
  // inline.
  FASTMM_FORCE_INLINE void record_md_hops(Cycles t2) noexcept {
    if (event_t0_.v != 0) record_md_hops_from_wire(t2);
  }
  void record_md_hops_from_wire(Cycles t2) noexcept {
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
    flush_stale_reject_folds(now);
    publish_pool();
    if (!cfg_.publish_live && !journal_.enabled()) return;
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
    if (!cfg_.publish_live) return;
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
    live.reduce_only = ro_session_ ? 1 : 0;
    live.reduce_only_venues = ro_venues_;
    live.reduce_only_instruments = ro_count_;
    presence_.settle(now_);
    const QuotePresenceStats presence = presence_.total();
    live.quoting_elapsed_ns = presence.elapsed_ns;
    live.quoting_two_sided_ns = presence.two_sided_ns;
    live.max_loss_raw = risk_.limits().max_loss.raw;
    {
      const TokenBucket& bucket = risk_.bucket();
      live.risk_tokens = bucket.enabled() ? bucket.available(now_) : -1;
      live.risk_token_wait_ns = bucket.wait_ns(now_);
      live.risk_orders_per_sec = risk_.limits().orders_per_sec;
      live.risk_burst = risk_.limits().burst;
    }
    publish_quotes(live);
    live.metric_count = static_cast<std::uint32_t>(metric_count_);
    for (std::size_t k = 0; k < metric_count_; ++k) live.metrics[k] = metrics_[k];
    live.param_updates = stats_.param_updates;
    live.param_control_seq = control_param_seq_;
    live.param_last_seq = last_param_.publish_seq;
    live.param_last_ns = last_param_ts_.ns;
    live.param_last_origin = last_param_.origin;
    std::memcpy(live.param_last_source, last_param_.source, sizeof(live.param_last_source));
    for (std::size_t u = 0; u < cfg_.underlying.count; ++u) {
      auto& e = live.underlyings[u];
      e.known = risk_.underlying_net(
          u,
          instruments_,
          now_,
          [this](InstrumentId j) { return positions_.get(j).qty; },
          e.net_raw);
      e.max_net_raw = risk_.underlying_limit(u).raw;
    }
    const std::size_t rows = std::min(balances_->size(), kMaxLiveBalances);
    live.balance_count = static_cast<std::uint32_t>(rows);
    for (std::size_t i = 0; i < rows; ++i) {
      const BalanceBook::Row& r = balances_->row(i);
      LiveBalance& b = live.balances[i];
      b.venue = r.venue.value;
      b.account = r.account ? 1 : 0;
      b.known = r.reported ? 1 : 0;
      std::memcpy(b.asset, r.asset.data(), r.asset.size());
      b.free_raw = r.free;
      b.locked_raw = r.locked;
      b.total_raw = r.total;
      b.equity_raw = r.equity;
      b.maintenance_raw = r.maintenance;
      b.transferable_raw = r.transferable();
      b.as_of_ns = r.as_of.ns;
    }
    if (perp_.instruments() != 0) {
      for (const Instrument& inst : instruments_) {
        if (live.perp_count == kMaxLivePerps) break;
        if (perp_.row(inst.id).reports == 0) continue;
        fill_live_perp(live.perps[live.perp_count++], perp_, inst.id, inst.venue, now_);
      }
    }
    live.latency = latency;
    live_pub_->store(live);
  }

  // ---- one step ---------------------------------------------------------------------------------
  std::size_t step_from(const EventHeader* first) noexcept {
    ++stats_.steps;
    in_engine_ = true;
    catch_up();
    show_own_all();
    std::size_t n = 0;
    // Messages already in the feed (an [ab] arm's parameters, what a slow tier handed over) came
    // first: `first` goes behind them, as a wire's message would.
    if constexpr (requires {
                    feed_.empty();
                    feed_.push(*first);
                  }) {
      if (first != nullptr && !feed_.empty() && feed_.push(*first)) {
        if (market_ != nullptr) took_from_market(*first);
        first = nullptr;
      }
    }
    if (first != nullptr) {
      process(first);
      if (market_ != nullptr) took_from_market(*first);
      n = 1;
    }
    while (n < cfg_.max_events_per_step) {
      const EventHeader* h = feed_.next();
      if (h == nullptr) break;
      ++n;
      process(h);
      feed_.release();
    }
    batch_end();
    refresh_clock();
    const Timestamp now = clock_.now();
    if constexpr (!kFeedReplaysTimers)
      n += timers_.poll(now, [this](TimerId id, std::uint64_t ud) { on_timer_fired(id, ud); });
    batch_end();
    if (now - last_publish_ >= cfg_.latency_publish_interval) publish_latency(now);
    hide_own_all();
    update_interest();
    in_engine_ = false;
    return n;
  }

  // ---- shared market data (market_view.hpp) -----------------------------------------------------
  [[nodiscard]] static constexpr bool is_market_data(EventType t) noexcept {
    return t == EventType::BookDelta || t == EventType::BookSnapshot || t == EventType::Trade ||
           t == EventType::BookTicker || t == EventType::OptionTicker;
  }
  // What the messages the engine was not handed would have left: the risk engine's mids, book times
  // and last trades, the positions' marks, the feed's lag, the counts. Each of these takes the last
  // value of its message, so taking the last one is taking them all.
  FASTMM_FORCE_INLINE void catch_up() noexcept {
    if (market_ != nullptr && market_->counts.messages != market_seen_.messages) catch_up_market();
  }
  FASTMM_NOINLINE void catch_up_market() noexcept {
    const std::uint64_t seen = market_seen_.messages;
    const std::size_t n = instruments_.size();
    for (std::uint32_t i = 0; i < n; ++i) {
      const MarketView::Instrument& r = market_->inst[i];
      if (r.mid_at > seen) {
        const InstrumentId id{i};
        Price mid = r.mid;
        if (FASTMM_UNLIKELY(shows_own(i))) {
          Price bid = r.bid;
          Price ask = r.ask;
          own_touch(i, bid, ask);
          mid = Price::from_raw((bid.raw + ask.raw) / 2);
        }
        risk_.on_book(id, mid, r.mid_ts);
        positions_.mark(id, mid, instruments_.get(id));
      }
      if (r.trade_at > seen) risk_.on_trade(InstrumentId{i}, r.trade);
    }
    for (std::size_t v = 0; v < market_->health_at.size(); ++v) {
      if (market_->health_at[v] > seen) health_.copy_feed(market_->health, v);
    }
    stats_.events += market_->counts.messages - seen;
    stats_.book_updates += market_->counts.book_updates - market_seen_.book_updates;
    stats_.trades += market_->counts.trades - market_seen_.trades;
    market_seen_ = market_->counts;
  }
  // ---- our orders in a simulated feed (OwnLevelsMsg)
  // --------------------------------------------- The book of an instrument whose feed shows our
  // orders is the market's with our quantity added, during a step (show_own_all) and not between
  // steps (hide_own_all): the market's book is all the runs of a multiverse share, and each run
  // sees its own orders in it. Our quantity at or through the market's opposite touch is not shown:
  // the venue would have matched it, and the fill model waits for a trade.
  [[nodiscard]] bool shows_own(std::uint32_t i) const noexcept {
    return own_shown_ != nullptr &&
           !(own_shown_[i].levels[0].empty() && own_shown_[i].levels[1].empty());
  }
  void show_own(std::uint32_t i) noexcept {
    OwnShown& o = own_shown_[i];
    Book& b = books_[i];
    const Level bid = b.best_bid();
    const Level ask = b.best_ask();
    for (const Side side : {Side::Buy, Side::Sell}) {
      for (const Level& l : o.levels[static_cast<std::size_t>(side)]) {
        if (side == Side::Buy ? ask.qty.is_positive() && l.price >= ask.price
                              : bid.qty.is_positive() && l.price <= bid.price)
          continue;
        bool inserted = false;
        if (b.add_own(side, l.price, l.qty, inserted))
          static_cast<void>(
              o.added.push_back(typename OwnShown::Added{side, inserted, l.price, l.qty}));
      }
    }
  }
  void hide_own(std::uint32_t i) noexcept {
    OwnShown& o = own_shown_[i];
    Book& b = books_[i];
    for (std::size_t k = o.added.size(); k > 0; --k) {
      const typename OwnShown::Added& a = o.added[k - 1];
      b.remove_own(a.side, a.px, a.qty, a.inserted);
    }
    o.added.clear();
  }
  void show_own_all() noexcept {
    if (own_shown_ids_.empty()) return;
    for (const std::uint32_t i : own_shown_ids_) show_own(i);
    own_showing_ = true;
  }
  void hide_own_all() noexcept {
    if (!own_showing_) return;
    for (const std::uint32_t i : own_shown_ids_) hide_own(i);
    own_showing_ = false;
  }
  // The best bid and ask of the book with our quantity shown, from the market's (zero: none).
  void own_touch(std::uint32_t i, Price& bid, Price& ask) const noexcept {
    const OwnShown& o = own_shown_[i];
    const Price mb = bid;
    const Price ma = ask;
    for (const Level& l : o.levels[0]) {
      if ((!ma.is_positive() || l.price < ma) && (!bid.is_positive() || l.price > bid))
        bid = l.price;
    }
    for (const Level& l : o.levels[1]) {
      if ((!mb.is_positive() || l.price > mb) && (!ask.is_positive() || l.price < ask))
        ask = l.price;
    }
  }
  // A recorded book ticker with our best orders in it: a side's best order of ours shown at or
  // inside the recorded top (none at or through the opposite one).
  void show_own_top(std::uint32_t i, BookTickerMsg& m) const noexcept {
    const OwnShown& o = own_shown_[i];
    const Price rb = m.bid_px;
    const Price ra = m.ask_px;
    const auto show = [](Price& px, Qty& qty, const Level& l, bool buy) {
      if (!px.is_positive() || (buy ? l.price > px : l.price < px)) {
        px = l.price;
        qty = l.qty;
      } else if (l.price == px) {
        qty += l.qty;
      }
    };
    const Level* best = nullptr;
    for (const Level& l : o.levels[0]) {
      if (ra.is_positive() && l.price >= ra) continue;
      if (best == nullptr || l.price > best->price) best = &l;
    }
    if (best != nullptr) show(m.bid_px, m.bid_qty, *best, true);
    best = nullptr;
    for (const Level& l : o.levels[1]) {
      if (rb.is_positive() && l.price <= rb) continue;
      if (best == nullptr || l.price < best->price) best = &l;
    }
    if (best != nullptr) show(m.ask_px, m.ask_qty, *best, false);
  }
  // The box of the market's touch within which the mid of instrument i's book with our orders in
  // it stays strictly inside (lo, hi) and our orders stay shown or hidden as they are: false when
  // there is none (the engine then takes every update). Each bound is exclusive, as lo and hi are.
  [[nodiscard]] bool own_box(std::uint32_t i,
                             std::int64_t lo,
                             std::int64_t hi,
                             MdInterest::Instrument& n) const noexcept {
    const Book& b = books_[i];
    if (lo >= hi || !b.is_valid()) return false;
    constexpr std::int64_t kBelow = MdInterest::kBelow;
    constexpr std::int64_t kAbove = MdInterest::kAbove;
    const std::int64_t bb = b.best_bid().price.raw;
    const std::int64_t ba = b.best_ask().price.raw;
    if (bb <= 0) return false;  // the mid's rounding below assumes positive prices
    const OwnShown& o = own_shown_[i];
    std::int64_t bid_lo = 0;
    std::int64_t bid_hi = kAbove;
    std::int64_t ask_lo = 0;
    std::int64_t ask_hi = kAbove;
    // A bid of ours is shown while the market's ask is above it, an ask while its bid is below.
    std::int64_t vb = kBelow;  // the best of ours shown
    std::int64_t va = kAbove;
    for (const Level& l : o.levels[0]) {
      const std::int64_t p = l.price.raw;
      if (p < ba) {
        vb = std::max(vb, p);
        ask_lo = std::max(ask_lo, p);
      } else {
        ask_hi = std::min(ask_hi, p + 1);
      }
    }
    for (const Level& l : o.levels[1]) {
      const std::int64_t p = l.price.raw;
      if (p > bb) {
        va = std::min(va, p);
        bid_hi = std::min(bid_hi, p);
      } else {
        bid_lo = std::max(bid_lo, p - 1);
      }
    }
    if (vb != kBelow && va != kAbove && vb >= va) return false;  // ours cross each other
    // The touch with ours shown: our best where it is at or better than the market's, as long as
    // the market's stays at or behind it; else the market's, as long as it stays ahead of ours.
    const bool bid_ours = vb != kBelow && vb >= bb;
    const bool ask_ours = va != kAbove && va <= ba;
    if (bid_ours) {
      bid_hi = std::min(bid_hi, vb + 1);
    } else if (vb != kBelow) {
      bid_lo = std::max(bid_lo, vb);
    }
    if (ask_ours) {
      ask_lo = std::max(ask_lo, va - 1);
    } else if (va != kAbove) {
      ask_hi = std::min(ask_hi, va);
    }
    // The mid, (bid + ask) / 2 rounded down, strictly inside (lo, hi): bid + ask in
    // [2 lo + 2, 2 hi - 1].
    const Int128 b0 = bid_ours ? vb : bb;
    const Int128 a0 = ask_ours ? va : ba;
    const Int128 sum_lo = lo == kBelow ? std::numeric_limits<Int128>::min() : Int128{lo} * 2 + 2;
    const Int128 sum_hi = hi == kAbove ? std::numeric_limits<Int128>::max() : Int128{hi} * 2 - 1;
    if (b0 + a0 < sum_lo || b0 + a0 > sum_hi) return false;
    const auto clamp = [](Int128 v) {
      return v <= kBelow ? kBelow : v >= kAbove ? kAbove : static_cast<std::int64_t>(v);
    };
    const bool finite_lo = lo != kBelow;
    const bool finite_hi = hi != kAbove;
    if (!bid_ours && ask_ours) {
      if (finite_lo) bid_lo = std::max(bid_lo, clamp(sum_lo - a0 - 1));
      if (finite_hi) bid_hi = std::min(bid_hi, clamp(sum_hi - a0 + 1));
    } else if (bid_ours && !ask_ours) {
      if (finite_lo) ask_lo = std::max(ask_lo, clamp(sum_lo - b0 - 1));
      if (finite_hi) ask_hi = std::min(ask_hi, clamp(sum_hi - b0 + 1));
    } else if (!bid_ours && !ask_ours) {
      // Both move: each takes half of the room the sum has, down and up.
      const Int128 s0 = b0 + a0;
      if (finite_lo) {
        const Int128 down = s0 - sum_lo;
        bid_lo = std::max(bid_lo, clamp(b0 - down / 2 - 1));
        ask_lo = std::max(ask_lo, clamp(a0 - (down - down / 2) - 1));
      }
      if (finite_hi) {
        const Int128 up = sum_hi - s0;
        bid_hi = std::min(bid_hi, clamp(b0 + up / 2 + 1));
        ask_hi = std::min(ask_hi, clamp(a0 + (up - up / 2) + 1));
      }
    }
    if (!(bid_lo < bb && bb < bid_hi && ask_lo < ba && ba < ask_hi)) return false;
    n.lo = kBelow;
    n.hi = kAbove;
    n.bid_lo = bid_lo;
    n.bid_hi = bid_hi;
    n.ask_lo = ask_lo;
    n.ask_hi = ask_hi;
    return true;
  }

  // A message of the market taken (step_with): it counts as one the market will have taken.
  void took_from_market(const EventHeader& m) noexcept {
    if (m.type == EventType::OwnLevels) return;  // this run's own, not the market's
    ++market_seen_.messages;
    if ((m.type == EventType::BookDelta || m.type == EventType::BookSnapshot) &&
        instruments_.contains(m.instrument))
      ++market_seen_.book_updates;
    if (m.type == EventType::Trade) ++market_seen_.trades;
  }

  void interest_changed(InstrumentId id) noexcept {
    interest_stale_[id.value] = true;
    interest_any_stale_ = true;
  }
  // The mid band of each instrument within which no move of its mark can trip max_loss: the budget
  // left (net PnL + max_loss) shared over the instruments with a position. While each instrument's
  // loss from its mark now stays under its share, their sum stays under the budget. A band may be
  // narrower than it could be (the engine is then only woken sooner), never wider: its edge is
  // checked with the positions' own valuation. Set again when a position, a limit or the kill
  // state changes, or a mark leaves its band.
  void update_risk_bands() noexcept {
    risk_band_stale_ = false;
    band_changes_ = positions_.changes();
    for (const Instrument& inst : instruments_) {
      const std::size_t i = inst.id.value;
      if (risk_lo_[i] != MdInterest::kBelow || risk_hi_[i] != MdInterest::kAbove) {
        risk_lo_[i] = MdInterest::kBelow;
        risk_hi_[i] = MdInterest::kAbove;
        interest_changed(inst.id);
      }
    }
    const Notional limit = risk_.limits().max_loss;
    if (!limit.is_positive() || risk_.killed()) return;
    std::size_t held = 0;
    for (const Instrument& inst : instruments_) {
      if (!positions_.get(inst.id).qty.is_zero()) ++held;
    }
    if (held == 0) return;
    const Int128 left = static_cast<Int128>(net_pnl().raw) + limit.raw;
    const Int128 share = left > 0 ? left / static_cast<Int128>(held) : 0;
    for (const Instrument& inst : instruments_) {
      const std::size_t i = inst.id.value;
      const Position& p = positions_.get(inst.id);
      if (p.qty.is_zero()) continue;
      interest_changed(inst.id);
      const Price mark = p.last_mark;
      if (share <= 0 || !mark.is_positive()) {
        risk_lo_[i] = MdInterest::kAbove;  // every update
        continue;
      }
      // A long loses as the mid falls, a short as it rises; the band ends a sixteenth of the mark
      // below it or four times it above (a wider band would only wake the engine less).
      const bool lng = p.qty.is_positive();
      const std::int64_t end = lng ? mark.raw / 16
                                   : (mark.raw > std::numeric_limits<std::int64_t>::max() / 4
                                          ? std::numeric_limits<std::int64_t>::max()
                                          : mark.raw * 4);
      risk_lo_[i] = lng ? band_edge(inst, mark, end, share) : MdInterest::kBelow;
      risk_hi_[i] = lng ? MdInterest::kAbove : band_edge(inst, mark, end, share);
    }
  }
  // The band's edge on the side of `end` from `mark`: a mid e such that every mid strictly between
  // e and the mark loses less than `share` from the mark. `end` if it does itself; otherwise the
  // secant from the mark to `end` (exact for a linear contract), moved halfway to the mark until
  // the mid next to it is checked to lose less (a loss grows monotonically away from the mark).
  [[nodiscard]] std::int64_t band_edge(const Instrument& inst,
                                       Price mark,
                                       std::int64_t end,
                                       Int128 share) const noexcept {
    const Int128 at_mark = positions_.unrealized_at(inst.id, mark, inst).raw;
    const auto loss = [&](std::int64_t mid) {
      return at_mark - positions_.unrealized_at(inst.id, Price::from_raw(mid), inst).raw;
    };
    const Int128 at_end = loss(end);
    if (at_end < share) return end;
    const Int128 span = static_cast<Int128>(end) - mark.raw;
    auto e = static_cast<std::int64_t>(mark.raw + span * share / at_end);
    const std::int64_t step = end < mark.raw ? 1 : -1;  // from e towards the mark
    while (e != mark.raw && loss(e + step) >= share) e += (mark.raw - e + step) / 2;
    return e;
  }
  // md_interest() after a step, for an engine that follows a market.
  FASTMM_FORCE_INLINE void update_interest() noexcept {
    if (market_ != nullptr) update_market_interest();
  }
  FASTMM_NOINLINE void update_market_interest() noexcept {
    const bool every = journal_.enabled() || cfg_.max_param_age.ns > 0 || feed_lag_limit_.ns > 0 ||
                       queue_on_ || fx_on_ || perp_.marking() || quotes_.starved() ||
                       quotes_.backed_off();
    if (every != interest_.every) {
      interest_.every = every;
      ++interest_.epoch;
    }
    if (risk_band_stale_ || band_changes_ != positions_.changes()) update_risk_bands();
    if (!interest_any_stale_) return;
    interest_any_stale_ = false;
    for (const Instrument& inst : instruments_) {
      const std::size_t i = inst.id.value;
      if (!interest_stale_[i]) continue;
      interest_stale_[i] = false;
      const Watch& w = watch_[i];
      MdInterest::Instrument n;
      const bool book = has_hook(Hook::Book) && w.on;
      n.every_book = book && !w.quiet;
      n.lo = book && w.quiet ? w.lo.raw : MdInterest::kBelow;
      n.hi = book && w.quiet ? w.hi.raw : MdInterest::kAbove;
      n.lo = std::max(n.lo, risk_lo_[i]);
      n.hi = std::min(n.hi, risk_hi_[i]);
      // With our orders shown, the mid band is the market touch's box that keeps it.
      if (!n.every_book && shows_own(static_cast<std::uint32_t>(i)) &&
          (n.lo != MdInterest::kBelow || n.hi != MdInterest::kAbove) &&
          !own_box(static_cast<std::uint32_t>(i), n.lo, n.hi, n))
        n.every_book = true;
      n.trades = w.on && has_hook(Hook::Trade);
      n.tickers = w.on && (has_hook(Hook::BookTicker) || has_hook(Hook::OptionTicker));
      MdInterest::Instrument& o = interest_.inst[i];
      if (n.every_book == o.every_book && n.lo == o.lo && n.hi == o.hi && n.trades == o.trades &&
          n.tickers == o.tickers && n.bid_lo == o.bid_lo && n.bid_hi == o.bid_hi &&
          n.ask_lo == o.ask_lo && n.ask_hi == o.ask_hi)
        continue;
      n.epoch = ++interest_.epoch;
      o = n;
    }
  }

  // ---- members ----------------------------------------------------------------------------------

  EngineConfig cfg_;
  const InstrumentTable& instruments_;
  Clock& clock_;
  Transport& transport_;
  Feed& feed_;
  Strategy& strategy_;
  Context ctx_;
  std::unique_ptr<Book[]> own_books_;  // null: follow_market()
  Book* books_;
  // The market this engine follows (follow_market), and its counts the engine has taken.
  const MarketView* market_ = nullptr;
  MarketView::Counts market_seen_{};
  // What the strategy watches of each instrument (watch, quiet_book), the max_loss budget's band
  // (update_risk_bands) and what md_interest() makes of them.
  struct Watch {
    bool on = true;
    bool quiet = false;
    Price lo;
    Price hi;
  };
  std::array<Watch, kMaxInstruments> watch_{};
  std::array<std::int64_t, kMaxInstruments> risk_lo_ = filled(MdInterest::kBelow);
  std::array<std::int64_t, kMaxInstruments> risk_hi_ = filled(MdInterest::kAbove);
  bool risk_band_stale_ = true;     // a limit, a kill or a mark past its band since
  std::uint64_t band_changes_ = 0;  // positions_.changes() the bands were set at
  MdInterest interest_;
  std::array<bool, kMaxInstruments> interest_stale_ = filled_true();
  bool interest_any_stale_ = true;
  static constexpr std::array<std::int64_t, kMaxInstruments> filled(std::int64_t v) noexcept {
    std::array<std::int64_t, kMaxInstruments> a{};
    a.fill(v);
    return a;
  }
  static constexpr std::array<bool, kMaxInstruments> filled_true() noexcept {
    std::array<bool, kMaxInstruments> a{};
    a.fill(true);
    return a;
  }
  Oms oms_;
  RiskEngine risk_;
  VenueHealth health_;  // feed lag and ack RTT per venue, and the feed-lag gate
  Duration feed_lag_limit_ = milliseconds(cfg_.risk.max_feed_lag_ms);  // [risk] max_feed_lag_ms
  // [risk] fill_cluster_names: the fill-cluster guard (core/fill_cluster.hpp).
  FillCluster cluster_;
  bool cluster_on_ = cfg_.fill_cluster.enabled();
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
  // Risk rejects waiting for on_risk_reject (after_hooks); empty for a strategy without the hook.
  static constexpr std::size_t kMaxPendingRiskRejects = 64;
  static constexpr int kRiskRejectRounds = 4;
  StaticVector<RiskReject, kMaxPendingRiskRejects> risk_rejects_;
  EngineStats stats_{};
  Seqlocked<LatencySnapshot> latency_pub_;
  // Off the engine object: the publication is read by other threads once a second.
  std::unique_ptr<Seqlocked<EngineLiveStats>> live_pub_ =
      std::make_unique<Seqlocked<EngineLiveStats>>();
  Timestamp last_publish_{};
  // The last ParamUpdate applied (its fields are not read: the strategy holds the values), the
  // engine time of it, and the publish_seq of the last from the control socket (EngineLiveStats).
  std::unique_ptr<QuoteTrack[]> quote_track_ =
      std::make_unique<QuoteTrack[]>(static_cast<std::size_t>(kMaxInstruments) * 2);
  std::array<LiveMetric, kMaxStrategyMetrics> metrics_{};
  std::size_t metric_count_ = 0;
  // RejectRecord folding, per instrument and side (record_reject).
  static constexpr std::size_t kRejectFolds = kMaxInstruments * 2 * kMaxVenues;
  std::unique_ptr<RejectFold[]> reject_folds_ = std::make_unique<RejectFold[]>(kRejectFolds);
  bool reject_folds_pending_ = false;  // a fold may hold counted refusals
  ParamUpdateMsg last_param_{};
  Timestamp last_param_ts_{};
  std::uint64_t control_param_seq_ = 0;
  // [ab]: the arm each instrument is in (ParamUpdateMsg::arm), 0 none, k + 1 arm k. A new order
  // or replace takes its instrument's (Order::arm).
  std::array<std::uint8_t, kMaxInstruments> ab_arm_{};
  // The parameters each instrument holds values of its own for (strategies/param_overrides.hpp).
  std::array<ParamMask, kMaxInstruments> param_own_{};
  // [engine] markout_ms: per horizon, a ring of the fills whose mid is still to be read, in fill
  // order; markout_next_ns_ is the earliest one due (INT64_MAX: none).
  struct PendingMarkout {
    std::int64_t due_ns;
    std::int64_t fill_ns;
    std::uint64_t fill_seq;
    InstrumentId instrument;
  };
  static constexpr std::size_t kMaxMarkoutHorizons = 4;
  static constexpr std::size_t kMarkoutQueue = 4096;  // fills in one horizon's wait
  struct MarkoutQueue {
    std::int32_t horizon_ms = 0;
    std::vector<PendingMarkout> ring;  // kMarkoutQueue entries, sized at construction
    std::size_t head = 0;
    std::size_t size = 0;
  };
  std::vector<MarkoutQueue> markouts_;
  std::int64_t markout_next_ns_ = std::numeric_limits<std::int64_t>::max();
  Timestamp now_{};  // valid while latched_

  // heartbeat(): on a line of its own, which the watching thread reads now and then.
  alignas(kCacheLine) std::atomic<std::uint64_t> beat_{0};
  std::atomic<std::uint32_t> killed_open_{kUnknownOpenOrders};
  alignas(kCacheLine) std::byte out_storage_[kOutBatch * kOutSlotBytes] = {};
  StaticVector<const EventHeader*, kOutBatch> out_batch_;

  Cycles event_t0_{};
  Cycles event_t1_{};
  Cycles strategy_t3_{};
  bool sent_in_event_ = false;
  bool batch_end_wanted_ = false;    // ctx.request_batch_end() since the last on_batch_end
  bool checkpoint_wanted_ = false;   // ctx.request_checkpoint() since the last batch end
  bool empty_state_warned_ = false;  // warn_empty_state() has logged
  Cycles batch_t0_{};                // the origin of the event that asked last
  Cycles batch_t1_{};
  bool fx_on_;  // cfg_.fx converts ([accounting])
  // [capacity]: the price of an order slot (core/capacity_price.hpp); null when off.
  std::unique_ptr<CapacityPrice> capacity_;
  bool capacity_on_ = false;
  const bool pools_on_ = cfg_.pools.active;  // a venue has pool members: new orders are routed
  // A pool keeps homes ([venues.<primary>.pool]); homes_on_ implies pools_on_.
  const bool homes_on_ = cfg_.pools.active && cfg_.homes.active();
  bool latched_ = false;
  bool in_engine_ = false;  // inside step(), drain(), start() or finish()
  bool quoting_enabled_;
  // queue_.enabled(), next to the flags every event reads: without queue tracking the market-data
  // handlers do not touch the tracker's lines.
  bool queue_on_ = false;
  // A venue has reported balances: orders update their holds and check_balance applies.
  bool balances_live_ = false;
  std::uint32_t reconciling_ = 0;  // bit per venue between its reconciliation's Begin and End
  static constexpr Duration kReconcileHold = milliseconds(250);
  Timestamp reconcile_hold_until_{};  // check_reconcile_hold: the held quotes are pulled after it
  bool reconcile_hold_over_ = false;  // ... and have been, for this reconciliation
  // Bit per venue whose first reconciliation since the start has not ended (await_reconcile): its
  // position is the store's plus whatever the execution replay has booked so far, so no order is
  // sent (NotReconciled) and quoting stays off until every one of them has.
  std::uint32_t awaiting_ = 0;
  // The reconciliation in progress was preceded by a complete execution replay, so what it reports
  // is the venue's own and nothing in it has to be estimated (ReconcileMsg::kExecutionsExact).
  bool reconcile_exact_ = false;
  bool params_stale_;                  // max_param_age passed, or no ParamUpdate yet
  bool param_deadline_armed_ = false;  // param_deadline_ applies (max_param_age set, fresh)
  Timestamp param_deadline_{};
  TimerId param_timer_{};            // the engine's one-shot max_param_age timer
  TimerId ack_timer_{};              // the engine's repeating ack_timeout sweep
  TimerId flatten_timer_{};          // ... and its flatten sweep while one runs
  TimerId state_timer_{};            // ... and the strategy state capture (state_interval)
  TimerId checkpoint_timer_{};       // ... and the journal's checkpoint (checkpoint_interval)
  std::uint64_t state_records_ = 0;  // states written to the journal
  std::unique_ptr<StatePosition[]> position_buf_;  // a checkpoint's positions, made by start()
  std::unique_ptr<StateOrder[]> order_buf_;        // ... and its open orders
  // The strategy's state between capture_state (engine thread) and take_strategy_state (any).
  std::mutex state_mu_;
  std::string staged_state_;  // stage_strategy_state -> take_over, under state_mu_
  std::string state_pending_;
  bool state_dirty_ = false;
  bool state_restored_ = false;
  std::uint64_t state_captures_ = 0;
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
  // Reduce-only mode by scope (ControlCommand::ReduceOnly), and reduce_only_ per instrument, the
  // union the risk check reads; ro_count_ instruments it holds.
  bool ro_session_ = false;
  std::uint32_t ro_venues_ = 0;
  std::array<bool, kMaxInstruments> ro_inst_{};
  std::array<bool, kMaxInstruments> reduce_only_{};
  std::uint32_t ro_count_ = 0;
  // Bit per venue whose order link is not live (ConnectionStateMsg channel 1): the pool routing
  // skips such a member.
  std::uint32_t order_link_down_ = 0;
  // New orders and replaces sent per account, for order_budget against the connector's
  // OrderBudget::orders_taken; and per pool primary, where the next automatic choice starts.
  std::array<std::uint64_t, kMaxVenues> sent_orders_{};
  std::array<std::uint8_t, kMaxVenues> pool_turn_{};
  KillReason kill_reason_ = KillReason::None;
  Timestamp kill_ts_{};
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
  // With pools: each account's last reported position per instrument (set_reported_position), so
  // the instrument's position is their sum. Allocated at the first report.
  struct PoolPosition {
    Qty qty{};
    Price avg_px{};
    bool reported = false;
  };
  using PoolPositions = std::array<std::array<PoolPosition, PoolMembers::kMax>, kMaxInstruments>;
  std::unique_ptr<PoolPositions> pool_pos_;
  // With pools: each account's part of each instrument's position, from its own fills and its
  // position reports, by index in the pool (holding(); a spot instrument's is its base balance
  // once the account has reported it).
  using HeldTable = std::array<std::array<Qty, PoolMembers::kMax>, kMaxInstruments>;
  std::unique_ptr<HeldTable> held_;
  // Homes (core/pool_homes.hpp) by instrument, and a copy any thread reads (pool_load).
  std::array<VenueId, kMaxInstruments> home_{};
  std::array<std::atomic<std::uint8_t>, kMaxInstruments> home_pub_{};
  // Homes the planner and a spill leave alone: [venues.<primary>.pool] homes and the operator's
  // moves (fastmm-ctl pool home without --free); and the copy pool_load reads.
  std::array<bool, kMaxInstruments> pinned_{};
  std::array<std::atomic<bool>, kMaxInstruments> pinned_pub_{};
  // New orders and replaces by instrument since the start, for the planner (any thread).
  std::array<std::atomic<std::uint64_t>, kMaxInstruments> inst_orders_{};
  // Funding needs by instrument (set_funding_need; raw Notional), for pool_funding (any thread).
  std::array<std::atomic<std::int64_t>, kMaxInstruments> need_pub_{};
  // The payout a pool treasury holds (on_transfer), by the pool's primary, and the copy
  // pool_funding reads.
  struct TransferHold {
    bool active = false;
    std::uint64_t seq = 0;
    VenueId from{};
    FixedString<8> asset{};
    std::int64_t amount = 0;  // raw, held out of `from`'s free estimate
    Timestamp sent{};         // the Sent record's time
  };
  std::array<TransferHold, kMaxVenues> transfer_holds_{};
  // The pools' transfer lanes (core/transfer_lane.hpp), by the primary's venue id; null without.
  static constexpr std::size_t kLaneRing = 1024;
  bool lanes_on_ = false;
  std::array<std::unique_ptr<TransferLane>, kMaxVenues> lanes_{};
  // A lane sweeps now (sweep on, not a dry run, the breaker closed, the day's budget left): the
  // routing sends its reducing spot orders home. Set by sweep_tick and the lane's records.
  std::uint32_t lanes_live_ = 0;  // bit per primary
  std::atomic<std::uint32_t> lanes_live_pub_{0};
  TimerId sweep_timer_{};
  Timestamp lane_start_{};
  // What the sweep knows of each account's holding of each instrument: the base total it last saw
  // and since when, a refused transfer's wait, and (dry run) the total it last planned from.
  struct SweepCell {
    std::int64_t total = BalanceBook::kUnreported;
    Timestamp changed{};
    Timestamp retry{};
    std::int64_t planned = BalanceBook::kUnreported;
  };
  using SweepCells = std::array<std::array<SweepCell, PoolMembers::kMax>, kMaxInstruments>;
  std::unique_ptr<SweepCells> sweep_cells_;
  std::array<bool, kMaxInstruments> sweep_now_{};  // fastmm-ctl pool sweep SYMBOL
  // Intents and notices for the transfer worker (core/transfer_worker.hpp).
  std::unique_ptr<SpscRing<LaneIntent, kLaneRing>> lane_out_;
  std::array<std::atomic<std::uint64_t>, kMaxVenues> hold_seq_pub_{};
  std::array<std::atomic<std::int64_t>, kMaxVenues> hold_amt_pub_{};
  // By venue id: its 10 s order limit and whether it is usable, as publish_pool() last saw them.
  std::array<std::atomic<std::int64_t>, kMaxVenues> limit_pub_{};
  std::atomic<std::uint32_t> unusable_pub_{0};
  // Venues that have ended a balance snapshot, and the pools whose homes were reconciled once
  // every account of theirs had (bit per primary).
  std::uint32_t snapshot_seen_ = 0;
  std::uint32_t homes_reconciled_ = 0;
  // Venues whose feed shows our orders (bit v), and our quantity there; null when there are none.
  std::uint32_t own_venues_ = 0;
  std::unique_ptr<OwnQuantity> own_;
  // Our quantity a simulated feed shows (OwnLevelsMsg), per instrument and side by price, and what
  // of it the book holds now (show_own); null without venues whose feed shows our orders.
  struct OwnShown {
    struct Added {
      Side side;
      bool inserted;
      Price px;
      Qty qty;
    };
    StaticVector<Level, Book::kOwnLevels> levels[2];
    StaticVector<Added, 2 * Book::kOwnLevels> added;
  };
  std::unique_ptr<OwnShown[]> own_shown_;
  StaticVector<std::uint32_t, kMaxInstruments> own_shown_ids_;  // instruments with levels
  bool own_showing_ = false;                                    // in a step: the books show them
  // Balances per venue and asset, and the hold of each open order by its OMS slot (valid once a
  // venue has reported: balances_live_).
  std::unique_ptr<BalanceBook> balances_ = std::make_unique<BalanceBook>();
  // The venues' marks, indices and funding per instrument (on_perp_state).
  PerpBook perp_;
  std::unique_ptr<std::int64_t[]> holds_ = std::make_unique<std::int64_t[]>(kMaxOpenOrders);
  std::unique_ptr<bool[]> unacked_ = std::make_unique<bool[]>(kMaxOpenOrders);  // no ack yet
  QueueTracker queue_{cfg_.queue_conservatism_bps};
};

}  // namespace fastmm
