#pragma once
// HedgeExecutor: keeps a strategy's exposure hedged with taker orders (docs/how-to/strategies/
// hedge-executor.md). A strategy owns one, configures it in on_start and forwards its hooks.
//
// Exposure and residual, in base units (linear contracts: quantity * contract multiplier):
//
//   residual = sum of source positions + sum of hedge positions - target
//
// The sources are the instruments whose position is the exposure (a maker's quote instrument);
// the target is what the strategy wants to hold net (0 by default). The hedges are instruments
// that can take the other side, in preference order, each with its own price tolerance.
//
// Sizing comes from positions only, never from a count of fills: a restart, a replayed or
// duplicated fill and an order whose outcome arrives late all end in the same place.
//
// One order in flight at a time, across every hedge instrument and the de-risk orders: the
// residual is one number, and two orders sized from it on two venues would both fill. While an
// order is open on any hedge instrument (a previous session's included, once reconciliation
// adopts it), nothing else goes out; its end sends the next one. No order goes out while a venue
// reconciles (StrategyContext::reconciling): a position may still lack a replayed execution.
//
// A hedge is an IOC limit on the first usable hedge instrument, priced its tolerance through the
// touch, of -residual in its contracts rounded down to the lot and capped at the instrument's
// max_qty (the remainder follows when it ends). None goes out when that rounds under the lot,
// min_qty or min_notional: the remainder waits for more exposure rather than being refused.
//
// A hedge instrument is usable when its venue's order channel is up, its venue is not killed, its
// book is valid, not older than Config::stale and not held by the feed-lag gate, and it is not
// benched. The first usable one takes the hedge; when its balance or margin cannot cover the order
// (StrategyContext::balance_room), the next usable one that can does. When none can, the hedge is
// held (logged and counted once per episode) until a balance report or a fill changes that.
//
// Failures: a hedge that ends with nothing filled, or that the engine refuses, is a failure of its
// instrument; the next hedge waits Config::retry. max_failures within failure_window bench the
// instrument for Config::bench while another is not benched; when it is the last one, the executor
// halts (no more hedges) until restart(). A hedge whose outcome the venue never reported (the
// ack-timeout cancel, a reconciliation drop with quantity unaccounted for, a generic VenueReject
// such as a REST timeout) holds every order for Config::uncertain_hold, so a fill still on its way
// is booked before the positions are trusted again.
//
// De-risking (Config::derisk_after > 0): when no hedge instrument has taken the residual for that
// long (unusable, balance short or halted), the executor reduces the exposure itself: reduce-only
// IOC orders on a source whose position has the residual's sign, priced derisk_tolerance through
// its touch, each at most derisk_step (base units), one at a time and derisk_interval apart. It
// stops as soon as a hedge instrument can take the residual again, or the residual is gone. The
// strategy must not quote the side a de-risk order crosses: pulling the quotes while can_hedge() is
// false and quoting only the reducing side while held() does that.
//
// The executor has no timer of its own: the strategy calls on_timer from one of its timers (the
// retry, hold and de-risk waits end at the next call after them). Every call is noexcept and
// allocation-free. Linear instruments only: an inverse leg leaves it idle with an error.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/venue_health.hpp"
#include "fastmm/strategies/hooks.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fastmm {

class HedgeExecutor {
 public:
  static constexpr std::size_t kMaxSources = 4;
  static constexpr std::size_t kMaxHedges = 4;
  static constexpr std::size_t kMaxFailures = 16;

  struct Config {
    std::string_view name = "hedge";     // log prefix
    std::uint32_t tag = 0;               // user_tag of hedge orders
    std::uint32_t derisk_tag = 0;        // user_tag of de-risk orders
    Duration retry = milliseconds(200);  // after a hedge that filled nothing
    int max_failures = 5;                // within failure_window: bench, or halt on the last one
    Duration failure_window = milliseconds(60000);
    Duration uncertain_hold = milliseconds(5000);
    Duration stale{};                      // a hedge book older than this is unusable; 0: never
    Duration bench = milliseconds(60000);  // how long a failing instrument sits out
    Duration derisk_after{};               // 0: never de-risk
    Qty derisk_step{};                     // base units per de-risk order; 0: the whole residual
    Duration derisk_interval = milliseconds(1000);
    Ratio derisk_tolerance{};
  };

