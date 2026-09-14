#pragma once
// Engine<Strategy, Clock, Transport, Feed>: the single decision thread (5.1, 5.6).
//
//   feed.next() -> journal(seq) -> dispatch(switch on type) -> book / OMS / positions
//               -> strategy hook -> QuoteManager diff -> risk -> OMS -> transport.send()
//
// Everything the engine touches after warm_up() is preallocated; no virtual calls, no
// exceptions, no heap. The same template runs live (TscClock, LiveTransport, RingFeed),
// in the simulator (SimClock, SimTransport, InlineFeed) and in replay.
//
// Strategy hooks are optional and detected with `requires`:
//   on_start(ctx) on_stop(ctx) on_book(ctx, id, book) on_trade(ctx, msg)
//   on_book_ticker(ctx, msg) on_fill(ctx, update, msg) on_order_update(ctx, update)
//   on_timer(ctx, id, user_data) on_connection(ctx, msg) on_option_ticker(ctx, msg)
#include "fastmm/core/book/l2_book.hpp"
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/static_vector.hpp"
#include "fastmm/core/engine_runner.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/latency.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/risk.hpp"
#include "fastmm/core/rng.hpp"
#include "fastmm/core/seqlock.hpp"
#include "fastmm/core/strategy_context.hpp"
#include "fastmm/core/thread_utils.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/timer_wheel.hpp"
#include "fastmm/core/transport.hpp"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace fastmm {

struct EngineConfig {
  std::uint64_t session_id = 0;
  std::uint64_t rng_seed = 1;
  std::uint16_t session_epoch = 1;
  std::uint32_t max_events_per_step = 64;
  Duration crossed_grace = milliseconds(100);
  Duration latency_publish_interval = seconds(1);
  bool quoting_enabled = true;
  int cpu = -1;
  SpinMode spin_mode = SpinMode::Busy;
  RiskLimits risk;
  QuoteParams quotes;
};

struct EngineStats {
  std::uint64_t events = 0;
  std::uint64_t book_updates = 0;
  std::uint64_t trades = 0;
  std::uint64_t orders_sent = 0;
  std::uint64_t cancels_sent = 0;
  std::uint64_t replaces_sent = 0;
  std::uint64_t fills = 0;
  std::uint64_t risk_rejects = 0;
  std::uint64_t journal_overflows = 0;
  std::uint64_t transport_full = 0;
  std::uint64_t timers_fired = 0;
  std::uint64_t crossed_pulls = 0;
  std::uint64_t unknown_order_cancels = 0;
  std::uint64_t kills = 0;
  std::uint64_t unconverted_fees = 0;  // fills whose commission asset is neither base nor quote
  std::uint64_t steps = 0;
  std::uint64_t clock_reanchors = 0;  // TscClock picked up a recalibration continuously
  std::uint64_t clock_steps = 0;      // ... or had to step (old mapping off by > threshold)
};

template <class Strategy, ClockLike Clock, TransportLike Transport, FeedLike Feed = RingFeed>
class Engine {
 public:
  using Book = L2Book<256>;
  using Context = StrategyContext<Engine>;
  static constexpr std::size_t kOutBatch = 32;
  static constexpr std::size_t kOutSlotBytes = 192;  // largest Out*Msg

  Engine(const EngineConfig& cfg,
         const InstrumentTable& instruments,
         Clock& clock,
         Transport& transport,
         Feed& feed,
         Strategy& strategy,
         MsgRing* journal_ring = nullptr)
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
        rng_(cfg.rng_seed),
        spin_(cfg.spin_mode),
        quoting_enabled_(cfg.quoting_enabled) {
    // Replace is only used if every venue we trade supports it (QuoteManager is global).
    QuoteParams qp = cfg.quotes;
    for (const Instrument& inst : instruments_) {
      if (!transport_.supports_replace(inst.venue)) qp.supports_replace = false;
    }
    quotes_.set_params(qp);
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
    return n;
  }

