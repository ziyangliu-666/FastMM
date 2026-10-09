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

SIM_TOML = '''\
# @NAME_CLASS@ live against the simulated exchange on this machine: real orders over WebSocket to
# a matching engine with its own order flow, no account and no network. In one terminal:
#   fastmm sim
# and in another:
#   python -m fastmm run strategy:@NAME_CLASS@ --config sim.toml --duration 60s
# The session's journal lands in runs/: replay it, or backtest on its market data.

[engine]
name = "@NAME@-sim"
journal = true
journal_dir = "runs"
epoch_file = "runs/session_epoch"
cpu = -1
post_only = true
supports_replace = true

[venues.sim]
kind = "binance_spot"
ws_url = "ws://127.0.0.1:9080/stream"
ws_api_url = "ws://127.0.0.1:9080/ws-api/v3"
rest_url = "http://127.0.0.1:9080"
api_key = "sim-key"          # the simulator's built-in account
api_secret = "sim-secret"
testnet = true
supports_replace = true
recv_window_ms = 3000

[venues.sim.fees]
maker_bps = 1.0
taker_bps = 4.0

[[instruments]]
venue = "sim"
symbol = "BTCUSDT"
base = "BTC"
quote = "USDT"
asset_class = "spot"
tick = "0.01"
lot = "0.00001"
min_qty = "0.00001"
max_qty = "100"
min_notional = "5"

[strategy]
name = "py:@NAME_CLASS@"

# The simulator's touch sits about 5 bps from its mid at 60,000 USDT.
[strategy.params]
half_spread_bps = 5.0
skew_bps = 1.0
quote_qty = 0.001
max_inventory = 0.01
requote_ticks = 10

[risk]
max_order_qty = "0.01"
max_order_notional = "1000"
max_position = "0.02"
max_open_orders = 8
price_collar_bps = 100
fat_finger_bps = 500
stale_md_ms = 2000
max_loss = "50"
orders_per_sec = 20
burst = 10
stp = true

[logging]
level = "info"

# python backtest.py --config sim.toml --data journal:runs/<file>.fmj,strip_own=1
[backtest]
fill_model = "l2_queue"
markout_horizons_s = "1,10"
output_dir = "runs/backtest"
'''

PRODUCTION_TOML = '''\
# @NAME_CLASS@ on Binance Spot with the settings of a dedicated host (fastmm init --profile
# production). Real account, real orders: read the go-live checklist first
# (https://ziy.bio/FastMM/how-to/operations/go-live-checklist/).
#
#   python -m fastmm run strategy:@NAME_CLASS@ --config production.toml --dry-run --duration 60s
#   taskset -c 0 python -m fastmm run strategy:@NAME_CLASS@ --config production.toml
#
# The dry run sends no orders and needs FASTMM_BINANCE_API_KEY alone: the SBE streams take the key,
# not the private key.
#
# Cores. The engine and every venue's network thread poll without sleeping on a physical core of
# their own; `lscpu -e` lists them (CORE column; on AWS c7i, CPU n and n + vCPUs/2 are one core).
# Seven hot threads (an engine and six accounts) need seven cores plus one for everything else:
# 16 vCPUs on c7i. `taskset -c 0` keeps the journal, store, log and control threads on CPU 0; the
# engine and network threads move themselves to `cpu` and `net_cpus`. fastmm-live prints a
# topology check at start that names any thread sharing a core.
#
# Host: isolcpus, nohz_full and rcu_nocbs on those cores, the performance governor, NIC interrupts
# on CPU 0, ulimit -l unlimited for lock_memory and write access to /dev/cpu_dma_latency
# (https://ziy.bio/FastMM/how-to/operations/running-in-production/#host-tuning).
#
# Keys: an Ed25519 key logs on once per connection and then sends orders unsigned, and it is the
# key the SBE market-data streams take. Register its public key under API Management, trading
# permission only, IP-restricted:
#   export FASTMM_BINANCE_API_KEY=...                       # the key Binance shows for it
#   export FASTMM_BINANCE_PRIVATE_KEY="$(cat ed25519.pem)"  # the PKCS#8 PEM private key

[engine]
name = "@NAME@"
cpu = 1                      # the engine thread: a core of its own
net_cpus = [2]               # one core per [venues.*] section, in order; not cpu's sibling
spin_mode = "busy"           # never sleep: a wake-up costs more than the whole tick on a VM
lock_memory = true           # mlockall: no page faults after start
cpu_dma_latency_us = 0       # no CPU enters an idle state that is slow to leave
timer_slack_ns = 1           # the waits that remain end on time
instance_lock = true         # a second process on this engine name refuses to start
journal = true
journal_dir = "runs"
epoch_file = "runs/session_epoch"
min_requote_ticks = 1
min_requote_interval_ms = 50
post_only = true
supports_replace = false     # Spot's cancel-replace is two operations; a cancel and a new order

[venues.binance]
kind = "binance_spot"
ws_url = "wss://stream.binance.com:9443/stream"
ws_api_url = "wss://ws-api.binance.com:443/ws-api/v3"
rest_url = "https://api.binance.com"
api_key = "${FASTMM_BINANCE_API_KEY}"
key_type = "ed25519"
private_key_env = "FASTMM_BINANCE_PRIVATE_KEY"
md_format = "sbe"
order_api = "ws"
supports_replace = false
recv_window_ms = 3000

# Your account's schedule, bps; a negative value is a rebate.
[venues.binance.fees]
maker_bps = 10.0
taker_bps = 10.0

# A second account of the same exchange behind this venue: its own keys and network core
# (net_cpus = [2, 3]), the primary's instruments and market data.
# [venues.binance_m2]
# kind = "binance_spot"
# pool_of = "binance"
# ws_api_url = "wss://ws-api.binance.com:443/ws-api/v3"
# rest_url = "https://api.binance.com"
# api_key = "${FASTMM_BINANCE_M2_API_KEY}"
# key_type = "ed25519"
# private_key_env = "FASTMM_BINANCE_M2_PRIVATE_KEY"

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

[strategy.params]
half_spread_bps = 15.0
skew_bps = 5.0
quote_qty = 0.0001
max_inventory = 0.0005
requote_ticks = 100

# Size these to the account.
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

README_PRODUCTION_MD = '''
## Production

