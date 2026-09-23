"""`fastmm init`: write a starter project that backtests a Python strategy on the simulator.

The four files are templates in this module, so the command works from an installed wheel with no
repository checkout. `write_project` returns the files it wrote, in the order it wrote them.
"""

from __future__ import annotations

import os
from pathlib import Path

CONFIG_TOML = '''\
# Simulated market and backtest for @NAME@ (docs: https://ziy.bio/FastMM/reference/configuration/).
# The strategy quotes against a seeded synthetic order flow; nothing here touches a network.

[engine]
name = "@NAME@"
journal = false
rng_seed = 1
min_requote_ticks = 1
min_requote_interval_ms = 50
post_only = true             # a quote that would cross is rejected, never taken
supports_replace = false     # cancel-then-new, as the simulator does

[venues.sim]
kind = "sim"

# Positive = a fee the account pays, in basis points of notional. Binance spot VIP 0 pays 10 bps on
# both sides; a strategy that looks profitable only at a lower number is not profitable.
[venues.sim.fees]
maker_bps = 10.0
taker_bps = 10.0

[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
base = "BTC"
quote = "USDT"
asset_class = "spot"
tick = "0.01"                # decimal strings, never doubles
lot = "0.00001"
min_qty = "0.00001"
min_notional = "5"

[strategy]
name = "@NAME@"

# The Param defaults of strategy.py; every key here has to be one of them.
[strategy.params]
half_spread_bps = 0.01
skew_bps = 0.01
quote_qty = 0.002
max_inventory = 0.01
requote_ticks = 2

[risk]
max_order_qty = "0.01"
max_order_notional = "2000"
max_position = "0.05"
max_open_orders = 8
price_collar_bps = 200
fat_finger_bps = 1000
max_loss = "100"
stp = true

[logging]
level = "warn"

# The synthetic market: order flow rates, sizes and mid moves per second.
[sim]
seed = 1
start_mid = "60000"
depth_update_ms = 100
limit_rate_per_s = 400.0
limit_qty_median_lots = 300.0
market_rate_per_s = 30.0
market_qty_median_lots = 1500.0
mid_step_rate_per_s = 20.0
seed_levels = 20

[backtest]
fill_model = "l2_queue"      # queue position decides the fill; "matching" rests in the same book
queue_conservatism = 0.5     # 0 = cancels ahead always help, 1 = never
latency_fixed_us = 200
latency_jitter_us = 50
duration_s = 120
equity_bar_s = 1
initial_capital = 10000
markout_horizons_s = "1,10,60"
output_dir = "runs/backtest"
'''

STRATEGY_PY = '''\
"""@NAME@: quotes both sides around the mid, shifted against inventory.

The hooks carry @fastmm.hot, so Numba compiles them and the engine thread calls them without the
GIL. That is the style that also trades live: https://ziy.bio/FastMM/how-to/strategies/python-hot-hooks/
"""

import numba

import fastmm
from fastmm import Param, State


@numba.njit
def requote(self, ctx, book):
    """Place the ladder. Called by every hook that changes the quotes."""
    ctx.clear()
    mid = book.mid
    position = ctx.position
    half = mid * self.half_spread_bps * 1e-4
    # One quote_qty of inventory moves both quotes by skew_bps, so the side that would grow the
    # position is worse and the other side is better.
    centre = mid - mid * self.skew_bps * 1e-4 * (position / self.quote_qty)
    if position + self.quote_qty <= self.max_inventory:
        ctx.bid(centre - half, self.quote_qty)
    if position - self.quote_qty >= -self.max_inventory:
        ctx.ask(centre + half, self.quote_qty)
    ctx.uncross()      # never quote through the other side
    ctx.keep_passive()  # leave an order that is still correct where it is
    self.last_mid = mid if ctx.quoting_enabled else 0.0


class @NAME_CLASS@(fastmm.Strategy):
    """Symmetric quotes around the mid, skewed by inventory."""

    half_spread_bps = Param(0.01, min=0.0, max=10000.0, doc="half spread around the mid, bps")
    skew_bps = Param(0.01, min=0.0, max=10000.0, doc="shift per quote_qty of inventory, bps")
    quote_qty = Param(0.002, min=1e-9, doc="size per side, base units")
    max_inventory = Param(0.01, min=0.0, doc="stop quoting the side that would pass this")
    requote_ticks = Param(2, min=0, doc="ignore mid moves smaller than this many ticks")
    last_mid = State(0.0, doc="mid of the last quotes sent")

    @fastmm.hot
    def on_book(self, ctx, book):
        if not book.valid:
            ctx.pull()
            self.last_mid = 0.0
            return
        if self.last_mid > 0.0 and abs(book.mid - self.last_mid) < ctx.tick * self.requote_ticks:
            return
        requote(self, ctx, book)

    @fastmm.hot
    def on_fill(self, ctx, book):
        # The inventory moved: re-skew now, whatever the mid did.
        if book.valid:
            requote(self, ctx, book)

    @fastmm.hot
    def on_quoting(self, ctx, book):
        self.last_mid = 0.0
        if ctx.quoting_enabled and book.valid:
            requote(self, ctx, book)
'''