  void run() {
    pin_to_cpu(cfg_.cpu);
    set_thread_name("fm-engine");
    warm_up();
    start();
    while (!stop_.load(std::memory_order_relaxed)) {
      if (step() == 0) {
        spin_.idle();
      } else {
        spin_.active();
      }
    }
    finish();
  }
  void start() noexcept {
    if (started_) return;
    started_ = true;
    if constexpr (requires { strategy_.on_start(ctx_); }) strategy_.on_start(ctx_);
  }
  void finish() noexcept {
    if (!started_ || finished_) return;
    finished_ = true;
    if constexpr (requires { strategy_.on_stop(ctx_); }) strategy_.on_stop(ctx_);
    flush_out();
    publish_latency(clock_.now());
  }
  void stop() noexcept { stop_.store(true, std::memory_order_release); }
  [[nodiscard]] bool stopped() const noexcept { return stop_.load(std::memory_order_acquire); }

  // ---- strategy-facing API ------------------------------------------------------------------

  [[nodiscard]] Timestamp now() const noexcept { return clock_.now(); }
  [[nodiscard]] const Book& book(InstrumentId id) const noexcept { return books_[id.value]; }
  [[nodiscard]] Book& book_mut(InstrumentId id) noexcept { return books_[id.value]; }
  [[nodiscard]] const Position& position(InstrumentId id) const noexcept {
    return positions_.get(id);
  }
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
  [[nodiscard]] LatencyTracker& latency() noexcept { return latency_; }
  [[nodiscard]] JournalWriter& journal() noexcept { return journal_; }
  [[nodiscard]] QuoteManager& quote_manager() noexcept { return quotes_; }
  [[nodiscard]] TimerWheel<>& timers() noexcept { return timers_; }
  [[nodiscard]] Strategy& strategy() noexcept { return strategy_; }
  [[nodiscard]] Transport& transport() noexcept { return transport_; }
  [[nodiscard]] Context& context() noexcept { return ctx_; }
  [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const Seqlocked<LatencySnapshot>& latency_snapshot() const noexcept {
    return latency_pub_;
  }
  [[nodiscard]] EngineLiveStats live_stats() const noexcept { return live_pub_.load(); }
  [[nodiscard]] bool quoting_enabled() const noexcept {
    return quoting_enabled_ && !reconciling_ && !risk_.killed();
  }
  [[nodiscard]] bool reconciling() const noexcept { return reconciling_; }
  [[nodiscard]] const EngineConfig& config() const noexcept { return cfg_; }

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
    r.transport_full = stats_.transport_full;
    r.timers_fired = stats_.timers_fired;
    r.realized_pnl_raw = positions_.total_realized().raw;
    r.unrealized_pnl_raw = positions_.total_unrealized().raw;
    r.fees_raw = positions_.total_fees().raw;
    const auto& h = latency_.histogram(LatencyInterval::TickToTrade);
    r.tick_to_trade_p50_ns = h.percentile(0.5);
    r.tick_to_trade_p99_ns = h.percentile(0.99);
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
    auto r = submit_new(req);
    flush_out();
    return r;
  }
  Result<void, RejectReason> cancel_order(ClientOrderId id) noexcept {
    const Handle<Order> h = oms_.find(id);
    if (!h.valid()) return fail(RejectReason::UnknownOrder);
    auto r = submit_cancel(h);
    flush_out();
    return r;
  }
  Result<void, RejectReason> replace_order(ClientOrderId id, Price px, Qty qty) noexcept {
    const Handle<Order> h = oms_.find(id);
    if (!h.valid()) return fail(RejectReason::UnknownOrder);
    auto r = submit_replace(h, px, qty);
    flush_out();
    return r;
  }
  void set_quotes(InstrumentId id, const DesiredQuotes& q) noexcept {
    if (!quoting_enabled()) return;
    const Instrument& inst = instruments_.get(id);
    Placer place{this};
    quotes_.reconcile(inst, q, oms_, clock_.now(), place);
    flush_out();
  }
  void pull_quotes(InstrumentId id) noexcept {
    Placer place{this};
    quotes_.pull_quotes(instruments_.get(id), oms_, place);
    flush_out();
  }
  void pull_all_quotes() noexcept {
    Placer place{this};
    for (const Instrument& inst : instruments_) quotes_.pull_quotes(inst, oms_, place);
    flush_out();
  }
  // Cancels every working order (kill switch / control). Cancels are always allowed.
  void mass_cancel() noexcept {
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
    return timers_.add(clock_.now(), period, repeat, user_data);
  }
  bool cancel_timer(TimerId id) noexcept { return timers_.cancel(id); }

