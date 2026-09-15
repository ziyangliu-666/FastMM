"""Hot quoting with a slow model.

HotSlowMM quotes one level per side around a fair value on the engine thread. A slow method refits,
once per second, how far the mid moves after a book imbalance, with numpy least squares over
ctx.recent(), and publishes the fair-value offset the current imbalance implies.

Needs numba (pip install "fastmm-engine[hot]"). Run from the repository root:

    python examples/python/strategies/hot_slow_mm.py
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import numba
import numpy as np

import fastmm
from fastmm import Param, State


# --8<-- [start:quote]
@numba.njit
def quote(self, ctx, book):
    if not book.valid:
        ctx.pull()
        return
    fair = book.mid * (1.0 + self.fair_offset_bps * 1e-4)
    half = fair * self.half_spread_bps * 1e-4
    ctx.quote(fair - half, fair + half, self.quote_qty)
    ctx.keep_passive()
    self.fair = fair
# --8<-- [end:quote]


# --8<-- [start:class]
class HotSlowMM(fastmm.Strategy):
    half_spread_bps = Param(0.1, min=0.0, max=1000.0, doc="half spread around the fair value, bps")
    fair_offset_bps = Param(0.0, min=-20.0, max=20.0, doc="fair value minus mid, bps")
    quote_qty = Param(0.001, min=0.0, max=1.0, doc="quantity per side, base units")
    horizon_rows = Param(20, min=1, max=1000, doc="book changes ahead that the model predicts")
    fair = State(0.0, doc="the last fair value quoted around")

    @fastmm.hot
    def on_book(self, ctx, book):
        quote(self, ctx, book)

    @fastmm.hot
    def on_params(self, ctx, book):
        quote(self, ctx, book)  # requote as soon as the offset changes

    def on_start(self, ctx):
        self.fits = 0
        ctx.publish(fair_offset_bps=0.0)  # quoting starts with the first publish

    @fastmm.every("1s")
    def refit(self, ctx):
        rows = ctx.recent(0).rows
        book = rows[rows["kind"] == 0]
        depth = book["bid_qty"] + book["ask_qty"]
        book = book[(depth > 0) & np.isfinite(book["mid"])]
        h = self.horizon_rows
        if len(book) < h + 50:
            ctx.publish()  # no new values; keeps the parameters within max_param_age_ms
            return
        imbalance = (book["bid_qty"] - book["ask_qty"]) / (book["bid_qty"] + book["ask_qty"])
        move_bps = (book["mid"][h:] / book["mid"][:-h] - 1.0) * 1e4
        beta = np.linalg.lstsq(imbalance[:-h, None], move_bps, rcond=None)[0][0]
        offset = float(np.clip(beta * imbalance[-1], -20.0, 20.0))
        self.fits += 1
        ctx.publish(fair_offset_bps=offset if np.isfinite(offset) else 0.0)
# --8<-- [end:class]


def main() -> None:
    repo = Path(__file__).resolve().parents[3]
    cfg = fastmm.BacktestConfig.from_toml(repo / "configs" / "backtest-example.toml")
    cfg.clear_params()
    cfg.duration_s = 120
    with tempfile.TemporaryDirectory() as tmp:
        # --8<-- [start:run]
        cfg.journal_out = str(Path(tmp) / "hot_slow_mm.fmj")
        result = fastmm.run_backtest(cfg, data="synthetic", strategy=HotSlowMM, slow_delay_ms=5)
        refit = result.slow_methods["refit"]
        print(f"{result.strategy}: {len(result.fills['ts'])} fills, {result.outbound_messages} "
              f"messages, net PnL {result.stats()['net_pnl']:.4f}")
        print(f"refit: {refit['calls']} calls, wall p50 {refit['p50_ms']:.3f} ms, "
              f"p99 {refit['p99_ms']:.3f} ms")
        replayed = fastmm.replay(cfg.journal_out, HotSlowMM)
        print(f"replay: {'identical' if replayed.ok else 'different'} outbound hash, "
              f"what-if: {replayed.what_if}")
        # --8<-- [end:run]


if __name__ == "__main__":
    main()
