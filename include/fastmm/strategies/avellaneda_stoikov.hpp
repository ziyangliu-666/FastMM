#pragma once
// Avellaneda-Stoikov (2008) market making (8.6).
//
//   reservation  r = m - q * gamma * sigma^2 * tau
//   half spread  delta = gamma * sigma^2 * tau / 2 + (1 / gamma) * ln(1 + gamma / kappa)
//   quotes       bid = r - delta, ask = r + delta   (rounded to tick, floored at min ticks)
//
// where q is inventory in units of quote_qty, sigma^2 the EWMA variance of mid changes per
// second (price units), tau the time left in the horizon (seconds; 1 for infinite horizon)
// and kappa the order-arrival decay (fixed or estimated as 1 / EWMA|trade - mid|).
//
// DOCUMENTED EXCEPTION to the no-double rule: the formula above is evaluated in double on
// an already-converted mid, then rounded back to the tick grid. This is the only place a
// strategy uses floating point; it never touches Price/Qty arithmetic elsewhere.
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <cmath>
#include <cstdint>
#include <string_view>

namespace fastmm {

struct AvellanedaStoikovParams {
  FASTMM_PARAMS(AvellanedaStoikovParams)
  FASTMM_PARAM(double, gamma, 0.1, 1e-6, 100.0, "risk aversion")
  FASTMM_PARAM(double, kappa, 1.5, 1e-6, 1e6, "order arrival decay (per price unit)")
  FASTMM_PARAM(bool, estimate_kappa, false, 0, 1, "estimate kappa from trade distances to mid")
  FASTMM_PARAM(double, sigma_window_s, 60.0, 0.1, 86400.0, "EWMA window for mid variance, seconds")
  FASTMM_PARAM(double, horizon_s, 60.0, 0.1, 86400.0, "quoting horizon T, seconds (rolling)")
  FASTMM_PARAM(bool, infinite_horizon, true, 0, 1, "use tau = 1 instead of the rolling horizon")
  FASTMM_PARAM(double, quote_qty, 0.01, 0.0, 1e9, "quantity per side (base units)")
  FASTMM_PARAM(double, max_inventory, 0.1, 0.0, 1e9, "stop quoting the side that would exceed this")
  FASTMM_PARAM(int, min_half_spread_ticks, 1, 0, 1000000, "floor for the half spread")
  FASTMM_PARAM(int, requote_threshold_ticks, 1, 0, 1000000, "ignore mid moves smaller than this")
  FASTMM_PARAM(
      double, sigma_init, 0.0, 0.0, 1e12, "initial sigma (price units / sqrt(s)); 0 = 1 tick")
};

class AvellanedaStoikov : public StrategyBase<AvellanedaStoikovParams> {
 public:
  static constexpr std::string_view name() noexcept { return "avellaneda_stoikov"; }