  // Injects an event as if it came from the feed (tests, control thread bypass in sim).
  void inject(const EventHeader* h) noexcept { process(h); }

 private:
  struct Placer {
    Engine* e;
    bool operator()(QuoteAction& a) noexcept { return e->place_quote(a); }
  };

  // ---- event loop ---------------------------------------------------------------------------

  void process(const EventHeader* h) noexcept {
    ++stats_.events;
    if (journal_.enabled()) {
      auto r = journal_.record(*h);
      if (FASTMM_UNLIKELY(!r)) {
        ++stats_.journal_overflows;
        on_journal_overflow();
      }
    }
    event_t0_ = h->t0_cycles;
    event_t1_ = Cycles{h->t0_cycles.v + h->t1_delta};
    // T3 belongs to this event only (see mark_decision()).
    strategy_t3_ = Cycles{};
    sent_in_event_ = false;
    dispatch(h);
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
      case EventType::Timer: {
        const auto& t = msg_cast<TimerMsg>(h);
        fire_strategy_timer(t.timer_id, t.user_data);
        break;
      }
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
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    const Timestamp now = clock_.now();
    const Instrument& inst = instruments_.get(id);
    if (FASTMM_LIKELY(b.is_valid())) {
      const Price mid = b.mid();
      risk_.on_book(id, mid, d.hdr.recv_ts.valid() ? d.hdr.recv_ts : now);
      positions_.mark(id, mid, inst);
      if (risk_.on_pnl(positions_.net_pnl())) on_kill();
    } else if (b.crossed() && b.crossed_for(now) > cfg_.crossed_grace) {
      ++stats_.crossed_pulls;
      pull_quotes(id);
    }
    if constexpr (requires { strategy_.on_book(ctx_, id, b); }) {
      strategy_.on_book(ctx_, id, b);
      record_strategy_hop(t2);
    }
    flush_out();
  }

  void on_trade(const TradeMsg& t) noexcept {
    ++stats_.trades;
    const InstrumentId id = t.hdr.instrument;
    if (instruments_.contains(id)) risk_.on_trade(id, t.price);
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    if constexpr (requires { strategy_.on_trade(ctx_, t); }) {
      strategy_.on_trade(ctx_, t);
      record_strategy_hop(t2);
    }
    flush_out();
  }

  void on_book_ticker(const BookTickerMsg& m) noexcept {
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    if constexpr (requires { strategy_.on_book_ticker(ctx_, m); }) {
      strategy_.on_book_ticker(ctx_, m);
      record_strategy_hop(t2);
    }
    flush_out();
  }

  void on_option_ticker(const OptionTickerMsg& m) noexcept {
    const Cycles t2 = clock_.cycles();
    record_md_hops(t2);
    if constexpr (requires { strategy_.on_option_ticker(ctx_, m); }) {
      strategy_.on_option_ticker(ctx_, m);
      record_strategy_hop(t2);
    }
    flush_out();
  }

  // ---- order events -------------------------------------------------------------------------

  void after_oms_update(const OmsUpdate& u, const EventHeader& h) noexcept {
    if (u.action == OmsAction::CancelUnknown) {
      cancel_unknown(h, u);
    }
    if (u.action == OmsAction::ReconcileNeeded) {
      FASTMM_LOG_WARN("order {} exceeded cancel-reject retries; reconciliation needed",
                      encode_cl_ord_id(u.order.cl_ord_id));
    }
    if (u.known && u.handle.valid() && instruments_.contains(u.order.instrument)) {
      Placer place{this};
      quotes_.on_order_update(u, instruments_.get(u.order.instrument), oms_, clock_.now(), place);
    } else if (u.terminal && instruments_.contains(u.order.instrument)) {
      Placer place{this};
      quotes_.on_order_update(u, instruments_.get(u.order.instrument), oms_, clock_.now(), place);
    }
    if (u.changed) {
      if constexpr (requires { strategy_.on_order_update(ctx_, u); })
        strategy_.on_order_update(ctx_, u);
    }
    flush_out();
  }

