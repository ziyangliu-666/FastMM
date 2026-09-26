#pragma once
// Position tracking, average-cost method (5.8). All arithmetic is integer with __int128
// intermediates; realized/unrealized/fees feed the max-loss kill switch.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fx.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/strong_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fastmm {

struct alignas(kCacheLine) Position {
  Qty qty{};              // signed net position (+ long / - short)
  Price avg_px{};         // average entry price of the open position
  Notional realized{};    // closed PnL (before fees)
  Notional unrealized{};  // mark-to-market of the open position at the last mark()
  Notional fees{};        // cumulative fees paid (negative = rebates)
  Price last_mark{};
  Qty gross_traded{};  // sum of |fill qty|
  std::uint32_t fills = 0;
  std::uint32_t pad_ = 0;

  [[nodiscard]] Notional net_pnl() const noexcept { return realized + unrealized - fees; }
  [[nodiscard]] bool flat() const noexcept { return qty.is_zero(); }
  [[nodiscard]] Qty abs_qty() const noexcept { return qty.abs(); }
};
static_assert(sizeof(Position) == 64 && std::is_trivially_copyable_v<Position>);

// PnL totals over every instrument (StrategyContext::portfolio()), in the reporting currency when
// the tracker converts (PositionTracker::set_accounting).
struct Portfolio {
  Notional realized{};
  Notional unrealized{};
  Notional fees{};
  Notional net{};  // realized + unrealized - fees
};

class PositionTracker {
 public:
  [[nodiscard]] const Position& get(InstrumentId id) const noexcept {
    FASTMM_ASSERT(id.value < kMaxInstruments);
    return pos_[id.value];
  }
  [[nodiscard]] const Position& operator[](InstrumentId id) const noexcept { return get(id); }

  // Applies a fill. PnL is scaled by the instrument's contract multiplier and, for an inverse
  // (coin-margined) contract, computed in the base coin: qty * multiplier * (1/avg - 1/px). The
  // average entry price of an inverse position is the size-weighted harmonic mean, which is what
  // makes that exact.
  void on_fill(InstrumentId id,
               Side side,
               Price px,
               Qty qty,
               Notional fee,
               const Instrument& inst) noexcept {
    Position& p = pos_[id.value];
    const std::int64_t f = side == Side::Buy ? qty.raw : -qty.raw;
    const std::int64_t cur = p.qty.raw;
    const bool inv = inst.inverse();
    p.fees += fee;
    fees_total_ += fee;
    book(id, &Totals::fees, fee);
    p.gross_traded += qty;
    ++p.fills;
    if (cur == 0 || (cur > 0) == (f > 0)) {
      // Opening / adding: a new average over the old position and this fill.
      const std::int64_t denom = cur + f;
      p.avg_px = inv ? harmonic_avg(cur, p.avg_px, f, px, denom)
                     : arithmetic_avg(cur, p.avg_px, f, px, denom);
      p.qty = Qty::from_raw(denom);
    } else {
      // Reducing / flipping: realise on the closed part in the position's direction.
      const std::int64_t abs_cur = cur < 0 ? -cur : cur;
      const std::int64_t abs_f = f < 0 ? -f : f;
      const std::int64_t closed = abs_f < abs_cur ? abs_f : abs_cur;
      const std::int64_t dir = cur > 0 ? 1 : -1;
      Notional r;
      if (inv) {
        r = inst.inverse_pnl(p.avg_px, px, Qty::from_raw(closed * dir));
      } else {
        const Int128 pnl = static_cast<Int128>(px.raw - p.avg_px.raw) * closed * dir / kFixedScale;
        r = scale(Notional::from_raw(static_cast<std::int64_t>(pnl)), inst);
      }
      p.realized += r;
      realized_total_ += r;
      book(id, &Totals::realized, r);
      const std::int64_t remaining = abs_f - closed;
      if (remaining > 0) {  // flipped through zero: the remainder opens at px
        p.qty = Qty::from_raw(f > 0 ? remaining : -remaining);
        p.avg_px = px;
      } else {
        p.qty = Qty::from_raw(cur + f);
        if (p.qty.is_zero()) p.avg_px = Price{};
      }
    }
    if (p.last_mark.is_positive()) {
      mark(id, p.last_mark, inst);
    } else {
      set_exposure(id, signed_notional(p, px, inst));
    }
  }

