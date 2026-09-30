#pragma once
// Xmm: quote on one venue, hedge on another (docs/how-to/strategies/xmm.md).
//
// Two instruments, named by their index in [[instruments]]: the quote instrument (maker quotes)
// and the hedge instrument (taker IOC orders), usually the same underlying on two venues.
//
//   ref    = hedge book mid (or microprice)
//   basis  = EWMA of (quote book mid - ref), half-life basis_halflife_s (0: no basis); with
//            mark_basis, the venues' premia instead: (quote mark - quote index) - (hedge mark -
//            hedge index), a leg that is not a perpetual counting 0
//   carry  = ref * (hedge funding - quote funding) over funding_horizon_s, each perpetual leg's
//            rate from ctx.funding (a spot leg pays none); 0 while funding_horizon_s is 0
//   fair   = ref + basis + carry
//   half   = fair * (edge + quote maker fee + hedge taker fee + slippage), the fees from
//            ctx.fees: [venues.<x>.fees], [[instruments]] overrides, or the account's own
//   bid    = fair - half rounded down, ask = fair + half rounded up, one level, never crossing the
//            quote venue's touch
//
// Hedging is a HedgeExecutor (strategies/hedge_executor.hpp) with the quote instrument as its
// source and the hedge instrument, then fallback_instrument when set, as its hedges:
//
//   unhedged = quote position * multiplier + hedge positions * multipliers   (base units)
//
// When |unhedged| rounds to at least one hedge lot (and min_qty and min_notional) and no hedge is
// open, one IOC limit goes out, priced hedge_tolerance_bps (fallback_tolerance_bps) through the
// touch; when it ends the positions are looked at again. Sizing from positions, the one hedge in
// flight, the uncertain hold, the retry and the halt after max_hedge_failures within
// failure_window_ms are the executor's, described there. With a fallback, the hedge goes to it
// while the hedge instrument's venue is down, killed or gated, its book is invalid or older than
// stale_ms, its balance cannot cover the hedge, or it failed max_hedge_failures times (benched for
// failover_bench_ms); it comes back to the hedge instrument as soon as that can take it. The
// strategy halts only when every hedge instrument failed. With derisk_after_ms, a residual no hedge
// instrument took for that long is reduced on the quote instrument instead: reduce-only IOC orders
// of at most derisk_step_qty, derisk_interval_ms apart.
//
// Guards: the quotes come off when either book the fair value needs is invalid or older than
// stale_ms, when the hedge instrument's venue is gated, when no hedge instrument can take a hedge
// (the executor's can_hedge), and while hedging is halted. The side that would take |unhedged| past
// max_unhedged is not quoted. `restart` set to a new value clears a halt.
//
// Balances (ctx.balance_room): a side whose fill the quote venue's balance cannot cover is not
// quoted. A hedge no hedge venue's balance or margin can cover is held (Stats::hedges_held, once
// per episode), and while it is held only the side that reduces |unhedged| is quoted, as at
// max_unhedged.
//
// Perpetual legs (ctx.mark, ctx.index, ctx.funding): a bid that fills is hedged by a sell, so
// holding it earns the hedge leg's funding and pays the quote leg's; carry prices that in over the
// expected holding time, the same shift for both sides. A mark, index or funding rate the strategy
// needs that is stale (or never came) pulls the quotes, as a stale book does. A PerpState of either
// instrument requotes.
//
// Linear contracts only (spot, linear perpetuals and futures); an inverse instrument leaves the
// strategy idle with an error at start. The instrument indices are read at start. The basis is
// tracked in double (EWMA of price differences) and rounded back to raw price units; everything
// else is fixed point. Every hook is noexcept and allocation-free.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fees.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/perp_book.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/core/venue_health.hpp"
#include "fastmm/strategies/hedge_executor.hpp"
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
  FASTMM_PARAM(bool,
               mark_basis,
               false,
               0,
               1,
               "basis from the venues' mark and index prices of the perpetual legs instead of the "
               "EWMA of the mids")
  FASTMM_PARAM(double,
               funding_horizon_s,
               0.0,
               0.0,
               2592000.0,
               "expected holding time of a position, seconds: the perpetual legs' funding over it "
               "shifts fair value (0 = off)")
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
  FASTMM_PARAM(int,
               fallback_instrument,
               -1,
               -1,
               255,
               "index in [[instruments]] of a second hedge instrument, used while the first cannot "
               "take the hedge (-1 = none; read at start)")
  FASTMM_PARAM_BPS(fallback_tolerance_bps,
                   5_bps,
                   0_bps,
                   1000_bps,
                   "hedge IOC limit on the fallback: this far through its touch")
  FASTMM_PARAM_MS(failover_bench_ms,
                  milliseconds(60000),
                  milliseconds(0),
                  milliseconds(86400000),
                  "a hedge instrument that failed max_hedge_failures times sits out this long "
                  "while the other can hedge")
  FASTMM_PARAM_MS(derisk_after_ms,
                  milliseconds(0),
                  milliseconds(0),
                  milliseconds(86400000),
                  "reduce the quote position when no hedge instrument took the residual for this "
                  "long (0 = never)")
  FASTMM_PARAM(
      Qty, derisk_step_qty, 0.001_qty, 0_qty, 1000000000_qty, "largest de-risk order, base units")
  FASTMM_PARAM_MS(derisk_interval_ms,
                  milliseconds(1000),
                  milliseconds(0),
                  milliseconds(3600000),
                  "wait between de-risk orders")
  FASTMM_PARAM_BPS(derisk_tolerance_bps,
                   10_bps,
                   0_bps,
                   1000_bps,
                   "de-risk IOC limit: this far through the quote venue's touch")

  std::optional<std::string> validate() const {
    if (quote_instrument == hedge_instrument)
      return "quote_instrument and hedge_instrument must differ";
    if (fallback_instrument == quote_instrument || fallback_instrument == hedge_instrument)
      return "fallback_instrument must differ from quote_instrument and hedge_instrument";
    if (!max_unhedged.is_zero() && max_unhedged < quote_qty)
      return "max_unhedged must be 0 or at least quote_qty";
    if (derisk_after_ms > Duration{} && !derisk_step_qty.is_positive())
      return "derisk_step_qty must be positive when derisk_after_ms is set";
    return std::nullopt;
  }
};

