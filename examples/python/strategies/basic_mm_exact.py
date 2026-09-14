"""BasicMM ported to Python with exact integer arithmetic.

A line-by-line port of the C++ strategy in include/fastmm/strategies/basic_mm.hpp and the helpers
it uses from include/fastmm/strategies/quoting.hpp. Prices and quantities are raw int64 values
(1e-8 scale); the C++ operators are reproduced exactly:

- ``Fixed * Ratio`` multiplies through Int128 and truncates toward zero once (``mul_ratio``);
  Python's ``//`` floors, which differs for negative skews, so ``tdiv`` truncates instead;
- ``Qty / Qty`` is integer division truncating toward zero;
- ``round_price`` floors bids and ceils asks to the tick, ``round_qty`` floors to the lot;
- ``DesiredQuotes::bid/ask`` drop a non-positive price or quantity, ``uncross`` moves a crossing
  level-0 ask one tick above the bid, and ``keep_passive`` shifts a whole ladder inside the touch.

The same configuration, data and seed give the same outbound SHA-256 as the C++ ``basic_mm``
(python/tests/test_strategy_parity.py checks it on tests/fixtures/journals/sample_1000.fmj). Run
from the repository root:

    python examples/python/strategies/basic_mm_exact.py
"""

from __future__ import annotations

from decimal import Decimal
from pathlib import Path
from typing import List, Tuple

import fastmm
from fastmm import BUY, SELL, Param, Strategy

SCALE = 100_000_000  # 1e-8 fixed point
RATIO_PER_BP = 10_000  # Ratio raw units per basis point (1.0 == SCALE)
MAX_QUOTE_LEVELS = 8
STALE_TIMER = 0x5741_4C45  # "STALE", BasicMM::kStaleTimer
STALE_CHECK_NS = 100_000_000  # the stale timer runs every 100 ms

Levels = List[Tuple[int, int]]


