#pragma once
// OptionsMM: greeks-aware option market making (docs/options.md).
//
// Per option instrument, on every OptionTicker (and, with use_venue_iv = false, every book update):
//
//   F     = ticker underlying_price (the forward the venue prices the option on), r = interest_rate
//   T     = (expiry - now) / 365 d;  sigma = venue mark IV, or the own EWMA of book-mid implied
//   vols theo  = Black-76(F, K, T, sigma, r) per unit of underlying, divided by F for coin-quoted
//           (inverse) instruments; vega_px = vega per vol point in the same price unit
//   half  = max(half_spread_vol * vega_px, min_half_spread_ticks * tick)
//           * (1 + vega_widen * min(1, |portfolio vega| / max_vega))
//   r_px  = theo - delta_skew_ticks * tick * (portfolio delta / max_delta) * option delta
//                - inventory_skew_ticks * tick * (position / quote_qty)
//   bid   = r_px - half (rounded down), ask = r_px + half (rounded up), clamped inside the touch
//
// Portfolio greeks use the positions of every instrument in the context: options contribute
// qty * contract_multiplier * delta (minus the coin premium for inverse options when
// premium_adjusted_delta) and qty * contract_multiplier * vega per vol point (quote currency);
// futures and perpetuals contribute qty * contract_multiplier, divided by their mid for inverse
// contracts (a USD-sized inverse contract holds multiplier / F coins). A side is not quoted when
// a fill of quote_qty would take |portfolio delta| above max_delta, |portfolio vega| above max_vega
// or |position| above max_position, unless the fill would reduce that exposure.
//
// An option is quoted only while its book is two-sided: the engine's stale-market-data check needs
// a valid book, and a quote placed before it would be rejected without the strategy noticing.
// Tickers keep updating the option's greeks meanwhile, and the first valid book quotes it.
//
// DOCUMENTED EXCEPTION to the no-double rule (as in avellaneda_stoikov.hpp): the option model is
// evaluated in double (core/options/black76.hpp) and rounded back to the tick grid; Price/Qty
// arithmetic is untouched. Every hook is noexcept and allocation-free.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/options/black76.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>

namespace fastmm {

struct OptionsMMParams {
  FASTMM_PARAMS(OptionsMMParams)
  FASTMM_PARAM(bool,
               use_venue_iv,
               true,
               0,
               1,
               "price with the venue mark IV (false: own EWMA of book-mid implied vols)")
  FASTMM_PARAM(double, iv_halflife_s, 30.0, 0.01, 86400.0, "half-life of the own IV EWMA, seconds")
  FASTMM_PARAM(
      double, half_spread_vol, 1.0, 0.0, 100.0, "half spread in vol points (x option vega)")
  FASTMM_PARAM(int, min_half_spread_ticks, 1, 0, 1000000, "floor for the half spread, ticks")
  FASTMM_PARAM(double, quote_qty, 1.0, 0.0, 1e9, "contracts per side")
  FASTMM_PARAM(
      double, max_position, 10.0, 0.0, 1e9, "per-option |position| cap, contracts (0 = none)")
  FASTMM_PARAM(double,
               max_delta,
               5.0,
               0.0,
               1e12,
               "portfolio |delta| limit, underlying units (0 = no limit, no delta skew)")
  FASTMM_PARAM(double,
               max_vega,
               1000.0,
               0.0,
               1e12,
               "portfolio |vega| limit, quote currency per vol point (0 = no limit)")
  FASTMM_PARAM(double,
               delta_skew_ticks,
               5.0,
               0.0,
               1e9,
               "reservation shift in ticks for a delta-1 option at max_delta portfolio delta")
  FASTMM_PARAM(double,
               inventory_skew_ticks,
               1.0,
               0.0,
               1e9,
               "reservation shift in ticks per quote_qty of this option's position")
  FASTMM_PARAM(double, vega_widen, 1.0, 0.0, 100.0, "extra half spread (fraction) at max_vega")
  FASTMM_PARAM(
      double, min_expiry_s, 3600.0, 0.0, 1e9, "stop quoting options expiring sooner, seconds")
  FASTMM_PARAM(int, requote_threshold_ticks, 1, 0, 1000000, "ignore theo moves smaller than this")
  FASTMM_PARAM(bool,
               premium_adjusted_delta,
               true,
               0,
               1,
               "inverse (coin-quoted) options: delta minus the coin premium")
  FASTMM_PARAM(int,
               pull_on_stale_ms,
               5000,
               0,
               3600000,
               "pull an option's quotes when its ticker is older than this (0 = never)")
};

class OptionsMM : public StrategyBase<OptionsMMParams> {
 public:
  static constexpr std::string_view name() noexcept { return "options_mm"; }
  static constexpr std::uint64_t kStaleTimer = 0x4f50'5453;  // "OPTS"