  struct Stats {
    std::uint64_t hedges_sent = 0;
    std::uint64_t hedge_failures = 0;  // ended with nothing filled, or refused by the engine
    std::uint64_t uncertain_ends = 0;  // ended without the venue saying how
    std::uint64_t halts = 0;
    std::uint64_t hedges_held = 0;  // episodes of a hedge no balance or margin could cover
    std::uint64_t failovers = 0;    // hedging moved to a later instrument in the list
    std::uint64_t benches = 0;      // instruments benched after max_failures
    std::uint64_t derisk_episodes = 0;
    std::uint64_t derisk_orders = 0;
  };

  // What the last evaluation found.
  enum class State : std::uint8_t {
    Idle,         // not started, or nothing evaluated yet
    Flat,         // the residual is zero
    Waiting,      // the residual rounds under the hedge instrument's minimum
    InFlight,     // a hedge or de-risk order is open
    Reconciling,  // a venue reconciles
    Holding,      // retry backoff or uncertain hold
    Held,         // no usable instrument's balance or margin covers the hedge
    Unavailable,  // no hedge instrument is usable
    Halted,       // every hedge instrument failed max_failures times; restart() clears it
  };

  // Why a hedge instrument is not usable now.
  enum class Reason : std::uint8_t { Usable, Down, Killed, NoBook, Stale, Gated, Benched };

  struct Status {
    State state = State::Idle;
    Qty residual{};
    std::uint8_t active = 0;  // index of the hedge instrument hedges go to
    bool in_flight = false;
    bool held = false;
    bool halted = false;
    bool derisking = false;
    Timestamp hold_until{};  // end of the last retry or uncertain hold
  };

  // ---- setup (on_start) ---------------------------------------------------------------------

  // Forgets the legs and every state but the stats. Call before adding legs in on_start.
  void reset() noexcept {
    n_sources_ = 0;
    n_hedges_ = 0;
    ready_ = false;
    target_ = Qty{};
    clear_state();
  }
  bool add_source(InstrumentId id) noexcept {
    if (n_sources_ == kMaxSources) return false;
    sources_[n_sources_++] = Leg{id, Ratio{}, 0};
    return true;
  }
  bool add_hedge(InstrumentId id, Ratio tolerance) noexcept {
    if (n_hedges_ == kMaxHedges) return false;
    hedges_[n_hedges_++] = Leg{id, tolerance, 0};
    return true;
  }
  // Checks the legs against the instrument table: each is configured, linear, used once, and there
  // is at least one of each. Logs the reason and stays idle when not.
  template <class Ctx>
  bool start(Ctx& ctx, const Config& cfg) noexcept {
    cfg_ = cfg;
    clear_state();
    ready_ = false;
    if (n_sources_ == 0 || n_hedges_ == 0) {
      FASTMM_LOG_ERROR("{}: needs a source and a hedge instrument; not hedging", cfg_.name);
      return false;
    }
    const auto n = ctx.instruments().size();
    for (std::size_t i = 0; i < n_sources_ + n_hedges_; ++i) {
      const InstrumentId id = leg(i).id;
      if (id.value >= n) {
        FASTMM_LOG_ERROR("{}: instrument {} is not among the {} configured; not hedging",
                         cfg_.name,
                         id.value,
                         n);
        return false;
      }
      const Instrument& inst = ctx.instrument(id);
      if (inst.inverse() || !inst.contract_multiplier.is_positive()) {
        FASTMM_LOG_ERROR(
            "{}: {} is not a linear contract; not hedging", cfg_.name, inst.symbol.view());
        return false;
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (leg(j).id == id) {
          FASTMM_LOG_ERROR("{}: {} is named twice; not hedging", cfg_.name, inst.symbol.view());
          return false;
        }
      }
    }
    for (std::size_t i = 0; i < n_sources_ + n_hedges_; ++i)
      venue_of_[i] = ctx.instrument(leg(i).id).venue;
    ready_ = true;
    return true;
  }
  // Parameters may change while running (on_params); the legs may not.
  void set_config(const Config& cfg) noexcept { cfg_ = cfg; }
  void set_tolerance(std::size_t hedge, Ratio tolerance) noexcept {
    if (hedge < n_hedges_) hedges_[hedge].tolerance = tolerance;
  }
  // The net base position the strategy wants to hold; the residual is measured from it.
  void set_target(Qty base) noexcept { target_ = base; }
  // Clears a halt, the benches and the failure counts; hedging resumes from the positions.
  void restart() noexcept {
    halted_ = false;
    next_ns_ = 0;
    for (std::size_t i = 0; i < kMaxHedges; ++i) {
      failures_[i] = 0;
      bench_until_[i] = 0;
    }
  }