BACKTEST_PY = '''\
"""Backtest @NAME_CLASS@ on the simulated market, or sweep one of its parameters.

    python backtest.py
    python backtest.py --sweep half_spread_bps=0.005,0.01,0.02,0.05
"""

import argparse
from pathlib import Path

import fastmm

from strategy import @NAME_CLASS@

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sweep", metavar="name=v1,v2,...", help="run one grid of one parameter")
    parser.add_argument("--duration-s", type=int, help="override [backtest] duration_s")
    args = parser.parse_args()

    cfg = fastmm.BacktestConfig.from_toml(HERE / "config.toml")
    if args.duration_s:
        cfg.duration_s = args.duration_s

    if args.sweep:
        name, _, values = args.sweep.partition("=")
        print(f"{name:>16}  {'net PnL':>10}  {'fills':>6}  {'capture bps':>11}")
        for value in [float(v) for v in values.split(",")]:
            result = fastmm.run_backtest(cfg, data="synthetic", strategy=@NAME_CLASS@,
                                         params={name: value})
            stats = result.stats()
            print(f"{value:>16}  {stats['net_pnl']:>10.4f}  {stats['fills']:>6}  "
                  f"{stats['spread_capture_bps']:>11.3f}")
        return

    result = fastmm.run_backtest(cfg, data="synthetic", strategy=@NAME_CLASS@)
    print(result.summary_table())
    stats = result.stats()
    # A passive quoter captures the spread by construction; read it against fees and markouts.
    print(f"net PnL {stats['net_pnl']:.4f} = capture {stats['spread_capture']:.4f} "
          f"+ mid drift {stats['mid_drift']:.4f} - fees {stats['fees_paid']:.4f}")
    for horizon in result.markouts():
        total = horizon["total"]
        print(f"markout {horizon['label']:>4}: {total['markout_bps']:+.4f} bps over "
              f"{total['fills']} fills")
    out = HERE / "runs" / "backtest"
    result.write_all(out)
    print(f"CSV and JSON: {out}")


if __name__ == "__main__":
    main()
'''

README_MD = '''\
# @NAME@

A FastMM market maker: `strategy.py` quotes both sides around the mid and shifts the quotes against
its inventory, `config.toml` describes the simulated market it trades, `backtest.py` runs it.

```bash
python -m venv .venv && .venv/bin/pip install "fastmm-engine[hot]"
.venv/bin/python backtest.py
.venv/bin/python backtest.py --sweep half_spread_bps=0.005,0.01,0.02,0.05
```

The backtest prints the summary, the PnL decomposition and the markouts, and writes the fills,
orders and equity curve to `runs/backtest/`. The sweep runs one backtest per value.

Net PnL is negative: the quotes capture about 0.008 bps on a synthetic spread while
`[venues.sim.fees]` charges 10 bps a side. Read the capture, the markouts and the fees separately,
and see <https://ziy.bio/FastMM/explanation/economics/>.

## What to change first

| Want | Edit |
|---|---|
| A different market or fee level | `[venues.sim.fees]`, `[[instruments]]` and `[sim]` in `config.toml` |
| A different quote | `requote` in `strategy.py`, and the `Param` defaults beside it |
| Tighter limits | `[risk]` in `config.toml` |
| Real market data | `python backtest.py` reads `data="synthetic"`; pass a `.fmj` journal or a CSV instead |

## Live trading

The same class runs against a venue once `fastmm-engine-live` is installed (Linux x86-64, CPython
3.10 or later), with a config that has a venue instead of `[venues.sim] kind = "sim"`:

```bash
.venv/bin/pip install "fastmm-engine[live]"
.venv/bin/python -m fastmm run strategy:@NAME_CLASS@ --config live.toml --dry-run
```

`--dry-run` takes public market data and sends no orders. Read the go-live checklist before you
drop it: <https://ziy.bio/FastMM/how-to/operations/go-live-checklist/>.

Docs: <https://ziy.bio/FastMM/>. Strategy API: <https://ziy.bio/FastMM/reference/python-api/>.
'''

TEMPLATES = {
    "config.toml": CONFIG_TOML,
    "strategy.py": STRATEGY_PY,
    "backtest.py": BACKTEST_PY,
    "README.md": README_MD,
}


def _project_name(directory: Path) -> str:
    """The engine and strategy name: the directory's name, reduced to [A-Za-z0-9_-]."""
    raw = "".join(c if (c.isalnum() and c.isascii()) or c in "-_" else "-"
                  for c in directory.resolve().name).strip("-")
    return raw or "fastmm-starter"


def _class_name(name: str) -> str:
    parts = [p for p in name.replace("-", "_").split("_") if p]
    camel = "".join(p[:1].upper() + p[1:] for p in parts) or "Starter"
    if not camel[0].isalpha():
        camel = "Mm" + camel
    return camel


def render(name: str) -> dict[str, str]:
    """The project's files, keyed by relative path."""
    class_name = _class_name(name)
    return {path: text.replace("@NAME_CLASS@", class_name).replace("@NAME@", name)
            for path, text in TEMPLATES.items()}


def write_project(directory: os.PathLike, force: bool = False) -> list[Path]:
    """Write the starter project into `directory`, which is created if it does not exist.

    Raises FileExistsError for a file that is already there unless `force`.
    """
    target = Path(directory)
    files = render(_project_name(target))
    if not force:
        existing = sorted(str(target / p) for p in files if (target / p).exists())
        if existing:
            raise FileExistsError(f"{', '.join(existing)} (use --force to overwrite)")
    target.mkdir(parents=True, exist_ok=True)
    written = []
    for path, text in files.items():
        (target / path).write_text(text, encoding="utf-8")
        written.append(target / path)
    return written