  struct State {
    double forward = 0.0;  // underlying_price
    double venue_iv = std::numeric_limits<double>::quiet_NaN();
    double own_iv = std::numeric_limits<double>::quiet_NaN();
    double rate = 0.0;
    std::int64_t own_iv_ns = 0;
    Timestamp last_ticker{};
    // Per contract, from the last pricing (portfolio units).
    double delta = 0.0;  // underlying units
    double vega = 0.0;   // quote currency per vol point
    double theo = 0.0;   // instrument price units
    Price quoted_theo{};
    bool have_ticker = false;
    bool priced = false;
  };

  struct Exposure {
    double delta = 0.0;  // underlying units
    double vega = 0.0;   // quote currency per vol point
  };

  struct Pricing {
    bool valid = false;
    double sigma = 0.0;
    double t_years = 0.0;
    double theo = 0.0;            // price units
    double vega_px = 0.0;         // price units per vol point, one unit of underlying
    double unit_delta = 0.0;      // per unit of underlying (premium adjusted when configured)
    double contract_delta = 0.0;  // underlying units per contract
    double contract_vega = 0.0;   // quote currency per vol point per contract
  };

  template <class Ctx>
  void on_start(Ctx& ctx) noexcept {
    quote_qty_ = Qty::from_double(params_.quote_qty);
    for (auto& s : st_) s = State{};
    if (params_.pull_on_stale_ms > 0)
      stale_timer_ = ctx.add_timer(milliseconds(250), true, kStaleTimer);
  }

  template <class Ctx>
  void on_option_ticker(Ctx& ctx, const OptionTickerMsg& m) noexcept {
    const InstrumentId id = m.hdr.instrument;
    if (!ctx.instruments().contains(id)) return;
    State& s = st_[id.value];
    const double f = m.underlying_price.to_double();
    if (f > 0.0) s.forward = f;
    if (m.mark_iv > 0.0) s.venue_iv = m.mark_iv;
    s.rate = std::isfinite(m.interest_rate) ? m.interest_rate : 0.0;
    s.last_ticker = ctx.now();
    s.have_ticker = true;
    requote(ctx, id, false);
  }

  template <class Ctx, class Book>
  void on_book(Ctx& ctx, InstrumentId id, const Book& book) noexcept {
    if (!ctx.instruments().contains(id)) return;
    const Instrument& inst = ctx.instrument(id);
    if (inst.asset_class != AssetClass::Option) return;
    State& s = st_[id.value];
    if (!book.is_valid()) {
      ctx.pull_quotes(id);
      s.quoted_theo = Price{};
      return;
    }
    if (params_.use_venue_iv) {
      // Tickers drive the quotes; a book update only matters for an option not quoted yet (its
      // book just became two-sided).
      if (!s.quoted_theo.is_positive()) requote(ctx, id, false);
      return;
    }
    update_own_iv(inst, book.mid(), ctx.now());
    requote(ctx, id, false);
  }

  template <class Ctx>
  void on_fill(Ctx& ctx, const OmsUpdate&, const OrderFillMsg&) noexcept {
    // Portfolio greeks changed: every option's skew and limits move.
    requote_all(ctx);
  }

  template <class Ctx>
  void on_timer(Ctx& ctx, TimerId, std::uint64_t user_data) noexcept {
    if (user_data != kStaleTimer || params_.pull_on_stale_ms <= 0) return;
    const Timestamp now = ctx.now();
    for (const Instrument& inst : ctx.instruments()) {
      if (inst.asset_class != AssetClass::Option) continue;
      State& s = st_[inst.id.value];
      if (!s.have_ticker || (now - s.last_ticker).millis() <= params_.pull_on_stale_ms) continue;
      ctx.pull_quotes(inst.id);
      s.quoted_theo = Price{};
      s.have_ticker = false;
    }
  }

  // The engine pulls a venue's quotes when a connection drops: forget the quoted theo so the next
  // update requotes, and requote at once when the venue is Live again.
  template <class Ctx>
  void on_connection(Ctx& ctx, const ConnectionStateMsg& m) noexcept {
    for (const Instrument& inst : ctx.instruments()) {
      if (inst.venue != m.hdr.venue) continue;
      st_[inst.id.value].quoted_theo = Price{};
    }
    if (m.state == ConnState::Live) requote_all(ctx);
  }

  // ---- pure functions (deterministic tests) ------------------------------------------------

