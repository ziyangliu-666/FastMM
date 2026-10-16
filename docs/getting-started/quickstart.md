# Quick start

The package on PyPI is `fastmm-engine`, imported as `fastmm`. `pip install fastmm` installs a different project.

The wheels need Linux x86-64 and CPython 3.10 or later; on macOS or Windows, run the steps in a container ([Install](install.md#without-building-it)). Nothing here needs an account or API keys. To write the strategy in C++ instead, see the [Quick start in C++](cpp-quickstart.md).

## 1. A project and a backtest

```bash
python3 -m venv .venv && . .venv/bin/activate     # Debian, Ubuntu: sudo apt install python3-venv
pip install "fastmm-engine[hot]"
fastmm init my-mm && cd my-mm
python backtest.py
```

`fastmm init` writes:

| File | What |
|---|---|
| `strategy.py` | `MyMm`, a `fastmm.Strategy` whose hooks are marked `@fastmm.hot` and compiled by Numba |
| `config.toml` | the simulated market the backtest runs on: instrument, fees, risk limits |
| `backtest.py` | runs the strategy on that market and prints the summary, the PnL decomposition and the markouts |
| `sim.toml` | the same strategy live against the simulated exchange on this machine |
| `live.toml` | the same strategy on Binance Spot Demo Mode |
| `README.md` | what to change first |

The backtest writes its fills, orders and equity curve to `runs/backtest/`. Two more commands:

```bash
python backtest.py --sweep half_spread_bps=0.005,0.01,0.02,0.05   # one backtest per value
fastmm report runs/backtest                                       # runs/backtest/report.html
```

The report is one HTML page: equity, inventory, where the PnL came from, markouts and fill quality ([Run report](../reference/run-report.md)). The quotes earn less than the fees at the default fee level; [Backtesting](../explanation/backtesting.md) explains how to read the decomposition.

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
