#pragma once
// Xmm: quote on one venue, hedge on another (docs/how-to/strategies/xmm.md).
//
// Two instruments, named by their index in [[instruments]]: the quote instrument (maker quotes)
// and the hedge instrument (taker IOC orders), usually the same underlying on two venues.
//
//   ref    = hedge book mid (or microprice)
//   basis  = EWMA of (quote book mid - ref), half-life basis_halflife_s (0: no basis)
//   fair   = ref + basis
//   half   = fair * (edge + quote_fee + hedge_fee + slippage)
//   bid    = fair - half rounded down, ask = fair + half rounded up, one level, never crossing the
//            quote venue's touch
//
// Hedging is derived from positions, never from a count of fills:
//
//   unhedged = quote position * multiplier + hedge position * multiplier   (base units)
//
// When |unhedged| rounds to at least one hedge lot (and min_qty) and no order is open on the hedge
// instrument, one IOC limit goes out: -unhedged / hedge multiplier rounded down to the lot, priced
// hedge_tolerance_bps through the hedge touch. When it ends the strategy looks at the positions
// again. A restart, a replayed or duplicated fill and an order whose outcome arrives late all end
// in the same place, and an order the OMS still holds (sent, not acknowledged, venue down) blocks
// the next hedge until an ack, a fill or reconciliation ends it. A hedge the venue reports ended
// before its executions arrive (Bybit's order and execution topics are not ordered) is booked by
// the engine from the reported cumulative quantity before this strategy hears of the end, and the
// executions that follow name that quantity instead of adding it (Oms::on_fill).
//
// Guards: the quotes come off when either book is invalid or older than stale_ms, when the hedge
// venue's market data or order channel is down, and while the strategy is halted. The side that
// would take |unhedged| past max_unhedged is not quoted. A hedge that ends with nothing filled is a
// failure: the next one waits hedge_retry_ms, and max_hedge_failures within failure_window_ms halt
// the strategy (quotes pulled, no more hedges, logged) until `restart` gets a new value. A hedge
// the venue never reported on (cancelled by the ack timeout, dropped by reconciliation with
// quantity unaccounted for, or a generic VenueReject such as a REST timeout) holds hedging for
// uncertain_hold_ms, so a fill that is still on its way is booked before the positions are
// trusted again.
//
// Linear contracts only (spot, linear perpetuals and futures); an inverse instrument leaves the
// strategy idle with an error at start. The instrument indices are read at start. The basis is
// tracked in double (EWMA of price differences) and rounded back to raw price units; everything
// else is fixed point. Every hook is noexcept and allocation-free.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/quoting.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>

namespace fastmm {

struct XmmParams {
  FASTMM_PARAMS(XmmParams)
  FASTMM_PARAM(int,
               quote_instrument,
               0,
               0,
               255,
               "index in [[instruments]] of the quoted instrument (read at start)")
  FASTMM_PARAM(int,
               hedge_instrument,
               1,
               0,
               255,
               "index in [[instruments]] of the hedge instrument (read at start)")
  FASTMM_PARAM(Qty, quote_qty, 0.001_qty, 0_qty, 1000000000_qty, "size per side, base units")
  FASTMM_PARAM_BPS(edge_bps, 2_bps, 0_bps, 10000_bps, "margin kept per round trip, basis points")
  FASTMM_PARAM_BPS(
      quote_fee_bps, 0_bps, -(100_bps), 1000_bps, "maker fee on the quote venue (negative: rebate)")
  FASTMM_PARAM_BPS(hedge_fee_bps, 4_bps, 0_bps, 1000_bps, "taker fee on the hedge venue")
  FASTMM_PARAM_BPS(slippage_bps, 1_bps, 0_bps, 1000_bps, "expected hedge slippage, priced in")
  FASTMM_PARAM_BPS(hedge_tolerance_bps,
                   5_bps,
                   0_bps,
                   1000_bps,
                   "hedge IOC limit: this far through the hedge venue's touch")
  FASTMM_PARAM(double,
               basis_halflife_s,
               60.0,
               0.0,
               86400.0,
               "half-life of the basis EWMA, seconds (0 = no basis)")
  FASTMM_PARAM(bool, use_microprice, false, 0, 1, "hedge reference is the touch microprice")
  FASTMM_PARAM(Qty,
               max_unhedged,
               0.005_qty,
               0_qty,
               1000000000_qty,
               "do not quote a side that would take |unhedged| past this, base units (0 = no cap)")
  FASTMM_PARAM(
      int, requote_threshold_ticks, 1, 0, 1000000, "ignore fair value moves smaller than this")
  FASTMM_PARAM_MS(stale_ms,
                  milliseconds(2000),
                  milliseconds(0),
                  milliseconds(3600000),
                  "pull quotes when either book is older than this (0 = never)")
  FASTMM_PARAM_MS(hedge_retry_ms,
                  milliseconds(200),
                  milliseconds(0),
                  milliseconds(60000),
                  "wait after a hedge that filled nothing")
  FASTMM_PARAM(int,
               max_hedge_failures,
               5,
               1,
               16,
               "hedges that fill nothing within failure_window_ms before the strategy halts")
  FASTMM_PARAM_MS(failure_window_ms,
                  milliseconds(60000),
                  milliseconds(1),
                  milliseconds(86400000),
                  "window for max_hedge_failures")
  FASTMM_PARAM_MS(uncertain_hold_ms,
                  milliseconds(5000),
                  milliseconds(0),
                  milliseconds(600000),
                  "after a hedge with an unreported outcome, wait this long before the next")
  FASTMM_PARAM(int, restart, 0, 0, 1000000000, "set to a new value to clear a halt")