def tdiv(a: int, b: int) -> int:
    """C++ integer division: truncates toward zero."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def mul_ratio(v: int, r: int) -> int:
    """Fixed<T> * Ratio: Int128 product, one truncation toward zero."""
    return tdiv(v * r, SCALE)


def round_price(p: int, tick: int, side: int) -> int:
    """round_to_tick: bids down, asks up (floor_div for negative prices too)."""
    floored = (p // tick) * tick
    if side == BUY or floored == p:
        return floored
    return floored + tick


def round_qty(q: int, lot: int) -> int:
    return (q // lot) * lot


def inventory_allows(side: int, position: int, qty: int, limit: int) -> bool:
    if limit == 0:
        return True
    return position + qty <= limit if side == BUY else position - qty >= -limit


def add_level(levels: Levels, px: int, qty: int) -> None:
    """DesiredQuotes::bid / ask."""
    if px > 0 and qty > 0 and len(levels) < MAX_QUOTE_LEVELS:
        levels.append((px, qty))


def keep_passive(bids: Levels, asks: Levels, best_bid: int, best_ask: int, tick: int) -> None:
    if bids and best_ask > 0:
        limit = best_ask - tick
        if bids[0][0] > limit:
            shift = bids[0][0] - limit
            bids[:] = [(p - shift, q) for p, q in bids]
            while bids and bids[-1][0] <= 0:
                bids.pop()
    if asks and best_bid > 0:
        limit = best_bid + tick
        if asks[0][0] < limit:
            shift = limit - asks[0][0]
            asks[:] = [(p + shift, q) for p, q in asks]


def exact(value: float, scale: int) -> int:
    """A decimal parameter as raw fixed point, like the C++ exact parser (no float rounding)."""
    raw = Decimal(repr(float(value))) * scale
    if raw != raw.to_integral_value():
        raise ValueError(f"{value} has more decimals than the fixed-point scale holds")
    return int(raw)


class BasicMMExact(Strategy):
    """Symmetric quotes around mid, half_spread_bps wide, skewed by inventory, with a per-side
    inventory cap. Parameter names, defaults and ranges match the C++ BasicMM."""

    half_spread_bps = Param(5.0, min=0.0, max=10000.0, doc="half spread around mid, basis points")
    skew_bps_per_unit = Param(
        1.0, min=0.0, max=10000.0,
        doc="shift both quotes by this many bps per quote_qty of inventory")
    quote_qty = Param(0.01, min=0.0, max=1000000000.0, doc="quantity per level (base units)")
    max_inventory = Param(
        0.1, min=0.0, max=1000000000.0,
        doc="stop quoting the side that would grow |position| past this (0 = no cap)")
    requote_threshold_ticks = Param(
        1, min=0, max=1000000, doc="ignore mid moves smaller than this many ticks")
    pull_on_stale_ms = Param(
        2000, min=0, max=3600000, doc="pull quotes when the book is older than this (0 = never)")
    levels = Param(1, min=1, max=8, doc="quote levels per side")
    level_step_ticks = Param(1, min=1, max=100000, doc="tick distance between successive levels")

    def on_start(self, ctx) -> None:
        self.half_spread = exact(self.half_spread_bps, RATIO_PER_BP)
        self.skew = exact(self.skew_bps_per_unit, RATIO_PER_BP)
        self.qty = exact(self.quote_qty, SCALE)
        self.max_inv = exact(self.max_inventory, SCALE)
        self.last_mid = [0] * len(ctx.instruments)
        if self.pull_on_stale_ms > 0:
            self.stale_timer = ctx.every(STALE_CHECK_NS, STALE_TIMER)

    def on_book(self, ctx, inst, book) -> None:
        if not book.valid:
            ctx.pull_quotes(inst)
            self.last_mid[inst.id] = 0
            return
        mid = book.mid_raw
        last = self.last_mid[inst.id]
        if last > 0 and abs(mid - last) < inst.tick_raw * self.requote_threshold_ticks:
            return
        self.requote(ctx, inst, book)

    def on_fill(self, ctx, fill) -> None:
        # Inventory changed: re-skew immediately (bypasses the mid-move threshold).
        book = ctx.book(fill.instrument)
        if book.valid:
            self.requote(ctx, ctx.instrument(fill.instrument), book)

    def on_timer(self, ctx, timer_id, tag) -> None:
        if tag != STALE_TIMER:
            return
        now = ctx.now_ns
        for inst in ctx.instruments:
            last = ctx.book(inst).last_update_ns
            if last != 0 and tdiv(now - last, 1_000_000) > self.pull_on_stale_ms:
                ctx.pull_quotes(inst)
                self.last_mid[inst.id] = 0

    def on_connection(self, ctx, msg) -> None:
        for inst in ctx.instruments:
            if inst.venue != msg.venue:
                continue
            self.last_mid[inst.id] = 0
            if not msg.live:
                continue
            book = ctx.book(inst)
            if book.valid:
                self.requote(ctx, inst, book)

    def on_quoting(self, ctx, enabled) -> None:
        for inst in ctx.instruments:
            self.last_mid[inst.id] = 0
            if not enabled:
                continue
            book = ctx.book(inst)
            if book.valid:
                self.requote(ctx, inst, book)

    def compute_quotes(self, mid: int, position: int, inst) -> Tuple[Levels, Levels]:
        bids: Levels = []
        asks: Levels = []
        if self.qty == 0:
            return bids, asks
        inventory_units = tdiv(position, self.qty)
        half = mul_ratio(mid, self.half_spread)
        centre = mid - mul_ratio(mid, self.skew * inventory_units)
        tick = inst.tick_raw
        step = tick * self.level_step_ticks
        qty = round_qty(self.qty, inst.lot_raw)
        can_buy = inventory_allows(BUY, position, self.qty, self.max_inv)
        can_sell = inventory_allows(SELL, position, self.qty, self.max_inv)
        for level in range(self.levels):
            if can_buy:
                add_level(bids, round_price(centre - half - step * level, tick, BUY), qty)
            if can_sell:
                add_level(asks, round_price(centre + half + step * level, tick, SELL), qty)
        if bids and asks and bids[0][0] >= asks[0][0]:  # DesiredQuotes::uncross
            asks[0] = (bids[0][0] + tick, asks[0][1])
        return bids, asks

    def requote(self, ctx, inst, book) -> None:
        mid = book.mid_raw
        bids, asks = self.compute_quotes(mid, ctx.position(inst).qty_raw, inst)
        keep_passive(bids, asks, book.best_bid_raw[0], book.best_ask_raw[0], inst.tick_raw)
        # Remember the mid only if the quotes were taken (ignored while quoting is disabled).
        self.last_mid[inst.id] = mid if ctx.set_quotes_raw(inst, bids, asks) else 0


def main() -> None:
    repo = Path(__file__).resolve().parents[3]
    cfg = fastmm.BacktestConfig.from_toml(repo / "configs" / "backtest-example.toml")
    data = repo / "tests" / "fixtures" / "journals" / "sample_1000.fmj"
    cpp = fastmm.run_backtest(cfg, data=data, strategy="basic_mm")
    py = fastmm.run_backtest(cfg, data=data, strategy=BasicMMExact)
    expected = (data.parent / "sample_1000.sha256").read_text().strip()
    print(f"C++ basic_mm    {cpp.outbound_sha256}  ({cpp.outbound_messages} messages)")
    print(f"{py.strategy:<15} {py.outbound_sha256}  ({py.outbound_messages} messages)")
    print(f"committed       {expected}")
    print("identical" if cpp.outbound_sha256 == py.outbound_sha256 == expected else "DIFFERENT")


if __name__ == "__main__":
    main()
