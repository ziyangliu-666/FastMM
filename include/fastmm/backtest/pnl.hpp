#pragma once
// PnLLedger: the backtest's own average-cost book, fed from venue fills and marked at bar
// boundaries. It reuses core PositionTracker so its numbers equal the engine's positions
// bit for bit (same algorithm, same inputs); the runner cross-checks the two.
#include "fastmm/core/fx.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/position.hpp"

#include <cstdint>

namespace fastmm::bt {

class PnLLedger {
 public:
  explicit PnLLedger(const InstrumentTable& instruments) noexcept : instruments_(instruments) {}
  // [accounting]: totals in the reporting currency, at the rates of the FX sources' marks.
  void set_accounting(const FxPlan& plan) noexcept {
    fx_ = plan;
    tracker_.set_accounting(plan);
  }

  void on_fill(const OrderFillMsg& f) noexcept {
    if (!instruments_.contains(f.hdr.instrument)) return;
    tracker_.on_fill(
        f.hdr.instrument, f.side, f.price, f.qty, f.fee, instruments_.get(f.hdr.instrument));
  }
  void on_fill(InstrumentId id, Side side, Price px, Qty qty, Notional fee) noexcept {
    if (!instruments_.contains(id)) return;
    tracker_.on_fill(id, side, px, qty, fee, instruments_.get(id));
  }
  void mark(InstrumentId id, Price mid) noexcept {
    if (!instruments_.contains(id) || !mid.is_positive()) return;
    tracker_.mark(id, mid, instruments_.get(id));
    if (const int c = fx_.priced_by(id); c > 0) {
      const auto k = static_cast<std::size_t>(c);
      tracker_.set_rate(k, FxRate::from_mid(mid, fx_.sources[k].invert));
    }
  }
  [[nodiscard]] const Position& position(InstrumentId id) const noexcept {
    return tracker_.get(id);
  }
  [[nodiscard]] Notional realized() const noexcept { return tracker_.total_realized(); }
  [[nodiscard]] Notional unrealized() const noexcept { return tracker_.total_unrealized(); }
  [[nodiscard]] Notional fees() const noexcept { return tracker_.total_fees(); }
  // Equity == realized + unrealized - fees (quote currency; the reporting one with [accounting]).
  [[nodiscard]] Notional equity() const noexcept { return tracker_.net_pnl(); }
  [[nodiscard]] Qty net_position() const noexcept {
    Qty q{};
    for (const Instrument& i : instruments_) q += tracker_.get(i.id).qty;
    return q;
  }

 private:
  const InstrumentTable& instruments_;
  PositionTracker tracker_;
  FxPlan fx_;
};

}  // namespace fastmm::bt