  // Portfolio delta and vega from the context's positions and the last pricing of each option.
  template <class Ctx>
  [[nodiscard]] Exposure exposure(const Ctx& ctx) const noexcept {
    Exposure e;
    for (const Instrument& inst : ctx.instruments()) {
      const double q = ctx.position(inst.id).qty.to_double();
      if (q == 0.0) continue;
      const State& s = st_[inst.id.value];
      const double mult = inst.contract_multiplier.to_double();
      switch (inst.asset_class) {
        case AssetClass::Option:
          if (s.priced) {
            e.delta += q * s.delta;
            e.vega += q * s.vega;
          }
          break;
        case AssetClass::Future:
        case AssetClass::Perpetual:
        case AssetClass::Spot: {
          if (!inst.inverse()) {
            e.delta += q * mult;
            break;
          }
          const auto& book = ctx.book(inst.id);
          if (book.is_valid()) {
            const double px = book.mid().to_double();
            if (px > 0.0) e.delta += q * mult / px;
          }
          break;
        }
        default:
          break;
      }
    }
    return e;
  }

  // Model price and greeks of one option at `now_ns` (no quoting, no state change).
  [[nodiscard]] Pricing price(const Instrument& inst, std::int64_t now_ns) const noexcept {
    Pricing p;
    if (inst.asset_class != AssetClass::Option || !inst.strike.is_positive()) return p;
    const State& s = st_[inst.id.value];
    if (!(s.forward > 0.0)) return p;
    p.sigma = s.venue_iv;
    if (!params_.use_venue_iv && s.own_iv > 0.0) p.sigma = s.own_iv;
    if (!(p.sigma > 0.0) || !std::isfinite(p.sigma)) return p;
    p.t_years = options::year_fraction(inst.expiry_ns, now_ns);
    if (!(p.t_years > 0.0) || p.t_years * options::kSecondsPerYear < params_.min_expiry_s) return p;
    const options::CallPut cp =
        inst.option_type == OptionType::Put ? options::CallPut::Put : options::CallPut::Call;
    const options::Greeks g =
        options::black76(cp, s.forward, inst.strike.to_double(), p.t_years, p.sigma, s.rate);
    const bool coin = inst.inverse();
    const double scale = coin ? 1.0 / s.forward : 1.0;
    p.theo = g.price * scale;
    p.vega_px = g.vega / 100.0 * scale;
    p.unit_delta =
        coin && params_.premium_adjusted_delta ? options::coin_delta(g.delta, p.theo) : g.delta;
    const double mult = inst.contract_multiplier.to_double();
    p.contract_delta = p.unit_delta * mult;
    p.contract_vega = g.vega / 100.0 * mult;
    p.valid = std::isfinite(p.theo) && p.theo >= 0.0;
    return p;
  }

  // Quotes for one option given its pricing, own position (contracts) and the portfolio exposure.
  [[nodiscard]] DesiredQuotes compute_quotes(const Instrument& inst,
                                             const Pricing& p,
                                             double position,
                                             const Exposure& e) const noexcept {
    DesiredQuotes q;
    if (!p.valid || quote_qty_.is_zero()) return q;
    const double tick = inst.tick.to_double();
    if (!(tick > 0.0)) return q;
    double half = std::max(params_.half_spread_vol * p.vega_px,
                           static_cast<double>(params_.min_half_spread_ticks) * tick);
    if (params_.max_vega > 0.0)
      half *= 1.0 + params_.vega_widen * std::min(1.0, std::fabs(e.vega) / params_.max_vega);
    double reservation = p.theo;
    if (params_.max_delta > 0.0) {
      const double d = std::clamp(e.delta / params_.max_delta, -1.0, 1.0);
      reservation -= params_.delta_skew_ticks * tick * d * p.unit_delta;
    }
    const double qq = quote_qty_.to_double();
    if (qq > 0.0) reservation -= params_.inventory_skew_ticks * tick * (position / qq);

    const Qty qty = inst.round_qty(quote_qty_);
    if (!qty.is_positive()) return q;
    const double dq = qty.to_double();
    const auto within = [](double now, double after, double limit) {
      return limit <= 0.0 || std::fabs(after) <= limit || std::fabs(after) < std::fabs(now);
    };
    const bool can_buy = within(position, position + dq, params_.max_position) &&
                         within(e.delta, e.delta + dq * p.contract_delta, params_.max_delta) &&
                         within(e.vega, e.vega + dq * p.contract_vega, params_.max_vega);
    const bool can_sell = within(position, position - dq, params_.max_position) &&
                          within(e.delta, e.delta - dq * p.contract_delta, params_.max_delta) &&
                          within(e.vega, e.vega - dq * p.contract_vega, params_.max_vega);
    if (can_buy && reservation - half >= tick) {
      const Price bid = inst.round_price(Price::from_double(reservation - half), Side::Buy);
      if (bid.is_positive()) static_cast<void>(q.bids.push_back(Level{bid, qty}));
    }
    if (can_sell && reservation + half >= tick) {
      const Price ask = inst.round_price(Price::from_double(reservation + half), Side::Sell);
      if (ask.is_positive()) static_cast<void>(q.asks.push_back(Level{ask, qty}));
    }
    if (!q.bids.empty() && !q.asks.empty() && q.bids[0].price >= q.asks[0].price)
      q.asks[0].price = q.bids[0].price + inst.tick;
    return q;
  }

