#pragma once
// PnLLedger: the backtest's own average-cost book, fed from venue fills and marked at bar
// boundaries. It reuses core PositionTracker so its numbers equal the engine's positions
// bit for bit (same algorithm, same inputs); the runner cross-checks the two.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/position.hpp"

#include <cstdint>

namespace fastmm::bt {

class PnLLedger {
 public:
  explicit PnLLedger(const InstrumentTable& instruments) noexcept : instruments_(instruments) {}

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
  }
  [[nodiscard]] const Position& position(InstrumentId id) const noexcept {
    return tracker_.get(id);
  }
  [[nodiscard]] Notional realized() const noexcept { return tracker_.total_realized(); }
  [[nodiscard]] Notional unrealized() const noexcept { return tracker_.total_unrealized(); }
  [[nodiscard]] Notional fees() const noexcept { return tracker_.total_fees(); }
  // Equity == realized + unrealized - fees (quote currency).
  [[nodiscard]] Notional equity() const noexcept { return tracker_.net_pnl(); }
  [[nodiscard]] Qty net_position() const noexcept {
    Qty q{};
    for (const Instrument& i : instruments_) q += tracker_.get(i.id).qty;
    return q;
  }

 private:
  const InstrumentTable& instruments_;
  PositionTracker tracker_;
};

}  // namespace fastmm::bt