  void on_order_ack(const OrderAckMsg& m) noexcept { after_oms_update(oms_.on_ack(m), m.hdr); }
  void on_order_reject(const OrderRejectMsg& m) noexcept {
    const OmsUpdate u = oms_.on_reject(m);
    if (u.changed) {
      FASTMM_LOG_WARN(
          "order {} rejected: {} ({})", encode_cl_ord_id(m.cl_ord_id), m.reason, m.venue_code);
    }
    after_oms_update(u, m.hdr);
  }
  void on_cancel_ack(const OrderCancelAckMsg& m) noexcept {
    after_oms_update(oms_.on_cancel_ack(m), m.hdr);
  }
  void on_cancel_reject(const OrderCancelRejectMsg& m) noexcept {
    after_oms_update(oms_.on_cancel_reject(m), m.hdr);
  }
  void on_expired(const OrderExpiredMsg& m) noexcept {
    after_oms_update(oms_.on_expired(m), m.hdr);
  }

  void on_fill(const OrderFillMsg& f) noexcept {
    const OmsUpdate u = oms_.on_fill(f);
    if (u.action == OmsAction::Duplicate) return;
    ++stats_.fills;
    const InstrumentId id = u.known ? u.order.instrument : f.hdr.instrument;
    const Side side = u.known ? u.order.side : f.side;
    if (instruments_.contains(id)) {
      const Instrument& inst = instruments_.get(id);
      // Commission in the base asset changes what we hold (a buy receives qty - fee, a sell
      // delivers qty + fee) and costs fee * price in quote terms; commission in another asset
      // (BNB) cannot be valued here and is counted instead of being booked as quote.
      Qty held = f.qty;
      Notional fee = f.fee;
      if (f.fee_asset == FeeAsset::Base) {
        const Qty fee_base = Qty::from_raw(f.fee.raw);
        fee = inst.notional(f.price, fee_base);
        held = side == Side::Buy ? f.qty - fee_base : f.qty + fee_base;
      } else if (f.fee_asset == FeeAsset::Other) {
        if (stats_.unconverted_fees++ == 0) {
          FASTMM_LOG_WARN(
              "fill commission in an asset other than base or quote is not included in "
              "fees or positions (first on order {})",
              encode_cl_ord_id(f.cl_ord_id));
        }
        fee = Notional{};
      }
      if (held.raw > 0) {
        positions_.on_fill(id, side, f.price, held, fee, inst);
      } else {
        positions_.on_fill(id, side, f.price, f.qty, fee, inst);
      }
      if (risk_.on_pnl(positions_.net_pnl())) on_kill();
    }
    if (u.action == OmsAction::UnknownFill) {
      FASTMM_LOG_ERROR(
          "fill for unknown order {} qty {} @ {}", encode_cl_ord_id(f.cl_ord_id), f.qty, f.price);
    }
    if constexpr (requires { strategy_.on_fill(ctx_, u, f); }) strategy_.on_fill(ctx_, u, f);
    after_oms_update(u, f.hdr);
  }

  void on_position_update(const PositionUpdateMsg& m) noexcept {
    if (!instruments_.contains(m.hdr.instrument)) return;
    positions_.set(m.hdr.instrument, m.qty, m.avg_px);
  }

  // ---- control / connection / reconcile ------------------------------------------------------

  void on_control(const ControlMsg& c) noexcept {
    switch (c.command) {
      case ControlCommand::Stop:
        stop();
        break;
      case ControlCommand::PullQuotes:
        quoting_enabled_ = false;
        pull_all_quotes();
        break;
      case ControlCommand::ResumeQuotes:
        quoting_enabled_ = true;
        break;
      case ControlCommand::TripKill:
        risk_.trip();
        on_kill(/*requested=*/true);
        break;
      case ControlCommand::ResetKill:
        risk_.reset();
        quoting_enabled_ = true;
        break;
      case ControlCommand::RecalibrateTsc:
        // The calibrator publishes new calibrations and step() picks them up anyway; this
        // applies a just-published one before the rest of the step runs.
        refresh_clock();
        break;
      case ControlCommand::Reload:
        break;  // not implemented: configuration changes need a restart
      case ControlCommand::FlushStats:
        publish_latency(clock_.now());
        break;
    }
  }

