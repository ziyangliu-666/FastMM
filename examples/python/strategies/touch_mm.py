"""The README's Python example: a quoter whose hook Numba compiles for the engine thread.

Run from the repository root:

    python examples/python/strategies/touch_mm.py
"""

# --8<-- [start:example]
import fastmm
from fastmm import Param


class TouchMM(fastmm.Strategy):
    half_spread_bps = Param(0.01, min=0.0, doc="half spread, bps")
    quote_qty = Param(0.002, min=0.0, doc="size per side")

    @fastmm.hot  # compiled to machine code with Numba
    def on_book(self, ctx, book):
        if not book.valid:
            ctx.pull()
            return
        half = book.mid * self.half_spread_bps * 1e-4
        ctx.clear()
        ctx.bid(book.mid - half, self.quote_qty)
        ctx.ask(book.mid + half, self.quote_qty)
        ctx.keep_passive()
# --8<-- [end:example]


cfg = fastmm.BacktestConfig.from_toml("configs/backtest-example.toml")
cfg.params = {}  # the file's [strategy.params] are basic_mm's
result = fastmm.run_backtest(cfg, data="synthetic", strategy=TouchMM)
print(result.summary_table())
