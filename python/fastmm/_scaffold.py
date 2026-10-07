"""`fastmm init`: write a starter project that backtests a Python strategy on the simulator.

The files are templates in this module, so the command works from an installed wheel with no
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
quote_qty = 0.0001
max_inventory = 0.0005
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
    quote_qty = Param(0.0001, min=1e-9, doc="size per side, base units")
    max_inventory = Param(0.0005, min=0.0, doc="stop quoting the side that would pass this")
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

LIVE_TOML = '''\
# @NAME_CLASS@ on BTCUSDT on Binance Spot Demo Mode: real-time market data, demo balances, no real
# money (docs: https://ziy.bio/FastMM/how-to/operations/run-on-testnet/).
#   python -m fastmm run strategy:@NAME_CLASS@ --config live.toml --dry-run --duration 60s
# --dry-run sends no orders and needs no keys. Orders need Demo Trading keys (API Key Management
# after switching to Demo Trading on binance.com), not testnet or live keys:
#   export FASTMM_BINANCE_API_KEY=... FASTMM_BINANCE_API_SECRET=...
#   python -m fastmm run strategy:@NAME_CLASS@ --config live.toml

[engine]
name = "@NAME@"
journal = true               # runs/*.fmj: python backtest.py --config live.toml --data <file>
journal_dir = "runs"
epoch_file = "runs/session_epoch"
cpu = -1                     # no thread pinning
min_requote_ticks = 50
min_requote_interval_ms = 1000
post_only = true
supports_replace = true

[venues.binance]
kind = "binance_spot"
ws_url = "wss://demo-stream.binance.com/stream"
ws_api_url = "wss://demo-ws-api.binance.com/ws-api/v3"
rest_url = "https://demo-api.binance.com"
api_key = "${FASTMM_BINANCE_API_KEY}"
api_secret = "${FASTMM_BINANCE_API_SECRET}"
testnet = true
supports_replace = true
recv_window_ms = 3000
stale_ms = 10000             # Demo Mode has quiet spells; 2 s would resync the book in them

[venues.binance.fees]
maker_bps = 10.0
taker_bps = 10.0

# Tick, lot and min_notional are replaced from the venue's exchangeInfo at startup.
[[instruments]]
venue = "binance"
symbol = "BTCUSDT"
base = "BTC"
quote = "USDT"
asset_class = "spot"
tick = "0.01"
lot = "0.00001"
min_qty = "0.00001"
min_notional = "5"

[strategy]
name = "py:@NAME_CLASS@"

# A 15 bps half spread keeps 5 bps a fill after the 10 bps maker fee, before adverse selection.
# requote_ticks is in 0.01 USDT ticks: 100 is about 1.3 bps at 77,000 USDT.
[strategy.params]
half_spread_bps = 15.0
skew_bps = 5.0
quote_qty = 0.0001
max_inventory = 0.0005
requote_ticks = 100

[risk]
max_order_qty = "0.0005"
max_order_notional = "100"
max_position = "0.001"
max_open_orders = 4
price_collar_bps = 50
fat_finger_bps = 200
stale_md_ms = 2000
max_loss = "10"
orders_per_sec = 5
burst = 5
stp = true

[logging]
level = "info"
'''

BACKTEST_PY = '''\
"""Backtest @NAME_CLASS@ on the simulated market, or sweep one of its parameters.

    python backtest.py
    python backtest.py --sweep half_spread_bps=0.005,0.01,0.02,0.05
    python backtest.py --config live.toml --data runs/<journal>.fmj
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
    parser.add_argument("--config", default=HERE / "config.toml", help="default: config.toml")
    parser.add_argument("--data", default="synthetic",
                        help="a recorded .fmj journal or a CSV (default: the synthetic market)")
    args = parser.parse_args()

    cfg = fastmm.BacktestConfig.from_toml(args.config)
    if args.duration_s:
        cfg.duration_s = args.duration_s

    if args.sweep:
        name, _, values = args.sweep.partition("=")
        print(f"{name:>16}  {'net PnL':>10}  {'fills':>6}  {'capture bps':>11}")
        for value in [float(v) for v in values.split(",")]:
            result = fastmm.run_backtest(cfg, data=args.data, strategy=@NAME_CLASS@,
                                         params={name: value})
            stats = result.stats()
            print(f"{value:>16}  {stats['net_pnl']:>10.4f}  {stats['fills']:>6}  "
                  f"{stats['spread_capture_bps']:>11.3f}")
        return

    result = fastmm.run_backtest(cfg, data=args.data, strategy=@NAME_CLASS@)
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
its inventory, `config.toml` describes the simulated market it trades, `backtest.py` runs it, and
`live.toml` runs the same class on Binance Spot Demo Mode.

In the virtual environment that has `fastmm-engine[hot]`:

```bash
python backtest.py
python backtest.py --sweep half_spread_bps=0.005,0.01,0.02,0.05
```

The backtest prints the summary, the PnL decomposition and the markouts, and writes the fills,
orders and equity curve to `runs/backtest/`. The sweep runs one backtest per value.

Net PnL is negative: in this synthetic market only a quote at the touch fills, and it captures
under 0.01 bps while `[venues.sim.fees]` charges 10 bps a fill. A wider quote does not fill at all.
Read the capture, the markouts and the fees separately, and see
<https://ziy.bio/FastMM/explanation/economics/>.

## What to change first

| Want | Edit |
|---|---|
| A different market or fee level | `[venues.sim.fees]`, `[[instruments]]` and `[sim]` in `config.toml` |
| A different quote | `requote` in `strategy.py`, and the `Param` defaults beside it |
| Tighter limits | `[risk]` in `config.toml` |
| Real market data | `backtest.py --data` with a `.fmj` journal or a CSV |

## Live trading

Live sessions need `fastmm-engine-live` (Linux x86-64):

```bash
pip install "fastmm-engine[live]"
python -m fastmm run strategy:@NAME_CLASS@ --config live.toml --dry-run --duration 60s
python backtest.py --config live.toml --data runs/<journal>.fmj
```

`--dry-run` takes public market data, sends no orders and needs no keys; the session's journal in
`runs/` is market data the backtest replays. To send orders, create keys under Demo Trading's API
Key Management on binance.com and run the same command without `--dry-run`:

```bash
export FASTMM_BINANCE_API_KEY=... FASTMM_BINANCE_API_SECRET=...
python -m fastmm run strategy:@NAME_CLASS@ --config live.toml
```

Before a real account, read the go-live checklist:
<https://ziy.bio/FastMM/how-to/operations/go-live-checklist/>.

Docs: <https://ziy.bio/FastMM/>. Strategy API: <https://ziy.bio/FastMM/reference/python-api/>.
'''

TEMPLATES = {
    "config.toml": CONFIG_TOML,
    "live.toml": LIVE_TOML,
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