  void on_connection_state(const ConnectionStateMsg& m) noexcept {
    const VenueId venue = m.hdr.venue;
    if (m.state != ConnState::Live) {
      for (const Instrument& inst : instruments_) {
        if (inst.venue != venue) continue;
        if (m.channel == 0) books_[inst.id.value].clear();
        pull_quotes(inst.id);
      }
    }
    if constexpr (requires { strategy_.on_connection(ctx_, m); }) strategy_.on_connection(ctx_, m);
    flush_out();
  }

  void on_reconcile(const ReconcileMsg& m) noexcept {
    switch (m.kind) {
      case ReconcileMsg::Kind::Begin:
        reconciling_ = true;
        pull_all_quotes();
        oms_.reconcile_begin();
        break;
      case ReconcileMsg::Kind::OpenOrder: {
        const OmsUpdate u = oms_.reconcile_open_order(m);
        after_oms_update(u, m.hdr);
        break;
      }
      case ReconcileMsg::Kind::Position:
        if (instruments_.contains(m.hdr.instrument))
          positions_.set(m.hdr.instrument, m.position_qty, m.avg_px);
        break;
      case ReconcileMsg::Kind::End:
        oms_.reconcile_end([&](const OmsUpdate& u) {
          if constexpr (requires { strategy_.on_order_update(ctx_, u); })
            strategy_.on_order_update(ctx_, u);
        });
        reconciling_ = false;
        break;
    }
  }

  // `requested`: the control thread asked for it (shutdown, operator); otherwise a risk limit or
  // an internal failure tripped it, which is an error.
  void on_kill(bool requested = false) noexcept {
    ++stats_.kills;
    quoting_enabled_ = false;
    if (requested) {
      FASTMM_LOG_WARN("kill switch requested (flags={:#x}); pulling quotes and cancelling all",
                      risk_.kill_flags());
    } else {
      FASTMM_LOG_ERROR("kill switch engaged (flags={:#x}); pulling quotes and cancelling all",
                       risk_.kill_flags());
    }
    pull_all_quotes();
    mass_cancel();
  }
  void on_journal_overflow() noexcept {
    // 5.5: journal ring full is fatal for determinism guarantees; trip and cancel everything.
    if (!risk_.killed()) {
      risk_.trip();
      on_kill();
    }
  }

  // ---- timers ---------------------------------------------------------------------------------