  // Own IV sample from a book mid (EWMA with iv_halflife_s); no-op without a forward or when the
  // mid carries no time value.
  void update_own_iv(const Instrument& inst, Price mid, Timestamp now) noexcept {
    State& s = st_[inst.id.value];
    if (!(s.forward > 0.0) || !mid.is_positive() || !inst.strike.is_positive()) return;
    const double t = options::year_fraction(inst.expiry_ns, now.ns);
    if (!(t > 0.0)) return;
    const double value = inst.inverse() ? mid.to_double() * s.forward : mid.to_double();
    const options::CallPut cp =
        inst.option_type == OptionType::Put ? options::CallPut::Put : options::CallPut::Call;
    const options::IvResult iv =
        options::implied_vol(cp, value, s.forward, inst.strike.to_double(), t, s.rate, 1e-9);
    if (!iv.ok()) return;
    if (!(s.own_iv > 0.0) || s.own_iv_ns == 0 || now.ns <= s.own_iv_ns) {
      s.own_iv = iv.vol;
    } else {
      const double dt = static_cast<double>(now.ns - s.own_iv_ns) / 1e9;
      const double alpha = 1.0 - std::exp(-dt * std::numbers::ln2 / params_.iv_halflife_s);
      s.own_iv += alpha * (iv.vol - s.own_iv);
    }
    s.own_iv_ns = now.ns;
  }

  [[nodiscard]] const State& state(InstrumentId id) const noexcept { return st_[id.value]; }

  // Post-only quotes must not cross the market (see BasicMM::clamp_to_touch).
  static void clamp_to_touch(DesiredQuotes& q,
                             Price best_bid,
                             Price best_ask,
                             Price tick) noexcept {
    if (!q.bids.empty() && best_ask.is_positive() && q.bids[0].price >= best_ask) {
      const Price limit = best_ask - tick;
      if (limit.is_positive()) {
        q.bids[0].price = limit;
      } else {
        q.bids.clear();
      }
    }
    if (!q.asks.empty() && best_bid.is_positive() && q.asks[0].price <= best_bid)
      q.asks[0].price = best_bid + tick;
  }

 private:
  template <class Ctx>
  void requote_all(Ctx& ctx) noexcept {
    for (const Instrument& inst : ctx.instruments()) {
      if (inst.asset_class == AssetClass::Option) requote(ctx, inst.id, true);
    }
  }

  template <class Ctx>
  void requote(Ctx& ctx, InstrumentId id, bool force) noexcept {
    const Instrument& inst = ctx.instrument(id);
    if (inst.asset_class != AssetClass::Option) return;
    State& s = st_[id.value];
    if (!s.have_ticker) return;
    const Pricing p = price(inst, ctx.now().ns);
    if (!p.valid) {
      s.priced = false;
      if (s.quoted_theo.is_positive()) ctx.pull_quotes(id);
      s.quoted_theo = Price{};
      return;
    }
    s.delta = p.contract_delta;
    s.vega = p.contract_vega;
    s.theo = p.theo;
    s.priced = true;
    const auto& book = ctx.book(id);
    if (!book.is_valid()) {
      s.quoted_theo = Price{};  // quote once the book is two-sided (on_book)
      return;
    }
    const Price theo = Price::from_double(p.theo);
    if (!force && s.quoted_theo.is_positive() &&
        (theo - s.quoted_theo).abs().raw < params_.requote_threshold_ticks * inst.tick.raw)
      return;
    DesiredQuotes q = compute_quotes(inst, p, ctx.position(id).qty.to_double(), exposure(ctx));
    clamp_to_touch(q, book.best_bid().price, book.best_ask().price, inst.tick);
    ctx.set_quotes(id, q);
    s.quoted_theo = theo.is_positive() ? theo : Price::from_raw(1);
  }

  Qty quote_qty_{};
  TimerId stale_timer_{};
  State st_[kMaxInstruments] = {};
};

static_assert(StrategyLike<OptionsMM>);

}  // namespace fastmm