  // A fill booked from a venue's cumulative quantity, at the order's own price and with no fee,
  // turned out to be an execution at `px` costing `fee`. Its quantity is already in the position,
  // so on_fill() would count it twice; what was never booked is what it cost. Paying `px` instead
  // of `est_px` for `qty` is worth qty * (px - est_px), given up on a buy and received on a sell,
  // and that difference is exactly the error the estimate left in this position's PnL. The average
  // entry price keeps the estimate: moving it would need the position the fill was booked into,
  // which is history, and the error it carries is the one this correction has already taken out.
  void correct_fill(InstrumentId id,
                    Side side,
                    Price est_px,
                    Price px,
                    Qty qty,
                    Notional fee,
                    const Instrument& inst) noexcept {
    Position& p = pos_[id.value];
    const Notional diff =
        inst.inverse()
            ? inst.inverse_pnl(est_px, px, qty)
            : scale(Notional::from_raw(detail::mul_div<kFixedScale>(px.raw - est_px.raw, qty.raw)),
                    inst);
    const Notional adj = side == Side::Buy ? Notional{} - diff : diff;
    p.realized += adj;
    realized_total_ += adj;
    book(id, &Totals::realized, adj);
    p.fees += fee;
    fees_total_ += fee;
    book(id, &Totals::fees, fee);
  }

  // Mark-to-market the open position, in the instrument's settlement currency.
  void mark(InstrumentId id, Price mid, const Instrument& inst) noexcept {
    Position& p = pos_[id.value];
    p.last_mark = mid;
    set_unrealized(id, p, revalue(p, mid, inst));
    set_exposure(id, signed_notional(p, mid, inst));
  }

  // Reconciliation: overwrite qty/avg with the venue's view. Realized PnL and fees are history and
  // are kept; the unrealized PnL of the new position is remeasured at the last mark, so the totals
  // the max-loss budget reads stay consistent with the positions.
  void set(InstrumentId id, Qty qty, Price avg_px, const Instrument& inst) noexcept {
    Position& p = pos_[id.value];
    p.qty = qty;
    p.avg_px = qty.is_zero() ? Price{} : avg_px;
    set_unrealized(id, p, revalue(p, p.last_mark, inst));
    set_exposure(id, signed_notional(p, p.last_mark, inst));
  }
  void reset(InstrumentId id) noexcept {
    Position& p = pos_[id.value];
    realized_total_ -= p.realized;
    book(id, &Totals::realized, Notional{} - p.realized);
    fees_total_ -= p.fees;
    book(id, &Totals::fees, Notional{} - p.fees);
    set_unrealized(id, p, Notional{});
    set_exposure(id, Notional{});
    p = Position{};
  }