  void on_timer_fired(TimerId id, std::uint64_t user_data) noexcept {
    ++stats_.timers_fired;
    // Journal a synthetic TimerMsg so replay reproduces the strategy's timer calls.
    if (journal_.enabled()) {
      TimerMsg t{};
      init_header(t, EventType::Timer);
      t.timer_id = id;
      t.user_data = user_data;
      t.fire_ts = clock_.now();
      t.hdr.flags |= EventHeader::kSynthetic;
      if (!journal_.record(t.hdr)) {
        ++stats_.journal_overflows;
        on_journal_overflow();
      }
    }
    fire_strategy_timer(id, user_data);
  }
  void fire_strategy_timer(TimerId id, std::uint64_t user_data) noexcept {
    if constexpr (requires { strategy_.on_timer(ctx_, id, user_data); })
      strategy_.on_timer(ctx_, id, user_data);
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

  Result<ClientOrderId, RejectReason> submit_new(const NewOrderRequest& req) noexcept {
    if (FASTMM_UNLIKELY(!instruments_.contains(req.instrument)))
      return fail(RejectReason::InstrumentDisabled);
    const Instrument& inst = instruments_.get(req.instrument);
    const Timestamp now = clock_.now();
    OrderIntent oi{req.instrument, inst.venue, req.side, req.type, req.price, req.qty};
    RiskInputs in{now,
                  &positions_.get(req.instrument),
                  oms_.open_qty(req.instrument, req.side),
                  oms_.open_count(req.instrument),
                  oms_.best_own_px(req.instrument, opposite(req.side))};
    const RejectReason rr = risk_.check_new(oi, inst, in);
    if (FASTMM_UNLIKELY(rr != RejectReason::None)) {
      ++stats_.risk_rejects;
      return fail(rr);
    }
    const ClientOrderId id = oms_.next_cl_ord_id();
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
    m.hdr.recv_ts = clock_.now();
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
    const Timestamp now = clock_.now();
    OrderIntent oi{o.instrument, o.venue, o.side, o.type, px, qty};
    RiskInputs in{now,
                  &positions_.get(o.instrument),
                  oms_.open_qty(o.instrument, o.side),
                  oms_.open_count(o.instrument),
                  oms_.best_own_px(o.instrument, opposite(o.side))};
    const RejectReason rr = risk_.check_replace(oi, o, inst, in);
    if (FASTMM_UNLIKELY(rr != RejectReason::None)) {
      ++stats_.risk_rejects;
      return fail(rr);
    }
    const ClientOrderId new_id = oms_.next_cl_ord_id();
    auto r = oms_.request_replace(h, new_id, px, qty);
    if (!r) return r;
    OutReplaceMsg m{};
    init_header(m, EventType::OutReplace, o.instrument, o.venue);
    m.hdr.recv_ts = now;
    m.cl_ord_id = new_id;
    m.orig_cl_ord_id = o.cl_ord_id;
    m.venue_order_id = o.venue_order_id;
    m.price = px;
    m.qty = qty;
    queue_out(m.hdr);
    ++stats_.replaces_sent;
    return {};
  }

  void cancel_unknown(const EventHeader& h, const OmsUpdate&) noexcept {
    // Never leave an unknown live order at the venue (5.8). We do not have an OMS record,
    // so send a bare cancel keyed by the venue's ids from the message.
    ++stats_.unknown_order_cancels;
    OutCancelMsg m{};
    init_header(m, EventType::OutCancel, h.instrument, h.venue);
    m.hdr.recv_ts = clock_.now();
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
    if (journal_.enabled()) {
      for (const EventHeader* m : out_batch_) {
        if (!journal_.record_outbound(*m)) ++stats_.journal_overflows;
      }
    }
    const std::size_t ok =
        transport_.send(std::span<const EventHeader* const>(out_batch_.data(), out_batch_.size()));
    const Cycles t5 = clock_.cycles();
    if (FASTMM_UNLIKELY(ok != out_batch_.size())) {
      stats_.transport_full += out_batch_.size() - ok;
      FASTMM_LOG_ERROR("outbound transport full: {} message(s) dropped; tripping kill switch",
                       out_batch_.size() - ok);
      out_batch_.clear();
      if (!risk_.killed()) {
        risk_.trip();
        on_kill();
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
    EngineLiveStats live;
    live.stats = runner_stats();
    live.kills = stats_.kills;
    live.kill_flags = risk_.kill_flags();
    live.latency = s;
    live_pub_.store(live);
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
  QuoteManager quotes_;
  TimerWheel<> timers_;
  PositionTracker positions_;
  LatencyTracker latency_;
  JournalWriter journal_;
  Xoshiro256ss rng_;
  SpinPolicy spin_;
  EngineStats stats_{};
  Seqlocked<LatencySnapshot> latency_pub_;
  Seqlocked<EngineLiveStats> live_pub_;
  Timestamp last_publish_{};

  alignas(kCacheLine) std::byte out_storage_[kOutBatch * kOutSlotBytes] = {};
  StaticVector<const EventHeader*, kOutBatch> out_batch_;

  Cycles event_t0_{};
  Cycles event_t1_{};
  Cycles strategy_t3_{};
  bool sent_in_event_ = false;
  bool quoting_enabled_;
  bool reconciling_ = false;
  bool started_ = false;
  bool finished_ = false;
  std::atomic<bool> stop_{false};
};

}  // namespace fastmm