  struct State {
    double var = 0.0;        // EWMA variance per second (price units^2)
    double mean_dist = 0.0;  // EWMA |trade px - mid| for kappa estimation
    double kappa_est = 0.0;
    Price last_mid{};    // previous mid sample for the variance estimate (every book update)
    Price quoted_mid{};  // mid of the last requote; cleared to force the next one
    Timestamp last_ts{};
    Timestamp start_ts{};
    bool have_var = false;
  };

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    quote_qty_ = Qty::from_double(params_.quote_qty);
    max_inventory_ = Qty::from_double(params_.max_inventory);
    for (auto& s : st_) s = State{};
    start_ = ctx.now();
  }

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    if (!book.is_valid()) {
      ctx.pull_quotes(id);
      return;
    }
    const Instrument& inst = ctx.instrument(id);
    const Price mid = book.mid();
    State& s = st_[id.value];
    const Timestamp now = ctx.now();
    update_variance(s, mid, now, inst);
    if (s.quoted_mid.is_positive() &&
        (mid - s.quoted_mid).abs().raw < params_.requote_threshold_ticks * inst.tick.raw) {
      return;
    }
    s.quoted_mid = mid;
    ctx.set_quotes(id, compute_quotes(id, mid, ctx.position(id).qty, inst, now));
  }

  // The engine pulls a venue's quotes when a connection drops. Forget the last quoted mid so the
  // next book update requotes even if the mid has not moved, and requote at once when the venue
  // is Live again (otherwise a quiet book could leave the strategy unquoted indefinitely).
  template <class Ctx>
  void on_connection(Ctx& ctx, const ConnectionStateMsg& m) noexcept {
    for (const Instrument& inst : ctx.instruments()) {
      if (inst.venue != m.hdr.venue) continue;
      st_[inst.id.value].quoted_mid = Price{};
      if (m.state == ConnState::Live) on_book(ctx, inst.id, ctx.book(inst.id));
    }
  }

  template <class Ctx>
  void on_trade(Ctx& ctx, const TradeMsg& t) noexcept {
    if (!params_.estimate_kappa) return;
    const InstrumentId id = t.hdr.instrument;
    if (!ctx.instruments().contains(id)) return;
    const auto& book = ctx.book(id);
    if (!book.is_valid()) return;
    State& s = st_[id.value];
    const double d = std::fabs((t.price - book.mid()).to_double());
    const double alpha = 0.05;
    s.mean_dist = s.mean_dist == 0.0 ? d : (1.0 - alpha) * s.mean_dist + alpha * d;
    if (s.mean_dist > 0.0) s.kappa_est = 1.0 / s.mean_dist;  // MLE of an exponential decay
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const OmsUpdate& u, const OrderFillMsg&) noexcept {
    const InstrumentId id = u.known ? u.order.instrument : InstrumentId{};
    if (!id.valid() || !ctx.instruments().contains(id)) return;
    const auto& book = ctx.book(id);
    if (!book.is_valid()) return;
    ctx.set_quotes(
        id, compute_quotes(id, book.mid(), ctx.position(id).qty, ctx.instrument(id), ctx.now()));
  }

  // Deterministic quoting function (exposed for tests).
  [[nodiscard]] DesiredQuotes compute_quotes(InstrumentId id,
                                             Price mid,
                                             Qty position,
                                             const Instrument& inst,
                                             Timestamp now) const noexcept {
    DesiredQuotes q;
    if (quote_qty_.is_zero()) return q;
    const State& s = st_[id.value];
    const double m = mid.to_double();
    const double tick = inst.tick.to_double();
    const double sigma2 =
        s.have_var
            ? s.var
            : (params_.sigma_init > 0.0 ? params_.sigma_init * params_.sigma_init : tick * tick);
    const double tau = params_.infinite_horizon ? 1.0 : horizon_left(now);
    const double gamma = params_.gamma;
    const double kappa =
        (params_.estimate_kappa && s.kappa_est > 0.0) ? s.kappa_est : params_.kappa;
    const double qinv = static_cast<double>(position.raw) / static_cast<double>(quote_qty_.raw);
    const double r = m - qinv * gamma * sigma2 * tau;
    double delta = gamma * sigma2 * tau / 2.0 + std::log(1.0 + gamma / kappa) / gamma;
    const double min_delta = static_cast<double>(params_.min_half_spread_ticks) * tick;
    if (delta < min_delta) delta = min_delta;
    const bool can_buy = max_inventory_.is_zero() || position + quote_qty_ <= max_inventory_;
    const bool can_sell = max_inventory_.is_zero() || position - quote_qty_ >= -max_inventory_;
    const Qty qty = inst.round_qty(quote_qty_);
    if (can_buy) {
      const Price bid = inst.round_price(Price::from_double(r - delta), Side::Buy);
      if (bid.is_positive()) static_cast<void>(q.bids.push_back(Level{bid, qty}));
    }
    if (can_sell) {
      const Price ask = inst.round_price(Price::from_double(r + delta), Side::Sell);
      static_cast<void>(q.asks.push_back(Level{ask, qty}));
    }
    if (!q.bids.empty() && !q.asks.empty() && q.bids[0].price >= q.asks[0].price) {
      q.asks[0].price = q.bids[0].price + inst.tick;
    }
    return q;
  }
  [[nodiscard]] const State& state(InstrumentId id) const noexcept { return st_[id.value]; }

 private:
  void update_variance(State& s, Price mid, Timestamp now, const Instrument&) noexcept {
    if (s.last_ts.valid() && s.last_mid.is_positive() && now > s.last_ts) {
      const double dt = static_cast<double>((now - s.last_ts).ns) / 1e9;
      const double dm = (mid - s.last_mid).to_double();
      const double inst_var = dm * dm / dt;  // variance rate
      const double alpha = 1.0 - std::exp(-dt / params_.sigma_window_s);
      s.var = s.have_var ? (1.0 - alpha) * s.var + alpha * inst_var : inst_var;
      s.have_var = true;
    }
    s.last_ts = now;
    s.last_mid = mid;
  }
  [[nodiscard]] double horizon_left(Timestamp now) const noexcept {
    const double elapsed = static_cast<double>((now - start_).ns) / 1e9;
    const double h = params_.horizon_s;
    const double left = h - std::fmod(elapsed, h);
    return left < 0.05 * h ? 0.05 * h : left;
  }

  Qty quote_qty_{};
  Qty max_inventory_{};
  Timestamp start_{};
  State st_[kMaxInstruments] = {};
};

static_assert(StrategyLike<AvellanedaStoikov>);

}  // namespace fastmm