`production.toml` runs the same class on a Binance Spot account with the settings of a dedicated
host: `spin_mode = "busy"`, the engine and network threads on cores of their own, `lock_memory`,
`cpu_dma_latency_us = 0`, an Ed25519 key and SBE market data. Its header says which cores to
choose and how to start it; fastmm-live prints a topology check at start that names any hot thread
sharing a physical core.

```bash
python -m fastmm run strategy:@NAME_CLASS@ --config production.toml --dry-run --duration 60s
taskset -c 0 python -m fastmm run strategy:@NAME_CLASS@ --config production.toml
```
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

## What to change first

| Want | Edit |
|---|---|
| A different market or fee level | `[venues.sim.fees]`, `[[instruments]]` and `[sim]` in `config.toml` |
| A different quote | `requote` in `strategy.py`, and the `Param` defaults beside it |
| Tighter limits | `[risk]` in `config.toml` |
| Real market data | `backtest.py --data` with a `.fmj` journal or a CSV |

## Live on the simulated exchange

`fastmm sim` runs the simulated exchange of `fastmm-engine[live]` on 127.0.0.1:9080: Binance's
API, a matching engine and its own order flow. `sim.toml` trades this strategy against it, with no
keys and no network:

```bash
pip install "fastmm-engine[live]"
fastmm sim &
python -m fastmm run strategy:@NAME_CLASS@ --config sim.toml --duration 60s
python -c "import glob, fastmm, strategy; print(fastmm.replay(max(glob.glob('runs/*.fmj')), strategy.@NAME_CLASS@))"
python backtest.py --config sim.toml --data "journal:$(ls -t runs/*.fmj | head -1),strip_own=1"
kill %1
```

The session logs every order and fill and writes its journal to `runs/`. `fastmm.replay` runs the
journal through the strategy again and checks every order against the recording; the backtest runs
the strategy over the session's market data, with the session's own orders taken out of the book.

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
    "sim.toml": SIM_TOML,
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


PROFILES = ("starter", "production")


def render(name: str, profile: str = "starter") -> dict[str, str]:
    """The project's files, keyed by relative path. The production profile adds production.toml
    and a section of the README about it."""
    if profile not in PROFILES:
        raise ValueError(f"unknown profile '{profile}' (one of {', '.join(PROFILES)})")
    templates = dict(TEMPLATES)
    if profile == "production":
        templates["production.toml"] = PRODUCTION_TOML
        templates["README.md"] = README_MD + README_PRODUCTION_MD
    class_name = _class_name(name)
    return {path: text.replace("@NAME_CLASS@", class_name).replace("@NAME@", name)
            for path, text in templates.items()}


def write_project(directory: os.PathLike, force: bool = False,
                  profile: str = "starter") -> list[Path]:
    """Write the project of `profile` into `directory`, which is created if it does not exist.

    Raises FileExistsError for a file that is already there unless `force`, and ValueError for an
    unknown profile.
    """
    target = Path(directory)
    files = render(_project_name(target), profile)
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
