# Python

The `fastmm` package runs strategies written in Python inside the C++ engine: in backtests, in replays of their journals and, with `fastmm-engine-live`, against venues. It also backtests the C++ strategies. Install: [Python](getting-started/install.md#python).

## What runs where

A strategy class uses one of two styles. Hot hooks (`@fastmm.hot`) are compiled with Numba and called by the engine thread without the GIL; slow methods beside them are plain Python on another thread and change the hooks' parameters. Plain hooks are `fastmm.Strategy` methods without `@fastmm.hot`, called by the engine thread with the GIL held.

| Style | Backtest (`run_backtest`) | Replay (`fastmm.replay`) | Live (`run_live`, `python -m fastmm run`) |
|---|---|---|---|
| hot hooks | yes | yes | yes |
| slow methods beside hot hooks (`on_start`, `on_stop`, `@fastmm.every`) | yes | no; the recorded parameter updates are applied | yes |
| plain hooks | yes | no | no |

## First strategy

Write it as hot hooks, the only style that trades live:

1. [Write hot hooks in Python](how-to/strategies/python-hot-hooks.md): parameters, hooks and a backtest.
2. [Run slow methods beside hot hooks](how-to/strategies/python-slow-methods.md): a model in plain Python that publishes parameters.
3. [Run a Python strategy live](how-to/strategies/python-live.md): the simulated exchange, then a venue.

`examples/python/strategies/basic_mm_hot.py` ports the C++ `basic_mm` to hot hooks with the same outbound hash, and `hot_slow_mm.py` adds a slow model:

```bash
python examples/python/strategies/basic_mm_hot.py
```

Reference: [Python strategy API](reference/python-api.md).

## Running a backtest

```python
import fastmm

cfg = fastmm.BacktestConfig.from_toml("configs/backtest-example.toml")
cfg.seed = 7
cfg.duration_s = 300
cfg.fill_model = "matching"            # or "l2_queue"
cfg.set_param("half_spread_bps", 0.02)  # str / int / float / bool

r = fastmm.run_backtest(cfg, data="synthetic")
print(r.summary_table())
r.stats()["net_pnl"], r.outbound_sha256
frames = r.to_pandas()                  # {"fills", "equity", "orders", "markouts"} DataFrames
```

`from_toml` issues a `UserWarning` for each unknown key or section, with its line, and lists them in `cfg.warnings`. `BacktestConfig.single_instrument("BTCUSDT", tick="0.01", lot="0.00001")` builds a config by hand. Other fields: `strategy`, `params`, `engine_seed`, `start_ns`, `equity_bar_s`, `initial_capital`, `queue_conservatism`, `latency_fixed_us`, `latency_jitter_us`, `latency_md_us`, `latency_md_jitter_us`, `p_drop`, `maker_fee_bps`, `taker_fee_bps`, `markout_horizons_s`, `supports_replace`, `start_mid`, `limit_rate_per_s`, `market_rate_per_s`, `mid_step_rate_per_s`, `cancel_rate_per_order_s`, `source`, `path`, `output_dir`, `journal_out`, `measure_wall_clock`.

### Data

| `data=` | source |
|---|---|
| `None` | the config's `[backtest] source / path` |
| `"synthetic"` | the seeded synthetic market |
| `"x.fmj"` / `"x.csv"` (str or `os.PathLike`) | journal / CSV (`ts_ns,type,inst,side,price,qty,seq`) |
| `dict` of numpy arrays | `ArraySource`, zero copy |

Array columns: `ts` int64 (ns), `type` uint8 (0 snapshot level, 1 delta level, 2 trade, 3 book ticker), `inst` uint32, `side` int8 (0 bid/buy, 1 ask/sell), `price` and `qty` int64 (raw 1e-8) or float64, optional `seq` uint64. Dtypes must match exactly and arrays must be 1-D, C-contiguous, aligned and native-endian: a float32 column raises `TypeError` and a strided slice raises `ValueError` instead of being copied. `fastmm.load_csv(path)` reads a CSV into int64 columns (identical outbound hash to running the file by path).

The GIL is released while a C++ strategy or hot hooks run, so several runs can proceed in Python threads. A strategy with plain hooks holds the GIL for its whole run.

### Results

`r.fills`, `r.equity` and `r.orders` are dicts of read-only numpy views over the C++ result vectors (no copy; the arrays keep the result alive). Prices, quantities, fees and PnL are raw int64 with a 1e-8 scale (`fastmm.FIXED_SCALE`); timestamps are int64 ns. `to_pandas()` converts to floats and `datetime64[ns]`. `stats()` returns the summary metrics; `sharpe_annualized` is NaN for runs shorter than 1 day and `max_drawdown_pct` is NaN without `initial_capital` ([`[backtest]`](reference/configuration.md#backtest)). `engine_stats()` and `transport_stats()` return the component counters; `write_all(dir)` writes the same CSV/JSON files as `fastmm-backtest`.

`markouts()` returns one dict per horizon of `cfg.markout_horizons_s` with the buckets `total`, `buy`, `sell`, `maker`, `taker` and `instrument`, each holding `markout`, `capture` and `adverse_selection` in quote currency and in bps of notional, plus `excluded_past_end` and `excluded_no_mid` for the fills that could not be marked. `fastmm.markout_frame(result)` puts the same rows in a DataFrame, and `stats()` carries the PnL decomposition (`spread_capture`, `mid_drift`, `fees_paid`, `rebates_received`, `decomposition_net`, `decomposition_residual`) and the fill-quality diagnostics. `r.fills` gains `best_bid`, `best_ask`, `queue_ahead` (-1 when the fill model has no queue position) and one `markout_mid_<ns>ns` column per horizon, 0 where the fill could not be marked ([Backtesting](explanation/backtesting.md)).

## Sweeps

```python
points = fastmm.sweep(cfg, {"half_spread_bps": [0.01, 0.02], "skew_bps_per_unit": [0, 0.01]},
                      data="synthetic", threads=0)
df = fastmm.sweep_frame(points)           # one row per grid point
```

Points run on a C++ thread pool (GIL released), every worker with its own cursor over `data`; results come back in grid order with the first parameter varying slowest.

## Plain hooks

Subclass `fastmm.Strategy`, define the hooks you need and pass the class as `strategy=`. The class runs in backtests only ([What runs where](#what-runs-where)):

```python
class Joiner(fastmm.Strategy):
    qty = fastmm.Param(0.002, min=0.0, doc="size per side")

    def on_book(self, ctx, inst, book):
        if book.valid:
            ctx.set_quotes(inst, [(book.best_bid[0], self.qty)], [(book.best_ask[0], self.qty)])

cfg.clear_params()                     # the example file configures basic_mm
r = fastmm.run_backtest(cfg, data="synthetic", strategy=Joiner, params={"qty": 0.001})
```

`examples/python/strategies/basic_mm_exact.py` is an integer port of the C++ BasicMM with the same outbound hash; `skew_mm.py` is a float market maker.

## Other helpers

- `fastmm.strategies()`: `{name: [{"name", "type", "default", "min", "max", "doc"}]}`.
- `fastmm.OrderBook()`: L2 book (256 levels/side) with `apply_snapshot(bids, asks)`, `apply_delta(bids, asks)` (`(n, 2)` arrays or `(price, qty)` pairs, qty 0 deletes), `best_bid()`, `best_ask()`, `mid()`, `spread()`, `microprice()`, `weighted_mid(levels)`, `imbalance(levels)`, `bids(n)`, `asks(n)`.
- `fastmm.inspect_journal(path)`: header and message counts of an `.fmj`.
- `fastmm.open_store(path)`: the fills, orders, positions, PnL and kill events a live session recorded, as DataFrames ([Query what you traded](how-to/operations/query-trading-records.md)). Needs pandas.

## Logging

The C++ engine logs through an asynchronous logger that has no output until it is started. From Python, call `fastmm.enable_logging(level="warn", path=None)` to write records at `level` or above to a file (appended) or to stderr; warnings and errors are always mirrored to stderr as well. `fastmm.disable_logging()` flushes and stops it; interpreter exit does the same.

Development install, tests, the type stub and wheels: [Python packages](contributing/python-packages.md).
