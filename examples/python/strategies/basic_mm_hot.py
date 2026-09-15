"""BasicMM as hot hooks, compiled with Numba and called by the engine thread.

BasicMMHot is a line-by-line port of include/fastmm/strategies/basic_mm.hpp with exact integer
arithmetic: the parameters' `_raw` fields and the book's and ctx's raw fields, with `fastmm.fx` for
the C++ operators (`Fixed * Ratio` truncates toward zero, `Qty / Qty` truncates, bids round down and
asks up to the tick). On the same configuration, data and seed it sends the same orders as the C++
`basic_mm` (python/tests/test_hot.py checks the outbound SHA-256).

BasicMMHotFloat writes the same quoting in floats; the engine rounds each float level to the
nearest 1e-8 and then to the tick and lot.

Needs numba (pip install "fastmm-engine[hot]"). Run from the repository root:

    python examples/python/strategies/basic_mm_hot.py
"""

from __future__ import annotations

from pathlib import Path

import numba

import fastmm
from fastmm import Param, State, fx


# --8<-- [start:requote]
# A helper called from hot hooks is a numba.njit function; FastMM compiles it with bounds checks and
# runs the IR check on it with the hook.
@numba.njit
def requote(self, ctx, book):
    ctx.clear()  # quote nothing unless levels follow (C++: an empty DesiredQuotes)
    mid = book.mid_raw
    pos = ctx.position_raw
    q = self.quote_qty_raw
    if q != 0:
        units = fx.tdiv(pos, q)  # Qty / Qty, truncating
        half = fx.mul_ratio(mid, fx.bps_ratio(self.half_spread_bps_raw))
        centre = mid - fx.mul_ratio(mid, fx.bps_ratio(self.skew_bps_per_unit_raw) * units)
        tick = ctx.tick_raw
        step = tick * self.level_step_ticks
        qty = fx.round_qty(q, ctx.lot_raw)
        limit = self.max_inventory_raw
        can_buy = limit == 0 or pos + q <= limit
        can_sell = limit == 0 or pos - q >= -limit
        for level in range(self.levels):
            if can_buy:
                ctx.bid_raw(fx.round_price(centre - half - step * level, tick, fx.BUY), qty)
            if can_sell:
                ctx.ask_raw(fx.round_price(centre + half + step * level, tick, fx.SELL), qty)
        ctx.uncross()
    ctx.keep_passive()
    # C++ remembers the mid only if set_quotes takes the quotes, which it does while quoting is
    # enabled for the instrument.
    self.last_mid = mid if ctx.quoting_enabled else 0
# --8<-- [end:requote]


# --8<-- [start:class]
class BasicMMHot(fastmm.Strategy):
    """Symmetric quotes around mid, skewed by inventory, with a per-side inventory cap. Parameter
    names, defaults and ranges match the C++ BasicMM."""

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
    last_mid = State(0, doc="mid of the last accepted quotes, raw")

    @fastmm.hot
    def on_book(self, ctx, book):
        if not book.valid:
            ctx.pull()
            self.last_mid = 0
            return
        mid = book.mid_raw
        if self.last_mid > 0 and abs(mid - self.last_mid) < ctx.tick_raw * self.requote_threshold_ticks:
            return
        requote(self, ctx, book)

    @fastmm.hot
    def on_fill(self, ctx, book):
        # Inventory changed: re-skew immediately (bypasses the mid-move threshold).
        if book.valid:
            requote(self, ctx, book)

    @fastmm.hot
    def on_quoting(self, ctx, book):
        self.last_mid = 0
        if ctx.quoting_enabled and book.valid:
            requote(self, ctx, book)

    @fastmm.hot
    def on_connection(self, ctx, book):
        self.last_mid = 0
        if ctx.connected and book.valid:
            requote(self, ctx, book)

    @fastmm.hot(every="100ms")
    def check_stale(self, ctx, book):
        if self.pull_on_stale_ms > 0 and book.ts_ns != 0:
            if (ctx.now_ns - book.ts_ns) // 1_000_000 > self.pull_on_stale_ms:
                ctx.pull()
                self.last_mid = 0
# --8<-- [end:class]


@numba.njit
def requote_float(self, ctx, book):
    ctx.clear()
    mid = book.mid
    pos = ctx.position
    q = self.quote_qty
    if q > 0.0:
        units = int(pos / q)
        half = mid * self.half_spread_bps * 1e-4
        centre = mid - mid * self.skew_bps_per_unit * 1e-4 * units
        step = ctx.tick * self.level_step_ticks
        limit = self.max_inventory
        can_buy = limit == 0.0 or pos + q <= limit + 1e-9
        can_sell = limit == 0.0 or pos - q >= -limit - 1e-9
        for level in range(self.levels):
            if can_buy:
                ctx.bid(centre - half - step * level, q)
            if can_sell:
                ctx.ask(centre + half + step * level, q)
        ctx.uncross()
    ctx.keep_passive()
    self.last_mid = mid if ctx.quoting_enabled else 0.0


class BasicMMHotFloat(fastmm.Strategy):
    """BasicMMHot in floats (no pull on stale books)."""

    half_spread_bps = Param(5.0, min=0.0, max=10000.0)
    skew_bps_per_unit = Param(1.0, min=0.0, max=10000.0)
    quote_qty = Param(0.01, min=0.0, max=1000000000.0)
    max_inventory = Param(0.1, min=0.0, max=1000000000.0)
    requote_threshold_ticks = Param(1, min=0, max=1000000)
    pull_on_stale_ms = Param(2000, min=0, max=3600000)
    levels = Param(1, min=1, max=8)
    level_step_ticks = Param(1, min=1, max=100000)
    last_mid = State(0.0)

    @fastmm.hot
    def on_book(self, ctx, book):
        if not book.valid:
            ctx.pull()
            self.last_mid = 0.0
            return
        if self.last_mid > 0.0 and abs(book.mid - self.last_mid) < ctx.tick * (
                self.requote_threshold_ticks - 0.5):
            return
        requote_float(self, ctx, book)

    @fastmm.hot
    def on_fill(self, ctx, book):
        if book.valid:
            requote_float(self, ctx, book)

    @fastmm.hot
    def on_quoting(self, ctx, book):
        self.last_mid = 0.0
        if ctx.quoting_enabled and book.valid:
            requote_float(self, ctx, book)


def main() -> None:
    repo = Path(__file__).resolve().parents[3]
    cfg = fastmm.BacktestConfig.from_toml(repo / "configs" / "backtest-example.toml")
    data = repo / "tests" / "fixtures" / "journals" / "sample_1000.fmj"
    cpp = fastmm.run_backtest(cfg, data=data, strategy="basic_mm")
    hot = fastmm.run_backtest(cfg, data=data, strategy=BasicMMHot)
    flt = fastmm.run_backtest(cfg, data=data, strategy=BasicMMHotFloat)
    expected = (data.parent / "sample_1000.sha256").read_text().strip()
    print(f"C++ basic_mm        {cpp.outbound_sha256}  ({cpp.outbound_messages} messages)")
    print(f"{hot.strategy:<19} {hot.outbound_sha256}  ({hot.outbound_messages} messages)")
    print(f"{flt.strategy:<19} {flt.outbound_sha256}  ({flt.outbound_messages} messages)")
    print(f"committed           {expected}")
    print("identical" if cpp.outbound_sha256 == hot.outbound_sha256 == expected else "DIFFERENT")


if __name__ == "__main__":
    main()
