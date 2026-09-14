"""SkewMM: a float market maker written in Python.

A ladder around the mid, shifted against inventory, with a per-side inventory cap, a requote
threshold and stale-book pulls. Prices and quantities are floats; ``ctx.set_quotes`` rounds them to
the tick (bids down, asks up) and the lot. Run from the repository root:

    python examples/python/strategies/skew_mm.py
"""

from __future__ import annotations

from pathlib import Path

import fastmm
from fastmm import Param, Strategy


class SkewMM(Strategy):
    """Symmetric ladder around mid, skewed by inventory."""

    half_spread_bps = Param(0.01, min=0.0, max=10_000.0, doc="half spread around mid, bps")
    skew_bps = Param(0.01, min=0.0, max=10_000.0, doc="shift per quote_qty of inventory, bps")
    quote_qty = Param(0.002, min=0.0, doc="size per level, base units")
    levels = Param(2, min=1, max=8, doc="levels per side")
    level_step_ticks = Param(5, min=1, doc="ticks between levels")
    max_inventory = Param(0.01, min=0.0, doc="stop quoting the side that grows |position| past it")
    requote_ticks = Param(2, min=0, doc="ignore mid moves smaller than this many ticks")
    stale_ms = Param(2000, min=0, doc="pull quotes when the book is older than this (0 = never)")

    def validate(self):
        if self.quote_qty <= 0.0:
            return "quote_qty must be positive"
        return None

    def on_start(self, ctx):
        self.last_mid = [0.0] * len(ctx.instruments)
        if self.stale_ms > 0:
            ctx.every(100_000_000)  # 100 ms

    def on_book(self, ctx, inst, book):
        if not book.valid:
            ctx.pull_quotes(inst)
            self.last_mid[inst.id] = 0.0
            return
        if abs(book.mid - self.last_mid[inst.id]) < self.requote_ticks * inst.tick:
            return
        self.quote(ctx, inst, book)

    def on_fill(self, ctx, fill):
        book = ctx.book(fill.instrument)
        if book.valid:
            self.quote(ctx, ctx.instrument(fill.instrument), book)

    def on_timer(self, ctx, timer_id, tag):
        now = ctx.now_ns
        for inst in ctx.instruments:
            last = ctx.book(inst).last_update_ns
            if last and now - last > self.stale_ms * 1_000_000:
                ctx.pull_quotes(inst)
                self.last_mid[inst.id] = 0.0

    def on_quoting(self, ctx, enabled):
        for inst in ctx.instruments:
            self.last_mid[inst.id] = 0.0
            book = ctx.book(inst)
            if enabled and book.valid:
                self.quote(ctx, inst, book)

    def quote(self, ctx, inst, book):
        mid = book.mid
        pos = ctx.position(inst).qty
        centre = mid * (1.0 - self.skew_bps * 1e-4 * pos / self.quote_qty)
        half = mid * self.half_spread_bps * 1e-4
        step = self.level_step_ticks * inst.tick
        # Keep level 0 passive: never at or through the opposite touch.
        top_bid = min(centre - half, book.best_ask[0] - inst.tick)
        top_ask = max(centre + half, book.best_bid[0] + inst.tick)
        eps = 1e-12
        bids = [] if pos + self.quote_qty > self.max_inventory + eps else [
            (top_bid - i * step, self.quote_qty) for i in range(self.levels)]
        asks = [] if pos - self.quote_qty < -self.max_inventory - eps else [
            (top_ask + i * step, self.quote_qty) for i in range(self.levels)]
        self.last_mid[inst.id] = mid if ctx.set_quotes(inst, bids, asks) else 0.0


def main() -> None:
    repo = Path(__file__).resolve().parents[3]
    cfg = fastmm.BacktestConfig.from_toml(repo / "configs" / "backtest-example.toml")
    cfg.clear_params()  # the file configures basic_mm
    cfg.duration_s = 60
    result = fastmm.run_backtest(cfg, data="synthetic", strategy=SkewMM, params={"levels": 2})
    print(result.summary_table())
    print(f"{result.strategy}: {result.md_events} events in {result.wall_seconds:.2f} s")


if __name__ == "__main__":
    main()
