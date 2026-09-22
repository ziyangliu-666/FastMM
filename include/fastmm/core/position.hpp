#pragma once
// Position tracking, average-cost method (5.8). All arithmetic is integer with __int128
// intermediates; realized/unrealized/fees feed the max-loss kill switch.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/strong_id.hpp"

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

// PnL totals over every instrument (StrategyContext::portfolio()).
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

  // Applies a fill. PnL is scaled by the instrument's contract multiplier.
  void on_fill(InstrumentId id,
               Side side,
               Price px,
               Qty qty,
               Notional fee,
               const Instrument& inst) noexcept {
    Position& p = pos_[id.value];
    const std::int64_t f = side == Side::Buy ? qty.raw : -qty.raw;
    const std::int64_t cur = p.qty.raw;
    p.fees += fee;
    fees_total_ += fee;
    p.gross_traded += qty;
    ++p.fills;
    if (cur == 0 || (cur > 0) == (f > 0)) {
      // Opening / adding: new average = (cur*avg + f*px) / (cur + f), all in abs terms.
      const Int128 num = static_cast<Int128>(cur) * p.avg_px.raw + static_cast<Int128>(f) * px.raw;
      const std::int64_t denom = cur + f;
      p.avg_px = Price::from_raw(static_cast<std::int64_t>(num / denom));
      p.qty = Qty::from_raw(denom);
    } else {
      // Reducing / flipping: realise on the closed part at (px - avg) * sign(position).
      const std::int64_t abs_cur = cur < 0 ? -cur : cur;
      const std::int64_t abs_f = f < 0 ? -f : f;
      const std::int64_t closed = abs_f < abs_cur ? abs_f : abs_cur;
      const std::int64_t dir = cur > 0 ? 1 : -1;
      const Int128 pnl = static_cast<Int128>(px.raw - p.avg_px.raw) * closed * dir / kFixedScale;
      const Notional r = scale(Notional::from_raw(static_cast<std::int64_t>(pnl)), inst);
      p.realized += r;
      realized_total_ += r;
      const std::int64_t remaining = abs_f - closed;
      if (remaining > 0) {  // flipped through zero: the remainder opens at px
        p.qty = Qty::from_raw(f > 0 ? remaining : -remaining);
        p.avg_px = px;
      } else {
        p.qty = Qty::from_raw(cur + f);
        if (p.qty.is_zero()) p.avg_px = Price{};
      }
    }
    if (p.last_mark.is_positive()) mark(id, p.last_mark, inst);
  }

  // Mark-to-market the open position.
  void mark(InstrumentId id, Price mid, const Instrument& inst) noexcept {
    Position& p = pos_[id.value];
    p.last_mark = mid;
    Notional u{};
    if (!p.qty.is_zero() && !mid.is_zero()) {
      u = scale(Notional::from_raw(detail::mul_div<kFixedScale>(mid.raw - p.avg_px.raw, p.qty.raw)),
                inst);
    }
    unrealized_total_ += u - p.unrealized;
    p.unrealized = u;
  }

  // Reconciliation: overwrite qty/avg with the venue's view (PnL history is kept).
  void set(InstrumentId id, Qty qty, Price avg_px) noexcept {
    Position& p = pos_[id.value];
    p.qty = qty;
    p.avg_px = qty.is_zero() ? Price{} : avg_px;
  }
  void reset(InstrumentId id) noexcept {
    Position& p = pos_[id.value];
    realized_total_ -= p.realized;
    unrealized_total_ -= p.unrealized;
    fees_total_ -= p.fees;
    p = Position{};
  }

  // Totals over every instrument, kept up to date by the updates above (the engine checks
  // net_pnl() on every market-data event; summing kMaxInstruments positions there cost more
  // than the rest of the event).
  [[nodiscard]] Notional total_realized() const noexcept { return realized_total_; }
  [[nodiscard]] Notional total_unrealized() const noexcept { return unrealized_total_; }
  [[nodiscard]] Notional total_fees() const noexcept { return fees_total_; }
  [[nodiscard]] Notional net_pnl() const noexcept {
    return total_realized() + total_unrealized() - total_fees();
  }
  [[nodiscard]] Portfolio portfolio() const noexcept {
    Portfolio t;
    t.realized = realized_total_;
    t.unrealized = unrealized_total_;
    t.fees = fees_total_;
    t.net = t.realized + t.unrealized - t.fees;
    return t;
  }

 private:
  static Notional scale(Notional n, const Instrument& inst) noexcept {
    if (inst.contract_multiplier == Qty::from_int(1)) return n;
    return Notional::from_raw(mul_raw(n, inst.contract_multiplier));
  }
  Position pos_[kMaxInstruments] = {};
  Notional realized_total_{};
  Notional unrealized_total_{};
  Notional fees_total_{};
};

}  // namespace fastmm