class Xmm : public StrategyBase<XmmParams> {
 public:
  static constexpr std::string_view name() noexcept { return "xmm"; }
  static constexpr std::uint64_t kTimer = 0x584d'4d54;      // "XMMT"
  static constexpr std::uint32_t kHedgeTag = 0x584d'4d48;   // "XMMH", outside the quote tag range
  static constexpr std::uint32_t kDeriskTag = 0x584d'4d44;  // "XMMD"
  static constexpr Duration kTimerPeriod = milliseconds(100);

  using Stats = HedgeExecutor::Stats;

  // ---- hooks --------------------------------------------------------------------------------

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    ready_ = false;
    have_basis_ = false;
    basis_raw_ = 0.0;
    basis_ns_ = 0;
    quoted_ = false;
    quoted_fair_ = Price{};
    hedge_.reset();
    const XmmParams& p = params();
    restart_seen_ = p.restart;
    const auto n = static_cast<std::int64_t>(ctx.instruments().size());
    if (p.quote_instrument >= n || p.hedge_instrument >= n || p.fallback_instrument >= n) {
      FASTMM_LOG_ERROR(
          "xmm: quote_instrument {}, hedge_instrument {} or fallback_instrument {} is not in the "
          "{} "
          "configured instruments; not trading",
          p.quote_instrument,
          p.hedge_instrument,
          p.fallback_instrument,
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
    static_cast<void>(hedge_.add_source(q_));
    static_cast<void>(hedge_.add_hedge(h_, p.hedge_tolerance_bps));
    if (p.fallback_instrument >= 0) {
      static_cast<void>(
          hedge_.add_hedge(InstrumentId{static_cast<std::uint32_t>(p.fallback_instrument)},
                           p.fallback_tolerance_bps));
    }
    if (!hedge_.start(ctx, hedge_config())) return;
    ready_ = true;
    timer_ = ctx.every(kTimerPeriod, kTimer);
  }

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book&) noexcept {
    if (!ready_ || (id != q_ && !hedge_.is_hedge(id))) return;
    if (id == q_ || id == h_) update_basis(ctx);
    hedge_.on_book(ctx, id);
    requote(ctx, false);
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const Fill& fill) noexcept {
    if (!ready_ || !hedge_.on_fill(ctx, fill)) return;
    requote(ctx, true);
  }