  // ---- hooks: forward the strategy's -----------------------------------------------------------

  // A fill of any leg: the residual moved. False for another instrument.
  template <class Ctx>
  bool on_fill(Ctx& ctx, const Fill& fill) noexcept {
    if (!ready_ || !is_leg(fill.instrument)) return false;
    update(ctx);
    return true;
  }

  // The end of a hedge or de-risk order: counts a failure or holds, then looks again. False for
  // anything else (another instrument, or an update that is not terminal).
  template <class Ctx>
  bool on_order_update(Ctx& ctx, const OmsUpdate& u) noexcept {
    if (!ready_ || !u.terminal) return false;
    const Order& o = u.order;
    const std::int64_t now = ctx.now().ns;
    if (derisk_open_ && o.cl_ord_id == derisk_id_) {
      derisk_open_ = false;
      if (uncertain(u)) hold_uncertain(o, now);
      update(ctx);
      return true;
    }
    const int k = hedge_index(o.instrument);
    if (k < 0) return false;
    if (o.cum_qty.is_zero()) failed(ctx, static_cast<std::size_t>(k), now);
    if (uncertain(u)) hold_uncertain(o, now);
    update(ctx);
    return true;
  }

  // A hedge instrument's book: a new touch to price against.
  template <class Ctx>
  void on_book(Ctx& ctx, InstrumentId id) noexcept {
    if (ready_ && hedge_index(id) >= 0) update(ctx);
  }

  template <class Ctx>
  void on_timer(Ctx& ctx) noexcept {
    update(ctx);
  }

  // Order channels (channel != 0) of the legs' venues. Market data is covered by the books'
  // validity: the engine clears a venue's books when its channel drops, and a book resync is a
  // Resyncing followed by snapshots with no Live. Does not send: the next event does.
  template <class Ctx>
  void on_connection(Ctx&, const ConnectionStateMsg& m) noexcept {
    if (!ready_ || m.channel == 0) return;
    const std::uint32_t bit = 1U << (m.channel & 31U);
    for (std::size_t i = 0; i < n_sources_ + n_hedges_; ++i) {
      Leg& l = leg(i);
      if (venue_of_[i] != m.hdr.venue) continue;
      if (m.state == ConnState::Live) {
        l.down &= ~bit;
      } else {
        l.down |= bit;
      }
    }
  }

  // A balance report of a leg's venue: a held hedge may fit now. False for another venue.
  template <class Ctx>
  bool on_balance(Ctx& ctx, const BalanceMsg& m) noexcept {
    if (!ready_ || !involves(m.hdr.venue)) return false;
    update(ctx);
    return true;
  }

