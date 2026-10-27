# Quick start

A Python strategy, backtested on a simulated market and then run live against a local exchange. Nothing here needs an account or API keys.

The package on PyPI is `fastmm-engine`, imported as `fastmm`. It needs Linux x86-64 and CPython 3.10 or later; on macOS or Windows, use a container ([Install](install.md#without-building-it)). For C++, see the [Quick start in C++](cpp-quickstart.md).

## 1. A project and a backtest

```bash
python3 -m venv .venv && . .venv/bin/activate     # Debian, Ubuntu: sudo apt install python3-venv
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

```text
backtest py:MyMm  seed=1  md_events=7149  steps=12428  wall=0.02s
  net pnl                        -5.5272
  realized / unrealized / fees   0.5198 / 0.0005 / 6.0475
  ...
  fills (maker / taker)          1036 (1036 / 0)
  orders / cancels / replaces    2621 / 1622 / 0
  ...
net PnL -5.5272 = capture 0.5356 + mid drift -0.0152 - fees 6.0475
markout   1s: +0.8629 bps over 1029 fills
...
CSV and JSON: runs/backtest
```

The quotes earn 0.9 bps of spread but pay 10 bps in fees, so the run loses money. [Backtesting](../explanation/backtesting.md) explains how to read the decomposition.

`fastmm init` writes:

| File | What |
|---|---|
| `strategy.py` | `MyMm`, a `fastmm.Strategy` whose hooks are marked `@fastmm.hot` and compiled by Numba |
| `config.toml` | the simulated market the backtest runs on: instrument, fees, risk limits |
| `backtest.py` | runs the strategy on that market and prints the summary, the PnL decomposition and the markouts |
| `sim.toml` | the same strategy live against the simulated exchange on this machine |
| `live.toml` | the same strategy on Binance Spot Demo Mode |
| `README.md` | what to change first |

The backtest writes its fills, orders and equity curve to `runs/backtest/`. Two more commands sweep a parameter and draw the report:

```bash
python backtest.py --sweep half_spread_bps=0.005,0.01,0.02,0.05   # every value in one pass
fastmm report runs/backtest                                       # runs/backtest/report.html
```

The report is one HTML page: equity, inventory, where the PnL came from, markouts and fill quality ([Run report](../reference/run-report.md)).

## 2. Live against the simulated exchange

`fastmm-engine[live]` adds the venue connectors and `fastmm sim`, a Binance Spot-compatible exchange with its own order flow on 127.0.0.1:9080:

```bash
pip install "fastmm-engine[live]"
fastmm sim &
fastmm run strategy:MyMm --config sim.toml --duration 60s
```

The session sends real orders over WebSocket to the simulator and writes a journal to `runs/`. Ctrl-C stops it and cancels its orders.

## 3. Binance Demo

```bash
fastmm run strategy:MyMm --config live.toml --dry-run --duration 60s
```

`--dry-run` reads Binance Spot Demo Mode's market data and quotes on paper, with no keys. To send orders, export Demo Trading keys as `FASTMM_BINANCE_API_KEY` and `FASTMM_BINANCE_API_SECRET` and drop `--dry-run` ([Run on a testnet or Binance Demo](../how-to/operations/run-on-testnet.md)).

## Next

- [Write hot hooks in Python](../how-to/strategies/python-hot-hooks.md): parameters, hooks and the context.
- [Python](../python.md): backtests on recorded data, results as numpy and pandas, sweeps.
- [Run a Python strategy live](../how-to/strategies/python-live.md): the simulated exchange, then a venue.
- [Python strategy API](../reference/python-api.md).