  template <class Ctx>
  void on_order_update(Ctx& ctx, const OmsUpdate& u) noexcept {
    if (!ready_ || !hedge_.on_order_update(ctx, u)) return;
    requote(ctx, true);
  }

  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t tag) noexcept {
    if (!ready_ || tag != kTimer) return;
    hedge_.on_timer(ctx);
    requote(ctx, false);
  }

  template <class Ctx>
  void on_connection(Ctx& ctx, const ConnectionStateMsg& m) noexcept {
    if (!ready_) return;
    hedge_.on_connection(ctx, m);
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
    hedge_.update(ctx);
    requote(ctx, true);
  }

  // A venue's mark, index or funding of either instrument: fair value may have moved.
  template <class Ctx>
  void on_perp_state(Ctx& ctx, InstrumentId id, const PerpStateMsg&) noexcept {
    if (!ready_ || (id != q_ && id != h_)) return;
    if (!params().mark_basis && !(params().funding_horizon_s > 0.0)) return;
    requote(ctx, false);
  }

  // A venue reported a balance: a held hedge may fit now, and a side may be quotable again.
  template <class Ctx>
  void on_balance(Ctx& ctx, const BalanceMsg& m) noexcept {
    if (!ready_ || !hedge_.on_balance(ctx, m)) return;
    requote(ctx, true);
  }

  // New parameters reach the executor. A new value of `restart` clears a halt. (Reconciliations
  // pause and resume quoting on their own, so on_quoting cannot tell an operator's resume from
  // theirs, and a publisher may repeat unchanged parameters.)
  template <class Ctx>
  void on_params(Ctx& ctx) noexcept {
    if (!ready_) return;
    hedge_.set_config(hedge_config());
    hedge_.set_tolerance(0, params().hedge_tolerance_bps);
    hedge_.set_tolerance(1, params().fallback_tolerance_bps);
    if (params().restart != restart_seen_) {
      restart_seen_ = params().restart;
      if (hedge_.halted()) {
        FASTMM_LOG_WARN("xmm: restart={}; hedging and quoting restart", params().restart);
        hedge_.restart();
      }
    }
    hedge_.update(ctx);
    requote(ctx, true);
  }

  // ---- pure functions (deterministic tests) ----------------------------------------------------

  [[nodiscard]] static constexpr Qty to_base(const Instrument& inst, Qty contracts) noexcept {
    return HedgeExecutor::to_base(inst, contracts);
  }
  [[nodiscard]] static constexpr Qty to_contracts(const Instrument& inst, Qty base) noexcept {
    return HedgeExecutor::to_contracts(inst, base);
  }

  // The hedge that brings `unhedged` (base units) back towards zero (HedgeExecutor::hedge_order).
  [[nodiscard]] static std::optional<NewOrderRequest> hedge_order(const Instrument& hi,
                                                                  Qty unhedged,
                                                                  Price best_bid,
                                                                  Price best_ask,
                                                                  Ratio tolerance) noexcept {
    return HedgeExecutor::hedge_order(hi, unhedged, best_bid, best_ask, tolerance, kHedgeTag);
  }

  // One level each side around `fair`; a side is left out when a fill of it would take |unhedged|
  // (base units) past max_unhedged.
  [[nodiscard]] DesiredQuotes compute_quotes(const Instrument& qi,
                                             Price fair,
                                             Qty unhedged,
                                             Ratio fees) const noexcept {
    const XmmParams& p = params();
    DesiredQuotes q;
    if (!fair.is_positive()) return q;
    const Qty qty = qi.round_qty(to_contracts(qi, p.quote_qty));
    if (!qty.is_positive() || qty < qi.min_qty) return q;
    const Qty size = to_base(qi, qty);
    const Ratio width = p.edge_bps + fees + p.slippage_bps;
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
  [[nodiscard]] bool halted() const noexcept { return hedge_.halted(); }
  [[nodiscard]] bool hedge_held() const noexcept { return hedge_.held(); }
  [[nodiscard]] bool have_basis() const noexcept { return have_basis_; }
  [[nodiscard]] Price basis() const noexcept {
    return Price::from_raw(static_cast<std::int64_t>(std::llround(basis_raw_)));
  }
  [[nodiscard]] const Stats& stats() const noexcept { return hedge_.stats(); }
  [[nodiscard]] const HedgeExecutor& hedger() const noexcept { return hedge_; }
  [[nodiscard]] InstrumentId quote_id() const noexcept { return q_; }
  [[nodiscard]] InstrumentId hedge_id() const noexcept { return h_; }

  // Quote position plus hedge positions in base units.
  template <class Ctx>
  [[nodiscard]] Qty unhedged(const Ctx& ctx) const noexcept {
    return hedge_.residual(ctx);
  }

  // The hedge reference plus the basis and the funding carry; zero while one of them is unknown
  // or stale.
  template <class Ctx>
  [[nodiscard]] Price fair_value(const Ctx& ctx) const noexcept {
    const Price ref = reference(ctx.book(h_));
    if (!ref.is_positive()) return Price{};
    Price fair = ref;
    if (params().mark_basis) {
      Price quote_premium;
      Price hedge_premium;
      if (!premium(ctx, q_, quote_premium) || !premium(ctx, h_, hedge_premium)) return Price{};
      fair += quote_premium - hedge_premium;
    } else {
      if (!have_basis_) return Price{};
      fair += basis();
    }
    if (params().funding_horizon_s > 0.0) {
      double quote_rate = 0.0;
      double hedge_rate = 0.0;
      if (!funding_over(ctx, q_, quote_rate) || !funding_over(ctx, h_, hedge_rate)) return Price{};
      fair += carry(ref, hedge_rate, quote_rate);
    }
    return fair;
  }

  // What holding the hedged pair for the horizon earns per unit, in price: a bid that fills is
  // hedged by a sell, which receives the hedge leg's funding (positive rate: longs pay shorts) and
  // pays the quote leg's. `hedge_rate` and `quote_rate` are the rates over the horizon.
  [[nodiscard]] static Price carry(Price ref, double hedge_rate, double quote_rate) noexcept {
    return Price::from_raw(static_cast<std::int64_t>(
        std::llround(static_cast<double>(ref.raw) * (hedge_rate - quote_rate))));
  }

 private:
  [[nodiscard]] HedgeExecutor::Config hedge_config() const noexcept {
    const XmmParams& p = params();
    HedgeExecutor::Config c;
    c.name = "xmm";
    c.tag = kHedgeTag;
    c.derisk_tag = kDeriskTag;
    c.retry = p.hedge_retry_ms;
    c.max_failures = p.max_hedge_failures;
    c.failure_window = p.failure_window_ms;
    c.uncertain_hold = p.uncertain_hold_ms;
    c.stale = p.stale_ms;
    c.bench = p.failover_bench_ms;
    c.derisk_after = p.derisk_after_ms;
    c.derisk_step = p.derisk_step_qty;
    c.derisk_interval = p.derisk_interval_ms;
    c.derisk_tolerance = p.derisk_tolerance_bps;
    return c;
  }

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

  // A perpetual leg's premium, mark - index, from its venue; 0 for another leg. False when a mark
  // or index it needs is stale or unknown.
  template <class Ctx>
  [[nodiscard]] static bool premium(const Ctx& ctx, InstrumentId id, Price& out) noexcept {
    out = Price{};
    if (ctx.instrument(id).asset_class != AssetClass::Perpetual) return true;
    const RefPrice mark = ctx.mark(id);
    const RefPrice index = ctx.index(id);
    if (!mark.usable() || !index.usable()) return false;
    out = mark.price - index.price;
    return true;
  }
  // A perpetual leg's funding rate over funding_horizon_s; 0 for another leg. False when the rate
  // is stale or unknown.
  template <class Ctx>
  [[nodiscard]] bool funding_over(const Ctx& ctx, InstrumentId id, double& out) const noexcept {
    out = 0.0;
    if (ctx.instrument(id).asset_class != AssetClass::Perpetual) return true;
    const FundingView f = ctx.funding(id);
    if (!f.usable()) return false;
    out = f.over(Duration{std::llround(params().funding_horizon_s * 1e9)});
    return true;
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
  void pull(Ctx& ctx) noexcept {
    if (quoted_) ctx.pull_quotes(q_);
    quoted_ = false;
    quoted_fair_ = Price{};
  }

  template <class Ctx>
  void requote(Ctx& ctx, bool force) noexcept {
    const auto& qb = ctx.book(q_);
    // Off while no hedge instrument can take a fill's hedge (or a de-risk order may be working),
    // and while the fair value's inputs are missing or late.
    if (!hedge_.can_hedge(ctx) || !qb.is_valid() || stale(ctx, q_) || stale(ctx, h_) ||
        ctx.venue_health(ctx.instrument(h_).venue).gated) {
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
    const Ratio fees = cbps_ratio(ctx.fees(q_).maker_cbps) + cbps_ratio(ctx.fees(h_).taker_cbps);
    const Qty open = unhedged(ctx);
    DesiredQuotes q = compute_quotes(qi, fair, open, fees);
    keep_passive(q, qb.best_bid().price, qb.best_ask().price, qi.tick);
    // A held hedge: only the side that brings |unhedged| back is quoted.
    if (hedge_.held() && open.is_positive()) q.bids.clear();
    if (hedge_.held() && open.is_negative()) q.asks.clear();
    // A side the quote venue's balance cannot cover in full is not quoted; our resting quote on
    // that side counts, it is replaced.
    for (const Side side : {Side::Buy, Side::Sell}) {
      auto& levels = side == Side::Buy ? q.bids : q.asks;
      if (levels.empty()) continue;
      const Qty room = ctx.balance_room(q_, side, levels[0].price);
      if (room != Qty::max() && room + ctx.open_qty(q_, side) < levels[0].qty) levels.clear();
    }
    if (ctx.set_quotes(q_, q)) {
      quoted_ = true;
      quoted_fair_ = fair;
    } else {
      quoted_ = false;
      quoted_fair_ = Price{};  // ignored while quoting is disabled; on_quoting requotes
    }
  }

  // A fee rate in centi-bps (FeeRates) as a Ratio.
  [[nodiscard]] static constexpr Ratio cbps_ratio(std::int32_t cbps) noexcept {
    return Ratio::from_raw(static_cast<std::int64_t>(cbps) * (kRatioPerBp / 100));
  }

  HedgeExecutor hedge_;
  InstrumentId q_{};
  InstrumentId h_{};
  TimerId timer_{};
  double basis_raw_ = 0.0;  // quote mid - hedge reference, raw price units
  std::int64_t basis_ns_ = 0;
  Price quoted_fair_{};
  bool ready_ = false;
  bool have_basis_ = false;
  bool quoted_ = false;
  int restart_seen_ = 0;
};

static_assert(StrategyLike<Xmm>);
static_assert(verify_strategy<Xmm>());

}  // namespace fastmm