  // Accounting across settlement currencies. With a plan of two or more currencies the tracker also
  // keeps the totals per currency and converts them at the rate last set for each (set_rate), and
  // the totals below are in the reporting currency. A currency whose rate was never set counts as
  // zero. Without one (the default) nothing is converted and every Notional is added as it is.
  void set_accounting(const FxPlan& plan) noexcept {
    ccy_count_ = plan.count;
    for (std::size_t i = 0; i < kMaxInstruments; ++i) ccy_[i] = plan.ccy[i];
    for (auto& r : rate_) r = FxRate{};
    rate_[0] = FxRate::identity();
    for (auto& t : native_) t = Totals{};
    for (auto& t : conv_) t = Totals{};
    conv_total_ = Totals{};
    if (!converting()) return;
    // What is booked already (a tracker configured after fills) moves into its currencies.
    for (std::size_t i = 0; i < kMaxInstruments; ++i) {
      const Position& p = pos_[i];
      Totals& t = native_[ccy_[i]];
      t.realized += p.realized;
      t.unrealized += p.unrealized;
      t.fees += p.fees;
      t.gross += exposure_[i].abs();
      t.net += exposure_[i];
    }
    reconvert(0);
  }
  // The rate of currency `c` (an index of the plan): the totals in it are converted again.
  void set_rate(std::size_t c, FxRate r) noexcept {
    if (c == 0 || c >= kMaxCurrencies) return;
    rate_[c] = r;
    reconvert(c);
  }
  [[nodiscard]] bool converting() const noexcept { return ccy_count_ > 1; }
  [[nodiscard]] std::size_t currency_count() const noexcept { return ccy_count_; }
  [[nodiscard]] FxRate rate(std::size_t c) const noexcept {
    return c < kMaxCurrencies ? rate_[c] : FxRate{};
  }
  // Totals of one currency, in that currency (converting() only).
  struct Totals {
    Notional realized{};
    Notional unrealized{};
    Notional fees{};
    Notional gross{};  // sum of |position| at the last marks
    Notional net{};    // signed sum
    [[nodiscard]] Notional net_pnl() const noexcept { return realized + unrealized - fees; }
  };
  [[nodiscard]] const Totals& native(std::size_t c) const noexcept { return native_[c]; }

  // Totals over every instrument, kept up to date by the updates above (the engine checks
  // net_pnl() on every market-data event; summing kMaxInstruments positions there cost more
  // than the rest of the event). Without conversion Notional carries no currency, so they are only
  // meaningful when every instrument settles in the same one: InstrumentTable::settlement_mix()
  // finds a table that mixes them, and fastmm-live refuses to start on one while [risk] max_loss
  // is set and [accounting] does not cover it.
  // Portfolio exposure at the last marks: the sum of |position| and the signed sum.
  [[nodiscard]] Notional gross_exposure() const noexcept {
    return FASTMM_UNLIKELY(converting()) ? conv_total_.gross : gross_exposure_;
  }
  [[nodiscard]] Notional net_exposure() const noexcept {
    return FASTMM_UNLIKELY(converting()) ? conv_total_.net : net_exposure_;
  }
  [[nodiscard]] Notional total_realized() const noexcept {
    return FASTMM_UNLIKELY(converting()) ? conv_total_.realized : realized_total_;
  }
  [[nodiscard]] Notional total_unrealized() const noexcept {
    return FASTMM_UNLIKELY(converting()) ? conv_total_.unrealized : unrealized_total_;
  }
  [[nodiscard]] Notional total_fees() const noexcept {
    return FASTMM_UNLIKELY(converting()) ? conv_total_.fees : fees_total_;
  }
  [[nodiscard]] Notional net_pnl() const noexcept {
    return total_realized() + total_unrealized() - total_fees();
  }
  [[nodiscard]] Portfolio portfolio() const noexcept {
    Portfolio t;
    t.realized = total_realized();
    t.unrealized = total_unrealized();
    t.fees = total_fees();
    t.net = t.realized + t.unrealized - t.fees;
    return t;
  }