  // Looks at the positions and sends what they call for; the hooks above end here.
  template <class Ctx>
  void update(Ctx& ctx) noexcept {
    if (!ready_) return;
    const std::int64_t now = ctx.now().ns;
    if (in_flight(ctx)) {
      state_ = State::InFlight;
      return;
    }
    if (ctx.reconciling()) {
      state_ = State::Reconciling;
      return;
    }
    if (now < next_ns_) {
      state_ = State::Holding;
      return;
    }
    const Qty residual = this->residual(ctx);
    if (halted_) {
      state_ = State::Halted;
      blocked(ctx, residual, now);
      return;
    }
    if (residual.is_zero()) {
      state_ = State::Flat;
      unblock("flat");
      return;
    }
    std::size_t first = n_hedges_;
    for (std::size_t i = 0; i < n_hedges_; ++i) {
      if (why(ctx, i, now) == Reason::Usable) {
        first = i;
        break;
      }
    }
    if (first == n_hedges_) {
      if (!unavailable_) {
        unavailable_ = true;
        FASTMM_LOG_WARN("{}: no hedge instrument can take {} ({} is {})",
                        cfg_.name,
                        residual,
                        ctx.instrument(hedges_[0].id).symbol.view(),
                        to_string(why(ctx, 0, now)));
      }
      state_ = State::Unavailable;
      blocked(ctx, residual, now);
      return;
    }
    // The first usable instrument decides the size; a hedge its balance cannot cover goes to the
    // next usable instrument that can take it.
    const std::optional<NewOrderRequest> sized = order_on(ctx, first, residual);
    if (!sized) {
      state_ = State::Waiting;
      unblock("the residual is under the hedge minimum");
      return;
    }
    NewOrderRequest req = *sized;
    std::size_t k = first;
    if (!covered(ctx, req)) {
      k = n_hedges_;
      for (std::size_t j = first + 1; j < n_hedges_; ++j) {
        if (why(ctx, j, now) != Reason::Usable) continue;
        const std::optional<NewOrderRequest> r = order_on(ctx, j, residual);
        if (r && covered(ctx, *r)) {
          req = *r;
          k = j;
          break;
        }
      }
    }
    if (k == n_hedges_) {
      if (!held_) {
        held_ = true;
        ++stats_.hedges_held;
        FASTMM_LOG_WARN(
            "{}: hedge {} {} @ {} held: no usable hedge venue's balance covers it ({} on {}); {} "
            "unhedged until a balance report or a fill changes that",
            cfg_.name,
            req.side,
            req.qty,
            req.price,
            ctx.balance_room(req.instrument, req.side, req.price),
            ctx.instrument(req.instrument).symbol.view(),
            residual);
      }
      state_ = State::Held;
      blocked(ctx, residual, now);
      return;
    }
    if (held_) {
      held_ = false;
      FASTMM_LOG_INFO("{}: the hedge venue's balance covers the hedge again", cfg_.name);
    }
    if (k != active_) switch_to(ctx, k, now);
    unblock("a hedge instrument takes the residual again");
    const auto sent = ctx.send(req);
    if (!sent) {
      FASTMM_LOG_WARN("{}: hedge {} {} @ {} refused: {}",
                      cfg_.name,
                      req.side,
                      req.qty,
                      req.price,
                      sent.error());
      failed(ctx, k, now);
      return;
    }
    ++stats_.hedges_sent;
    state_ = State::InFlight;
  }