  std::optional<std::string> validate() const {
    if (quote_instrument == hedge_instrument)
      return "quote_instrument and hedge_instrument must differ";
    if (!max_unhedged.is_zero() && max_unhedged < quote_qty)
      return "max_unhedged must be 0 or at least quote_qty";
    return std::nullopt;
  }
};

class Xmm : public StrategyBase<XmmParams> {
 public:
  static constexpr std::string_view name() noexcept { return "xmm"; }
  static constexpr std::uint64_t kTimer = 0x584d'4d54;     // "XMMT"
  static constexpr std::uint32_t kHedgeTag = 0x584d'4d48;  // "XMMH", outside the quote tag range
  static constexpr Duration kTimerPeriod = milliseconds(100);

  struct Stats {
    std::uint64_t hedges_sent = 0;
    std::uint64_t hedge_failures = 0;  // ended with nothing filled, or refused by the engine
    std::uint64_t uncertain_ends = 0;  // ended without the venue saying how
    std::uint64_t halts = 0;
  };

  // ---- hooks --------------------------------------------------------------------------------

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    ready_ = false;
    have_basis_ = false;
    basis_raw_ = 0.0;
    basis_ns_ = 0;
    hedge_down_mask_ = 0;
    next_hedge_ns_ = 0;
    failures_ = 0;
    halted_ = false;
    quoted_ = false;
    quoted_fair_ = Price{};
    const XmmParams& p = params();
    const auto n = static_cast<std::int64_t>(ctx.instruments().size());
    if (p.quote_instrument >= n || p.hedge_instrument >= n) {
      FASTMM_LOG_ERROR(
          "xmm: quote_instrument {} or hedge_instrument {} is not in the {} configured "
          "instruments; not trading",
          p.quote_instrument,
          p.hedge_instrument,
          n);
      return;
    }
    q_ = InstrumentId{static_cast<std::uint32_t>(p.quote_instrument)};
    h_ = InstrumentId{static_cast<std::uint32_t>(p.hedge_instrument)};
    const Instrument& qi = ctx.instrument(q_);
    const Instrument& hi = ctx.instrument(h_);
    if (qi.inverse() || hi.inverse() || !qi.contract_multiplier.is_positive() ||
        !hi.contract_multiplier.is_positive()) {
      FASTMM_LOG_ERROR("xmm: {} and {} must both be linear contracts; not trading",
                       qi.symbol.view(),
                       hi.symbol.view());
      return;
    }
    ready_ = true;
    timer_ = ctx.every(kTimerPeriod, kTimer);
  }

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book&) noexcept {
    if (!ready_ || (id != q_ && id != h_)) return;
    update_basis(ctx);
    if (id == h_) maybe_hedge(ctx);
    requote(ctx, false);
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const Fill& fill) noexcept {
    if (!ready_ || (fill.instrument != q_ && fill.instrument != h_)) return;
    maybe_hedge(ctx);
    requote(ctx, true);
  }

  template <class Ctx>
  void on_order_update(Ctx& ctx, const OmsUpdate& u) noexcept {
    if (!ready_ || u.order.instrument != h_ || !u.terminal) return;
    const Order& o = u.order;
    const std::int64_t now = ctx.now().ns;
    if (o.cum_qty.is_zero()) hedge_failed(ctx, now);
    // The venue did not say how it ended: the ack timeout cancelled an order the venue never
    // acknowledged, reconciliation found it gone with quantity unaccounted for, or a connector
    // turned a request whose outcome it does not know into a generic reject (a REST timeout). A
    // fill may still be on its way, so the positions are not trusted for a while.
    const bool uncertain =
        u.unresolved_qty.is_positive() ||
        (o.state == OrderState::Canceled && o.venue_order_id.empty() && o.cum_qty.is_zero()) ||
        (o.state == OrderState::Rejected && o.reject_reason == RejectReason::VenueReject);
    if (uncertain) {
      ++stats_.uncertain_ends;
      const std::int64_t until = now + params().uncertain_hold_ms.ns;
      if (until > next_hedge_ns_) next_hedge_ns_ = until;
      FASTMM_LOG_WARN(
          "xmm: hedge {} ended without the venue saying how; next hedge in {} ms at the earliest",
          encode_cl_ord_id(o.cl_ord_id),
          params().uncertain_hold_ms.millis());
    }
    maybe_hedge(ctx);
    requote(ctx, true);
  }

  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t tag) noexcept {
    if (!ready_ || tag != kTimer) return;
    maybe_hedge(ctx);
    requote(ctx, false);
  }

  template <class Ctx>
  void on_connection(Ctx& ctx, const ConnectionStateMsg& m) noexcept {
    if (!ready_) return;
    if (m.hdr.venue == ctx.instrument(h_).venue) {
      const std::uint32_t bit = 1U << (m.channel & 31U);
      if (m.state == ConnState::Live) {
        hedge_down_mask_ &= ~bit;
      } else {
        hedge_down_mask_ |= bit;
      }
    }
    // The engine pulls the quotes of a venue that drops; requote from scratch either way.
    quoted_fair_ = Price{};
    requote(ctx, true);
  }

  template <class Ctx>
  void on_quoting(Ctx& ctx, bool enabled) noexcept {
    if (!ready_) return;
    quoted_ = false;
    quoted_fair_ = Price{};
    if (!enabled) return;
    maybe_hedge(ctx);
    requote(ctx, true);
  }

  // A new value of `restart` clears a halt. (Reconciliations pause and resume quoting on their
  // own, so on_quoting cannot tell an operator's resume from theirs, and a publisher may repeat
  // unchanged parameters.)
  template <class Ctx>
  void on_params(Ctx& ctx) noexcept {
    if (!ready_) return;
    if (halted_ && params().restart != restart_at_halt_) {
      FASTMM_LOG_WARN("xmm: restart={}; hedging and quoting restart", params().restart);
      halted_ = false;
      failures_ = 0;
      next_hedge_ns_ = 0;
    }
    maybe_hedge(ctx);
    requote(ctx, true);
  }

  // ---- pure functions (deterministic tests) ----------------------------------------------------

  // Contracts to base units and back (linear: qty * multiplier).
  [[nodiscard]] static constexpr Qty to_base(const Instrument& inst, Qty contracts) noexcept {
    return Qty::from_raw(mul_raw(contracts, inst.contract_multiplier));
  }
  [[nodiscard]] static constexpr Qty to_contracts(const Instrument& inst, Qty base) noexcept {
    if (!inst.contract_multiplier.is_positive()) return Qty{};
    return Qty::from_raw(detail::mul_div(base.raw, kFixedScale, inst.contract_multiplier.raw));
  }

  // The hedge that brings `unhedged` (base units) back towards zero: an IOC on the hedge
  // instrument, its quantity rounded down to the lot and capped at max_qty, priced `tolerance`
  // through the touch and rounded towards the touch. None when it rounds below the lot or min_qty
  // or the touch it needs is empty.
  [[nodiscard]] static std::optional<NewOrderRequest> hedge_order(const Instrument& hi,
                                                                  Qty unhedged,
                                                                  Price best_bid,
                                                                  Price best_ask,
                                                                  Ratio tolerance) noexcept {
    if (unhedged.is_zero()) return std::nullopt;
    const Side side = unhedged.is_negative() ? Side::Buy : Side::Sell;
    Qty qty = hi.round_qty(to_contracts(hi, unhedged.abs()));
    if (hi.max_qty.is_positive() && qty > hi.max_qty) qty = hi.round_qty(hi.max_qty);
    if (!qty.is_positive() || qty < hi.min_qty) return std::nullopt;
    Price px;
    if (side == Side::Buy) {
      if (!best_ask.is_positive()) return std::nullopt;
      px = hi.round_price(best_ask + best_ask * tolerance, Side::Buy);
    } else {
      if (!best_bid.is_positive()) return std::nullopt;
      px = hi.round_price(best_bid - best_bid * tolerance, Side::Sell);
    }
    if (!px.is_positive()) return std::nullopt;
    return NewOrderRequest::limit(hi.id, side, px, qty).ioc().tag(kHedgeTag);
  }

  // One level each side around `fair`; a side is left out when a fill of it would take |unhedged|
  // (base units) past max_unhedged.
  [[nodiscard]] DesiredQuotes compute_quotes(const Instrument& qi,
                                             Price fair,
                                             Qty unhedged) const noexcept {
    const XmmParams& p = params();
    DesiredQuotes q;
    if (!fair.is_positive()) return q;
    const Qty qty = qi.round_qty(to_contracts(qi, p.quote_qty));
    if (!qty.is_positive() || qty < qi.min_qty) return q;
    const Qty size = to_base(qi, qty);
    const Ratio width = p.edge_bps + p.quote_fee_bps + p.hedge_fee_bps + p.slippage_bps;
    const Price half = fair * width;
    const Qty cap = p.max_unhedged;
    const bool can_buy = cap.is_zero() || unhedged + size <= cap;
    const bool can_sell = cap.is_zero() || unhedged - size >= -cap;
    if (can_buy) q.bid(qi.round_price(fair - half, Side::Buy), qty);
    if (can_sell) q.ask(qi.round_price(fair + half, Side::Sell), qty);
    q.uncross(qi.tick);
    return q;
  }

  // ---- state (tests, diagnostics) ----------------------------------------------------------

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] bool halted() const noexcept { return halted_; }
  [[nodiscard]] bool have_basis() const noexcept { return have_basis_; }
  [[nodiscard]] Price basis() const noexcept {
    return Price::from_raw(static_cast<std::int64_t>(std::llround(basis_raw_)));
  }
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  [[nodiscard]] InstrumentId quote_id() const noexcept { return q_; }
  [[nodiscard]] InstrumentId hedge_id() const noexcept { return h_; }

  // Quote position plus hedge position in base units.
  template <class Ctx>
  [[nodiscard]] Qty unhedged(const Ctx& ctx) const noexcept {
    return to_base(ctx.instrument(q_), ctx.position(q_).qty) +
           to_base(ctx.instrument(h_), ctx.position(h_).qty);
  }

  // The hedge reference plus the basis; zero while either is unknown.
  template <class Ctx>
  [[nodiscard]] Price fair_value(const Ctx& ctx) const noexcept {
    const Price ref = reference(ctx.book(h_));
    if (!ref.is_positive() || !have_basis_) return Price{};
    return ref + basis();
  }

 private:
  template <class Book>
  [[nodiscard]] Price reference(const Book& b) const noexcept {
    if (!b.is_valid()) return Price{};
    return params().use_microprice ? microprice(b.best_bid(), b.best_ask()) : b.mid();
  }

  template <class Ctx>
  [[nodiscard]] bool stale(const Ctx& ctx, InstrumentId id) const noexcept {
    const Duration limit = params().stale_ms;
    if (limit <= Duration{}) return false;
    const Timestamp t = ctx.book(id).last_update();
    return t.valid() && ctx.now() - t > limit;
  }

  template <class Ctx>
  [[nodiscard]] bool hedge_in_flight(const Ctx& ctx) const noexcept {
    return ctx.open_qty(h_, Side::Buy).is_positive() || ctx.open_qty(h_, Side::Sell).is_positive();
  }

  template <class Ctx>
  void update_basis(Ctx& ctx) noexcept {
    const auto& qb = ctx.book(q_);
    const Price ref = reference(ctx.book(h_));
    if (!qb.is_valid() || !ref.is_positive()) return;
    const double halflife = params().basis_halflife_s;
    if (!(halflife > 0.0)) {
      basis_raw_ = 0.0;
      have_basis_ = true;
      return;
    }
    const auto sample = static_cast<double>(qb.mid().raw - ref.raw);
    const std::int64_t now = ctx.now().ns;
    if (!have_basis_ || now <= basis_ns_) {
      if (!have_basis_) basis_raw_ = sample;
    } else {
      const double dt = static_cast<double>(now - basis_ns_) / 1e9;
      const double alpha = 1.0 - std::exp(-dt * std::numbers::ln2 / halflife);
      basis_raw_ += alpha * (sample - basis_raw_);
    }
    basis_ns_ = now;
    have_basis_ = true;
  }

  template <class Ctx>
  void maybe_hedge(Ctx& ctx) noexcept {
    if (halted_ || hedge_down_mask_ != 0 || hedge_in_flight(ctx)) return;
    const std::int64_t now = ctx.now().ns;
    if (now < next_hedge_ns_) return;
    const auto& hb = ctx.book(h_);
    if (!hb.is_valid()) return;
    const std::optional<NewOrderRequest> req = hedge_order(ctx.instrument(h_),
                                                           unhedged(ctx),
                                                           hb.best_bid().price,
                                                           hb.best_ask().price,
                                                           params().hedge_tolerance_bps);
    if (!req) return;
    const auto sent = ctx.send(*req);
    if (!sent) {
      FASTMM_LOG_WARN(
          "xmm: hedge {} {} @ {} refused: {}", req->side, req->qty, req->price, sent.error());
      hedge_failed(ctx, now);
      return;
    }
    ++stats_.hedges_sent;
  }

  template <class Ctx>
  void hedge_failed(Ctx& ctx, std::int64_t now) noexcept {
    ++stats_.hedge_failures;
    next_hedge_ns_ = now + params().hedge_retry_ms.ns;
    const auto n = static_cast<std::size_t>(params().max_hedge_failures);
    failure_ns_[failures_ % kMaxFailures] = now;
    ++failures_;
    if (halted_ || failures_ < n) return;
    const std::int64_t oldest = failure_ns_[(failures_ - n) % kMaxFailures];
    if (now - oldest > params().failure_window_ms.ns) return;
    halted_ = true;
    restart_at_halt_ = params().restart;
    ++stats_.halts;
    FASTMM_LOG_ERROR(
        "xmm: {} hedges filled nothing within {} ms: quotes pulled and hedging stopped with {} "
        "unhedged; set restart to a new value to resume",
        n,
        params().failure_window_ms.millis(),
        unhedged(ctx));
    pull(ctx);
  }

  template <class Ctx>
  void pull(Ctx& ctx) noexcept {
    if (quoted_) ctx.pull_quotes(q_);
    quoted_ = false;
    quoted_fair_ = Price{};
  }

  template <class Ctx>
  void requote(Ctx& ctx, bool force) noexcept {
    const auto& qb = ctx.book(q_);
    if (halted_ || hedge_down_mask_ != 0 || !qb.is_valid() || stale(ctx, q_) || stale(ctx, h_)) {
      pull(ctx);
      return;
    }
    const Price fair = fair_value(ctx);
    if (!fair.is_positive()) {
      pull(ctx);
      return;
    }
    const Instrument& qi = ctx.instrument(q_);
    if (!force && quoted_ && quoted_fair_.is_positive() &&
        (fair - quoted_fair_).abs() < qi.ticks(params().requote_threshold_ticks))
      return;
    DesiredQuotes q = compute_quotes(qi, fair, unhedged(ctx));
    keep_passive(q, qb.best_bid().price, qb.best_ask().price, qi.tick);
    if (ctx.set_quotes(q_, q)) {
      quoted_ = true;
      quoted_fair_ = fair;
    } else {
      quoted_ = false;
      quoted_fair_ = Price{};  // ignored while quoting is disabled; on_quoting requotes
    }
  }

  static constexpr std::size_t kMaxFailures = 16;

  InstrumentId q_{};
  InstrumentId h_{};
  TimerId timer_{};
  double basis_raw_ = 0.0;  // quote mid - hedge reference, raw price units
  std::int64_t basis_ns_ = 0;
  std::int64_t next_hedge_ns_ = 0;
  std::int64_t failure_ns_[kMaxFailures] = {};
  std::size_t failures_ = 0;
  std::uint32_t hedge_down_mask_ = 0;  // bit per channel of the hedge venue that is not Live
  Price quoted_fair_{};
  Stats stats_{};
  bool ready_ = false;
  bool have_basis_ = false;
  bool halted_ = false;
  bool quoted_ = false;
  int restart_at_halt_ = 0;
};

static_assert(StrategyLike<Xmm>);
static_assert(verify_strategy<Xmm>());

}  // namespace fastmm