 private:
  static Notional scale(Notional n, const Instrument& inst) noexcept {
    if (inst.contract_multiplier == Qty::from_int(1)) return n;
    return Notional::from_raw(mul_raw(n, inst.contract_multiplier));
  }
  // (cur * avg + f * px) / (cur + f), in abs terms.
  static Price arithmetic_avg(
      std::int64_t cur, Price avg, std::int64_t f, Price px, std::int64_t denom) noexcept {
    const Int128 num = static_cast<Int128>(cur) * avg.raw + static_cast<Int128>(f) * px.raw;
    return Price::from_raw(static_cast<std::int64_t>(num / denom));
  }
  // An inverse contract costs multiplier / price coins, so the average that makes the coin PnL
  // exact is the size-weighted harmonic mean: (cur + f) / (cur / avg + f / px).
  static Price harmonic_avg(
      std::int64_t cur, Price avg, std::int64_t f, Price px, std::int64_t denom) noexcept {
    if (!px.is_positive()) return avg;
    Int128 inv = static_cast<Int128>(f) * kFixedScale / px.raw;
    if (cur != 0 && avg.is_positive()) inv += static_cast<Int128>(cur) * kFixedScale / avg.raw;
    if (inv == 0) return px;
    return Price::from_raw(
        static_cast<std::int64_t>(static_cast<Int128>(denom) * kFixedScale / inv));
  }
  // Open-position PnL at `mark`, in the settlement currency.
  // |position| valued at `mark`, carrying the position's sign.
  static Notional signed_notional(const Position& p, Price mark, const Instrument& inst) noexcept {
    if (p.qty.is_zero() || !mark.is_positive()) return Notional{};
    const Notional n = inst.notional(mark, p.qty.abs());
    return p.qty.raw < 0 ? Notional{} - n : n;
  }
  static Notional revalue(const Position& p, Price mark, const Instrument& inst) noexcept {
    if (p.qty.is_zero() || !mark.is_positive()) return Notional{};
    if (inst.inverse()) return inst.inverse_pnl(p.avg_px, mark, p.qty);
    return scale(
        Notional::from_raw(detail::mul_div<kFixedScale>(mark.raw - p.avg_px.raw, p.qty.raw)), inst);
  }
  // Every path that changes a position's quantity or its mark ends here, so the exposure totals
  // cannot drift from the positions: one place updates both.
  void set_exposure(InstrumentId id, Notional e) noexcept {
    Notional& cur = exposure_[id.value];
    const Notional dg = e.abs() - cur.abs();
    const Notional dn = e - cur;
    gross_exposure_ += dg;
    net_exposure_ += dn;
    book(id, &Totals::gross, dg);
    book(id, &Totals::net, dn);
    cur = e;
  }
  void set_unrealized(InstrumentId id, Position& p, Notional u) noexcept {
    const Notional d = u - p.unrealized;
    unrealized_total_ += d;
    book(id, &Totals::unrealized, d);
    p.unrealized = u;
  }
  // A change of one total in the instrument's currency, and of its converted value. Out of line:
  // without [accounting] this is one predictable branch on the mark path.
  FASTMM_FORCE_INLINE void book(InstrumentId id, Notional Totals::*f, Notional d) noexcept {
    if (FASTMM_UNLIKELY(converting())) book_converted(id, f, d);
  }
  FASTMM_NOINLINE void book_converted(InstrumentId id, Notional Totals::*f, Notional d) noexcept {
    const std::uint8_t c = ccy_[id.value];
    Notional& n = native_[c].*f;
    n += d;
    const Notional v = convert(n, rate_[c]);
    conv_total_.*f += v - conv_[c].*f;
    conv_[c].*f = v;
  }
  void reconvert(std::size_t c) noexcept {
    for (Notional Totals::*f :
         {&Totals::realized, &Totals::unrealized, &Totals::fees, &Totals::gross, &Totals::net}) {
      const Notional v = convert(native_[c].*f, rate_[c]);
      conv_total_.*f += v - conv_[c].*f;
      conv_[c].*f = v;
    }
  }
  Position pos_[kMaxInstruments] = {};
  Notional realized_total_{};
  Notional unrealized_total_{};
  Notional fees_total_{};
  Notional exposure_[kMaxInstruments]{};
  Notional gross_exposure_{};
  Notional net_exposure_{};
  std::uint8_t ccy_count_ = 0;
  std::array<std::uint8_t, kMaxInstruments> ccy_{};
  std::array<FxRate, kMaxCurrencies> rate_{FxRate::identity()};
  std::array<Totals, kMaxCurrencies> native_{};
  std::array<Totals, kMaxCurrencies> conv_{};  // native_ at rate_
  Totals conv_total_{};
};

}  // namespace fastmm