  // ---- state -----------------------------------------------------------------------------------

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] bool halted() const noexcept { return halted_; }
  // The last hedge the positions asked for fits no usable instrument's balance.
  [[nodiscard]] bool held() const noexcept { return held_; }
  [[nodiscard]] bool derisking() const noexcept { return derisking_; }
  [[nodiscard]] State state() const noexcept { return state_; }
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t hedge_count() const noexcept { return n_hedges_; }
  [[nodiscard]] InstrumentId hedge_id(std::size_t i) const noexcept { return hedges_[i].id; }
  [[nodiscard]] std::size_t active() const noexcept { return active_; }
  [[nodiscard]] Qty target() const noexcept { return target_; }
  [[nodiscard]] bool is_hedge(InstrumentId id) const noexcept { return hedge_index(id) >= 0; }
  [[nodiscard]] bool is_source(InstrumentId id) const noexcept {
    for (std::size_t i = 0; i < n_sources_; ++i) {
      if (sources_[i].id == id) return true;
    }
    return false;
  }
  [[nodiscard]] bool is_leg(InstrumentId id) const noexcept {
    return is_source(id) || is_hedge(id);
  }
  // A venue of any leg.
  [[nodiscard]] bool involves(VenueId v) const noexcept {
    for (std::size_t i = 0; i < n_sources_ + n_hedges_; ++i) {
      if (venue_of_[i] == v) return true;
    }
    return false;
  }

  // Sources plus hedges minus the target, base units.
  template <class Ctx>
  [[nodiscard]] Qty residual(const Ctx& ctx) const noexcept {
    Qty r = -target_;
    for (std::size_t i = 0; i < n_sources_ + n_hedges_; ++i) {
      const InstrumentId id = leg(i).id;
      r += to_base(ctx.instrument(id), ctx.position(id).qty);
    }
    return r;
  }

  // Why hedge instrument `i` cannot take a hedge now (Reason::Usable: it can).
  template <class Ctx>
  [[nodiscard]] Reason why(const Ctx& ctx, std::size_t i, std::int64_t now) const noexcept {
    const Leg& h = hedges_[i];
    const VenueId v = venue_of_[n_sources_ + i];
    if (h.down != 0) return Reason::Down;
    if (ctx.venue_killed(v)) return Reason::Killed;
    if (now < bench_until_[i]) return Reason::Benched;
    const auto& b = ctx.book(h.id);
    if (!b.is_valid()) return Reason::NoBook;
    if (cfg_.stale.ns > 0) {
      const Timestamp t = b.last_update();
      if (t.valid() && now - t.ns > cfg_.stale.ns) return Reason::Stale;
    }
    if (ctx.venue_health(v).gated) return Reason::Gated;
    return Reason::Usable;
  }

  // Some hedge instrument can take a hedge now, and hedging is not halted. A quoting strategy pulls
  // its quotes while this is false: a fill could not be hedged, and a de-risk order may be working.
  template <class Ctx>
  [[nodiscard]] bool can_hedge(const Ctx& ctx) const noexcept {
    if (!ready_ || halted_) return false;
    const std::int64_t now = ctx.now().ns;
    for (std::size_t i = 0; i < n_hedges_; ++i) {
      if (why(ctx, i, now) == Reason::Usable) return true;
    }
    return false;
  }

  template <class Ctx>
  [[nodiscard]] Status status(const Ctx& ctx) const noexcept {
    Status s;
    s.state = state_;
    s.residual = residual(ctx);
    s.active = static_cast<std::uint8_t>(active_);
    s.in_flight = in_flight(ctx);
    s.held = held_;
    s.halted = halted_;
    s.derisking = derisking_;
    s.hold_until = Timestamp{next_ns_};
    return s;
  }

  // ---- pure functions --------------------------------------------------------------------------

  // Contracts to base units and back (linear: qty * multiplier).
  [[nodiscard]] static constexpr Qty to_base(const Instrument& inst, Qty contracts) noexcept {
    return Qty::from_raw(mul_raw(contracts, inst.contract_multiplier));
  }
  [[nodiscard]] static constexpr Qty to_contracts(const Instrument& inst, Qty base) noexcept {
    if (!inst.contract_multiplier.is_positive()) return Qty{};
    return Qty::from_raw(detail::mul_div(base.raw, kFixedScale, inst.contract_multiplier.raw));
  }

  // The order on `inst` that brings `residual` (base units) back towards zero: an IOC of
  // -residual in contracts, rounded down to the lot and capped at max_qty, priced `tolerance`
  // through the touch and rounded towards it. None when it rounds under the lot or min_qty, its
  // notional is under min_notional, or the touch it needs is empty: a hedge the venue (or the risk
  // check) must refuse would only count as a failure.
  [[nodiscard]] static std::optional<NewOrderRequest> hedge_order(const Instrument& inst,
                                                                  Qty residual,
                                                                  Price best_bid,
                                                                  Price best_ask,
                                                                  Ratio tolerance,
                                                                  std::uint32_t tag) noexcept {
    if (residual.is_zero()) return std::nullopt;
    const Side side = residual.is_negative() ? Side::Buy : Side::Sell;
    Qty qty = inst.round_qty(to_contracts(inst, residual.abs()));
    if (inst.max_qty.is_positive() && qty > inst.max_qty) qty = inst.round_qty(inst.max_qty);
    if (!qty.is_positive() || qty < inst.min_qty) return std::nullopt;
    Price px;
    if (side == Side::Buy) {
      if (!best_ask.is_positive()) return std::nullopt;
      px = inst.round_price(best_ask + best_ask * tolerance, Side::Buy);
    } else {
      if (!best_bid.is_positive()) return std::nullopt;
      px = inst.round_price(best_bid - best_bid * tolerance, Side::Sell);
    }
    if (!px.is_positive()) return std::nullopt;
    if (inst.min_notional.is_positive() && inst.notional(px, qty) < inst.min_notional)
      return std::nullopt;
    return NewOrderRequest::limit(inst.id, side, px, qty).ioc().tag(tag);
  }

  // A terminal update whose outcome the venue did not report: the ack timeout cancelled an order
  // the venue never acknowledged, reconciliation found it gone with quantity unaccounted for, or a
  // connector turned a request whose outcome it does not know into a generic reject.
  [[nodiscard]] static bool uncertain(const OmsUpdate& u) noexcept {
    const Order& o = u.order;
    return u.unresolved_qty.is_positive() ||
           (o.state == OrderState::Canceled && o.venue_order_id.empty() && o.cum_qty.is_zero()) ||
           (o.state == OrderState::Rejected && o.reject_reason == RejectReason::VenueReject);
  }

  [[nodiscard]] static constexpr std::string_view to_string(Reason r) noexcept {
    switch (r) {
      case Reason::Usable:
        return "usable";
      case Reason::Down:
        return "down";
      case Reason::Killed:
        return "killed";
      case Reason::NoBook:
        return "without a book";
      case Reason::Stale:
        return "stale";
      case Reason::Gated:
        return "held by the feed-lag gate";
      case Reason::Benched:
        return "benched";
    }
    return "?";
  }
  [[nodiscard]] static constexpr std::string_view to_string(State s) noexcept {
    switch (s) {
      case State::Idle:
        return "idle";
      case State::Flat:
        return "flat";
      case State::Waiting:
        return "waiting";
      case State::InFlight:
        return "in flight";
      case State::Reconciling:
        return "reconciling";
      case State::Holding:
        return "holding";
      case State::Held:
        return "held";
      case State::Unavailable:
        return "unavailable";
      case State::Halted:
        return "halted";
    }
    return "?";
  }

 private:
  struct Leg {
    InstrumentId id{};
    Ratio tolerance{};
    std::uint32_t down = 0;  // bit per order channel of the venue that is not Live
  };

  [[nodiscard]] Leg& leg(std::size_t i) noexcept {
    return i < n_sources_ ? sources_[i] : hedges_[i - n_sources_];
  }
  [[nodiscard]] const Leg& leg(std::size_t i) const noexcept {
    return i < n_sources_ ? sources_[i] : hedges_[i - n_sources_];
  }
  [[nodiscard]] int hedge_index(InstrumentId id) const noexcept {
    for (std::size_t i = 0; i < n_hedges_; ++i) {
      if (hedges_[i].id == id) return static_cast<int>(i);
    }
    return -1;
  }

  void clear_state() noexcept {
    for (std::size_t i = 0; i < kMaxSources + kMaxHedges; ++i) venue_of_[i] = VenueId{};
    for (Leg& l : sources_) l.down = 0;
    for (Leg& l : hedges_) l.down = 0;
    next_ns_ = 0;
    for (std::size_t i = 0; i < kMaxHedges; ++i) {
      failures_[i] = 0;
      bench_until_[i] = 0;
    }
    active_ = 0;
    halted_ = false;
    held_ = false;
    unavailable_ = false;
    state_ = State::Idle;
    blocked_ = false;
    blocked_ns_ = 0;
    derisking_ = false;
    derisk_open_ = false;
    derisk_id_ = ClientOrderId{};
    next_derisk_ns_ = 0;
    derisk_stuck_ = false;
  }

  // The hedge of `residual` on hedge instrument `i`, priced from its book.
  template <class Ctx>
  [[nodiscard]] std::optional<NewOrderRequest> order_on(const Ctx& ctx,
                                                        std::size_t i,
                                                        Qty residual) const noexcept {
    const Leg& h = hedges_[i];
    const auto& b = ctx.book(h.id);
    return hedge_order(ctx.instrument(h.id),
                       residual,
                       b.best_bid().price,
                       b.best_ask().price,
                       h.tolerance,
                       cfg_.tag);
  }
  // The venue's balance or margin covers `r` (StrategyContext::balance_room).
  template <class Ctx>
  [[nodiscard]] static bool covered(const Ctx& ctx, const NewOrderRequest& r) noexcept {
    return ctx.balance_room(r.instrument, r.side, r.price) >= r.qty;
  }

  template <class Ctx>
  [[nodiscard]] bool in_flight(const Ctx& ctx) const noexcept {
    if (derisk_open_) return true;
    for (std::size_t i = 0; i < n_hedges_; ++i) {
      const InstrumentId id = hedges_[i].id;
      if (ctx.open_qty(id, Side::Buy).is_positive() || ctx.open_qty(id, Side::Sell).is_positive())
        return true;
    }
    return false;
  }

  void hold_uncertain(const Order& o, std::int64_t now) noexcept {
    ++stats_.uncertain_ends;
    const std::int64_t until = now + cfg_.uncertain_hold.ns;
    if (until > next_ns_) next_ns_ = until;
    FASTMM_LOG_WARN(
        "{}: order {} ended without the venue saying how; next order in {} ms at the earliest",
        cfg_.name,
        encode_cl_ord_id(o.cl_ord_id),
        cfg_.uncertain_hold.millis());
  }

  template <class Ctx>
  void switch_to(const Ctx& ctx, std::size_t k, std::int64_t now) noexcept {
    const std::size_t from = active_;
    active_ = k;
    const InstrumentId a = hedges_[from].id;
    const InstrumentId b = hedges_[k].id;
    if (k > from) {
      ++stats_.failovers;
      const Reason r = why(ctx, from, now);
      FASTMM_LOG_WARN(
          "{}: hedging moves from {} (instrument {}) to {} (instrument {}): the first is {}",
          cfg_.name,
          ctx.instrument(a).symbol.view(),
          a.value,
          ctx.instrument(b).symbol.view(),
          b.value,
          r == Reason::Usable ? std::string_view{"short of balance"} : to_string(r));
    } else {
      FASTMM_LOG_WARN("{}: hedging back on {} (instrument {}) from {} (instrument {})",
                      cfg_.name,
                      ctx.instrument(b).symbol.view(),
                      b.value,
                      ctx.instrument(a).symbol.view(),
                      a.value);
    }
  }

  template <class Ctx>
  void failed(Ctx& ctx, std::size_t k, std::int64_t now) noexcept {
    ++stats_.hedge_failures;
    next_ns_ = now + cfg_.retry.ns;
    const auto n = static_cast<std::size_t>(cfg_.max_failures);
    std::size_t& count = failures_[k];
    fail_ns_[k][count % kMaxFailures] = now;
    ++count;
    if (halted_ || count < n) return;
    const std::int64_t oldest = fail_ns_[k][(count - n) % kMaxFailures];
    if (now - oldest > cfg_.failure_window.ns) return;
    bool other = false;
    for (std::size_t i = 0; i < n_hedges_; ++i) {
      if (i != k && now >= bench_until_[i]) other = true;
    }
    const std::string_view sym = ctx.instrument(hedges_[k].id).symbol.view();
    if (other) {
      bench_until_[k] = now + cfg_.bench.ns;
      count = 0;
      ++stats_.benches;
      FASTMM_LOG_WARN(
          "{}: {} hedges on {} (instrument {}) filled nothing within {} ms: it sits "
          "out {} ms",
          cfg_.name,
          n,
          sym,
          hedges_[k].id.value,
          cfg_.failure_window.millis(),
          cfg_.bench.millis());
      return;
    }
    halted_ = true;
    ++stats_.halts;
    FASTMM_LOG_ERROR(
        "{}: {} hedges on {} filled nothing within {} ms: hedging stopped with {} unhedged; "
        "restart to resume",
        cfg_.name,
        n,
        sym,
        cfg_.failure_window.millis(),
        residual(ctx));
  }

  // ---- de-risking ----------------------------------------------------------------------------

  // No hedge instrument took the residual: after derisk_after, reduce a source instead.
  template <class Ctx>
  void blocked(Ctx& ctx, Qty residual, std::int64_t now) noexcept {
    if (cfg_.derisk_after.ns <= 0 || residual.is_zero()) return;
    if (!blocked_) {
      blocked_ = true;
      blocked_ns_ = now;
    }
    if (now - blocked_ns_ < cfg_.derisk_after.ns) return;
    if (!derisking_) {
      derisking_ = true;
      derisk_stuck_ = false;
      ++stats_.derisk_episodes;
      FASTMM_LOG_WARN(
          "{}: no hedge instrument has taken {} for {} ms: de-risking the sources in steps of at "
          "most {}",
          cfg_.name,
          residual,
          cfg_.derisk_after.millis(),
          cfg_.derisk_step);
    }
    if (now < next_derisk_ns_) return;
    derisk_step(ctx, residual, now);
  }

  // A hedge instrument takes the residual, or there is none to take.
  void unblock(std::string_view why) noexcept {
    unavailable_ = false;
    blocked_ = false;
    if (!derisking_) return;
    derisking_ = false;
    FASTMM_LOG_WARN("{}: de-risking ends: {}", cfg_.name, why);
  }

  template <class Ctx>
  void derisk_step(Ctx& ctx, Qty residual, std::int64_t now) noexcept {
    const Side side = residual.is_positive() ? Side::Sell : Side::Buy;
    for (std::size_t i = 0; i < n_sources_; ++i) {
      const Leg& s = sources_[i];
      const Instrument& inst = ctx.instrument(s.id);
      const Qty pos = to_base(inst, ctx.position(s.id).qty);
      // Only a position on the residual's side is reduced.
      if (pos.is_zero() || pos.is_negative() != residual.is_negative()) continue;
      if (s.down != 0 || ctx.venue_killed(inst.venue)) continue;
      const auto& b = ctx.book(s.id);
      if (!b.is_valid()) continue;
      Qty base = residual.abs();
      if (pos.abs() < base) base = pos.abs();
      if (cfg_.derisk_step.is_positive() && cfg_.derisk_step < base) base = cfg_.derisk_step;
      std::optional<NewOrderRequest> r = hedge_order(inst,
                                                     side == Side::Sell ? base : -base,
                                                     b.best_bid().price,
                                                     b.best_ask().price,
                                                     cfg_.derisk_tolerance,
                                                     cfg_.derisk_tag);
      // A reduce-only derivative order passes the engine's balance check; spot sells need base.
      if (!r) continue;
      if (!inst.is_derivative() && ctx.balance_room(s.id, r->side, r->price) < r->qty) continue;
      NewOrderRequest req = *r;
      req.reduce_only = true;
      next_derisk_ns_ = now + cfg_.derisk_interval.ns;
      const auto sent = ctx.send(req);
      if (!sent) {
        FASTMM_LOG_WARN("{}: de-risk {} {} {} @ {} refused: {}",
                        cfg_.name,
                        inst.symbol.view(),
                        req.side,
                        req.qty,
                        req.price,
                        sent.error());
        return;
      }
      derisk_open_ = true;
      derisk_id_ = *sent;
      derisk_stuck_ = false;
      ++stats_.derisk_orders;
      return;
    }
    if (!derisk_stuck_) {
      derisk_stuck_ = true;
      FASTMM_LOG_WARN("{}: no source can take a de-risk order for {} now", cfg_.name, residual);
    }
  }

  Config cfg_{};
  std::array<Leg, kMaxSources> sources_{};
  std::array<Leg, kMaxHedges> hedges_{};
  std::array<VenueId, kMaxSources + kMaxHedges> venue_of_{};
  std::size_t n_sources_ = 0;
  std::size_t n_hedges_ = 0;
  Qty target_{};
  std::int64_t next_ns_ = 0;  // retry backoff or uncertain hold
  std::array<std::size_t, kMaxHedges> failures_{};
  std::array<std::array<std::int64_t, kMaxFailures>, kMaxHedges> fail_ns_{};
  std::array<std::int64_t, kMaxHedges> bench_until_{};
  std::size_t active_ = 0;
  Stats stats_{};
  State state_ = State::Idle;
  bool ready_ = false;
  bool halted_ = false;
  bool held_ = false;
  bool unavailable_ = false;  // logged once until a hedge instrument takes the residual again
  // de-risking
  bool blocked_ = false;
  bool derisking_ = false;
  bool derisk_open_ = false;
  bool derisk_stuck_ = false;
  std::int64_t blocked_ns_ = 0;
  std::int64_t next_derisk_ns_ = 0;
  ClientOrderId derisk_id_{};
};

}  // namespace fastmm
