# Python

The `fastmm` package runs strategies written in Python inside the C++ engine: in backtests, in replays of their journals and, with `fastmm-engine-live`, against venues. It also backtests the C++ strategies. Install: `pip install "fastmm-engine[hot]"`, plus the `live` extra for venues ([Install](getting-started/install.md#python)).

## What runs where

A strategy class uses one of two styles. Hot hooks (`@fastmm.hot`) are compiled with Numba and called by the engine thread without the GIL; slow methods beside them are plain Python on another thread and change the hooks' parameters. Plain hooks are `fastmm.Strategy` methods without `@fastmm.hot`, called by the engine thread with the GIL held.

| Style | Backtest (`run_backtest`) | Replay (`fastmm.replay`) | Live (`run_live`, `python -m fastmm run`) |
|---|---|---|---|
| hot hooks | yes | yes | yes |
| slow methods beside hot hooks (`on_start`, `on_stop`, `@fastmm.every`) | yes | no; the recorded parameter updates are applied | yes |
| plain hooks | yes | no | no |

## First strategy

`fastmm init my-mm` writes a starter project: a config for the simulated market, a strategy with hot hooks, a backtest that runs it, a config that trades it live against the simulated exchange `fastmm sim` runs, and one that runs it on Binance Spot Demo Mode, the Spot testnet or the real market (`--venue`; [Install](getting-started/install.md#without-building-it)).

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

`from_toml` raises `ConfigError` for an unknown key or section, with its line, and issues a `UserWarning` for each other warning, listed in `cfg.warnings`. `BacktestConfig.single_instrument("BTCUSDT", tick="0.01", lot="0.00001")` builds a config by hand, with the synthetic market a `[sim]` section without keys gives; `cfg.set_market(spread_bps=4.0, mid_step_bps=0.5, depth_bps=1.0)` reshapes it after `start_mid`. Other fields: `strategy`, `params`, `engine_seed`, `start_ns`, `equity_bar_s`, `initial_capital`, `queue_conservatism`, `latency_fixed_us`, `latency_jitter_us`, `latency_md_us`, `latency_md_jitter_us`, `p_drop`, `maker_fee_bps`, `taker_fee_bps`, `markout_horizons_s`, `supports_replace`, `start_mid`, `limit_rate_per_s`, `market_rate_per_s`, `mid_step_rate_per_s`, `cancel_rate_per_order_s`, `max_param_age_ms`, `source`, `path`, `output_dir`, `journal_out`, `measure_wall_clock`.

### Data

| `data=` | source |
|---|---|
| `None` | the config's `[backtest] source / path` |
| `"synthetic"` | the seeded synthetic market |
| `"x.fmj"` / `"x.csv"` (str or `os.PathLike`) | journal / CSV (`ts_ns,type,inst,side,price,qty,seq`) |
| `"binance:BTCUSDT,2024-03-27"` | a registered source and its options; `fastmm.data_sources()` lists them ([Market-data sources](reference/data-sources.md)) |
| `dict` of numpy arrays | `ArraySource`, zero copy |

`fastmm.data.binance.fetch(symbol, dates)` and `fastmm.data.tardis.fetch(exchange, symbol, dates)` download public archives into the cache, `python3 -m fastmm.data fetch` does it from a shell, and `fastmm.convert_data(spec, out, config)` packs a source into an `.fmj` for faster repeated replays ([Backtest on real BTCUSDT data](how-to/backtesting/binance-public-data.md)).

Array columns: `ts` int64 (ns), `type` uint8 (0 snapshot level, 1 delta level, 2 trade, 3 book ticker), `inst` uint32, `side` int8 (0 bid/buy, 1 ask/sell), `price` and `qty` int64 (raw 1e-8) or float64, optional `seq` uint64. Dtypes must match exactly and arrays must be 1-D, C-contiguous, aligned and native-endian: a float32 column raises `TypeError` and a strided slice raises `ValueError` instead of being copied. `fastmm.load_csv(path)` reads a CSV into int64 columns (identical outbound hash to running the file by path).

The GIL is released while a C++ strategy or hot hooks run, so several runs can proceed in Python threads. A strategy with plain hooks holds the GIL for its whole run.

### Real BTC/USDT spot data

The public archives have no Binance spot book, so record one: a `--dry-run` session reads Binance's public streams without keys, quotes on paper and journals the book and the trades it saw. The journal is the backtest's data.

```bash
pip install "fastmm-engine[hot,live]"
fastmm init btc --venue binance && cd btc
python -m fastmm run strategy:Btc --config live.toml --dry-run --duration 1h
python backtest.py --config live.toml --data "$(ls -t runs/*.fmj | head -1)"
```

`--venue binance` writes a `live.toml` for Binance Spot itself, with `[backtest] fill_model = "l2_queue"` for the replay. From Python, the same run is `fastmm.run_backtest(fastmm.BacktestConfig.from_toml("live.toml"), data="runs/<file>.fmj", strategy=Btc)`, and `fastmm.sweep` and `fastmm.walk_forward` take the journal as `data=` too.

| Data | Market | Book | Free | Suits |
|---|---|---|---|---|
| `--dry-run` journal of `--venue binance` | Binance spot, any symbol in the config | depth, trades, book ticker | yes, as long as you record | quoting at and behind the touch |
| `tardis:binance,BTCUSDT,<date>` ([Tardis](reference/data-sources.md#tardis)) | Binance spot and the other venues Tardis covers | full L2, trades | the 1st of each month | quoting several levels deep, on days you did not record |
| `binance:BTCUSDT,<date>` ([data.binance.vision](how-to/backtesting/binance-public-data.md)) | USDⓈ-M perpetual, not spot | top of book only, 2023-05-16 to 2024-03-30 | yes | quoting one level at the touch |
| `binance:BTCUSDT,<date>,market=spot` | spot | none, trades only | yes | nothing that quotes: there is no book to fill against |
| `--dry-run` journal of `--venue binance-demo` or `binance-testnet` | Demo Mode, Spot testnet | depth, trades | yes | the live session's plumbing; Demo Mode's prices are realistic, not real, and the testnet's book is its own |

The archive sources read what `python -m fastmm.data fetch` downloaded, for Tardis spot `python -m fastmm.data fetch --source tardis --exchange binance --symbol BTCUSDT --date 2026-10-01`.

### Results

`r.fills`, `r.equity` and `r.orders` are dicts of read-only numpy views over the C++ result vectors (no copy; the arrays keep the result alive). Prices, quantities, fees and PnL are raw int64 with a 1e-8 scale (`fastmm.FIXED_SCALE`); timestamps are int64 ns. `to_pandas()` converts to floats and `datetime64[ns]`. `stats()` returns the summary metrics; `sharpe_annualized` is NaN for runs shorter than 1 day and `max_drawdown_pct` is NaN without `initial_capital` ([`[backtest]`](reference/configuration.md#backtest)). When the kill switch stopped the run, `kill_reason` names why (`MaxLoss` for `[risk] max_loss`) and `kill_at_s` when, in seconds of simulated time; both are None otherwise, and the summary table opens with a `stopped` line. `engine_stats()` and `transport_stats()` return the component counters; `write_all(dir)` writes the same CSV/JSON files as `fastmm-backtest`.

`fastmm.write_report(r, "report.html")` writes the run as one self-contained HTML page, and `fastmm report <run-dir | journal.fmj>` does the same from a shell ([Run report](reference/run-report.md)).

`markouts()` returns one dict per horizon of `cfg.markout_horizons_s` with the buckets `total`, `buy`, `sell`, `maker`, `taker` and `instrument`, each holding `markout`, `capture` and `adverse_selection` in quote currency and in bps of notional, plus `excluded_past_end` and `excluded_no_mid` for the fills that could not be marked. `fastmm.markout_frame(result)` puts the same rows in a DataFrame, and `stats()` carries the PnL decomposition (`spread_capture`, `mid_drift`, `fees_paid`, `rebates_received`, `decomposition_net`, `decomposition_residual`) and the fill-quality diagnostics. `r.fills` gains `best_bid`, `best_ask`, `queue_ahead` (-1 when the fill model has no queue position) and one `markout_mid_<ns>ns` column per horizon, 0 where the fill could not be marked ([Backtesting](explanation/backtesting.md)).

### Names in each output

`stats()` and `summary.json` share the metric names, except that `summary.json` nests the PnL decomposition under `pnl_decomposition` and the markouts under `markouts`, where `stats()` flattens the first and leaves the second to `markouts()`. The CSV files name columns as `fastmm-backtest` writes them, the numpy dicts as the C++ result fields, and `to_pandas()` adds derived columns:

| Quantity | `stats()` | `summary.json` | CSV | `r.fills` / `r.equity` / `r.orders` | `to_pandas()` |
|---|---|---|---|---|---|
| time | | `start_ts` / `end_ts` | `ts_ns` (orders: also `venue_ts_ns`, `trigger_ts_ns`) | `ts` (orders: also `venue_ts`, `trigger_ts`) | index `ts`, datetime64 |
| instrument | | | `inst` | `instrument` | `instrument` |
| side, liquidity | | | `B` / `S`, `M` / `T` | 0 / 1, 1 / 2 | categorical `buy` / `sell`, `maker` / `taker` |
| prices, quantities, PnL | float | float | exact decimals | raw int64, 1e-8 | float |
| mid at a markout horizon | | | `mid_1s`, `mid_10s`, `mid_1m` | `markout_mid_<ns>ns`, 0 when unmarked | `mid_1s` (NaN when unmarked) and `markout_1s` |
| equity | | | `equity` | `realized + unrealized - fees` | `equity` |
| quotes resting | | | `quoted` (bit 0 bid, bit 1 ask) | `quoted` | `bid_quoted`, `ask_quoted` |
| decomposition: capture, drift, fees, rebates | `spread_capture`, `mid_drift`, `fees_paid`, `rebates_received` | `pnl_decomposition.` the same | | | |
| decomposition: sum, residual, notional | `decomposition_net`, `decomposition_residual`, `traded_notional` | `pnl_decomposition.net`, `.residual`, `.notional` | | | |

`spread_captured_bps` (summary table: `spread captured (bps)`) is the mean over fills of each fill's capture in bps of its mid; `spread_capture_bps` is the decomposition's total capture over the notional of the fills that had a mid. Both are in `stats()`; `summary.json` has the first at the top level and the second under `pnl_decomposition`.

## Sweeps

```python
points = fastmm.sweep(cfg, {"half_spread_bps": [0.01, 0.02], "skew_bps_per_unit": [0, 0.01]},
                      data="synthetic", threads=0)
df = fastmm.sweep_frame(points)           # one row per grid point
```

Results come back in grid order with the first parameter varying slowest, each the result `run_backtest` gives for that point without its order rows (`result.orders` is empty; the metrics count the orders and the outbound hash covers them, and `run_backtest` of the point has them). Under `fill_model = "l2_queue"` or `"l3_queue"` the points of a registered C++ strategy, or of a `fastmm.Strategy` class whose hooks are `@fastmm.hot`, share their passes over the data on C++ threads with the GIL released ([Many runs in one pass](explanation/backtesting.md#many-runs-in-one-pass)). A class with plain hooks, and any strategy under `matching`, runs one backtest per point: a C++ strategy on a C++ thread pool, a Python class as a fresh instance in worker processes forked from the calling one, so a class defined in a script or a notebook works:

```python
points = fastmm.sweep(cfg, {"half_spread_bps": [1.0, 2.0, 4.0]}, data="synthetic",
                      strategy=MyMM, threads=4)
```

`threads` is the number of threads or processes; 0 uses every CPU and 1 runs the points one after another in the calling process. Without `fork` (Windows) the points of a plain Python strategy run in the calling process.

`verify=K` runs K points, spread over the grid, again as backtests of their own and raises `RuntimeError` naming the first thing that differs (a fill, an order, an equity bar, a count or the outbound hash).

### Walk-forward

The best point of a sweep is the luckiest one on that data. `fastmm.walk_forward` scores it on data it was not chosen on:

```python
cfg = fastmm.BacktestConfig.from_toml("configs/backtest-example.toml")  # fill_model l2_queue
cfg.duration_s = 240
cfg.maker_fee_bps = 0.0
grid = {"half_spread_bps": [0.005, 0.01, 0.02, 0.04], "skew_bps_per_unit": [0.0, 0.01]}
rep = fastmm.walk_forward(cfg, grid, folds=4, data="synthetic", metric="net_pnl")
print(rep["table"])
```

The event-time range of `data` is cut into `folds` equal consecutive slices and the whole grid runs on each slice, every run a fresh backtest from a flat engine. A slice starts on the book the full stream had at its first instant (earlier events are read to build it, not traded) and ends at the next slice. For fold i ≥ 1, the best point of fold i−1 by `metric` (`net_pnl`, `realized_pnl`, `sharpe_bar` or `spread_captured_bps`; ties go to the earlier grid point) is the choice, scored on fold i. The choice uses fold i−1 alone rather than every earlier fold: the grid already ran there, the rule works for metrics that do not add up across folds (a Sharpe ratio), and it is what re-running the sweep on the latest data before trading does.

```text
walk-forward: 4 folds, 8 points, metric net_pnl, chosen on the previous fold
fold               time_s  chosen     in_sample  out_of_sample    best     hindsight
   0         0.000-60.000       -             -              -      #1     0.0130904
   1       60.000-120.000      #1     0.0130904       0.150667      #2      0.197885
   2      120.000-180.000      #2      0.197885       0.173777      #2      0.173777
   3      180.000-240.000      #2      0.173777      0.0302026      #1     0.0317501
  #1  half_spread_bps=0.005 skew_bps_per_unit=0.01
  #2  half_spread_bps=0.01 skew_bps_per_unit=0.0
mean in-sample best   0.128251
mean out-of-sample    0.118216
mean hindsight best   0.134471
choice changes        1 of 2
```

`chosen` and `best` are grid points (`#n` in grid order, listed under the table): the point carried over from the previous fold and the one that scores best on this fold in hindsight. `in_sample` is the chosen point's score on the fold it was chosen on, `out_of_sample` its score on this fold, `hindsight` the best score on this fold. The summary averages folds 1 to K−1 and counts how often the choice changed between consecutive folds. The choice generalizes when the out-of-sample mean stays close to the hindsight mean and the choice rarely changes; an out-of-sample mean well below both, or a choice that changes on every fold, means the ranking fits noise.

`folds=1` runs exactly `fastmm.sweep`. `strategy=` takes a `fastmm.Strategy` class here too, which runs every fold and point in worker processes. The result does not depend on `threads`. The synthetic market needs `fill_model = "l2_queue"` for more than one fold: the coupled matching market has no stream to cut. The dict also holds every fold's `points` and `scores`; the C++ entry point is `bt::walk_forward` in `backtest/sweep.hpp`.

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
- `fastmm.features(data=..., horizons=[...])`: book features and forward mid moves from a data source, as zero-copy numpy columns, and `fastmm.evaluate_signal(table, values)` for the information coefficient, bucket table and conditional touch markout of a signal ([Judging a signal before writing a strategy](explanation/signal-research.md)).
- `fastmm.inspect_journal(path)`: header and message counts of an `.fmj`.
- `fastmm.open_store(path)`: the fills, orders, positions, PnL, funding payments and kill events a live session recorded, as DataFrames ([Query what you traded](how-to/operations/query-trading-records.md)). Needs pandas.

## Logging

The C++ engine logs through an asynchronous logger that has no output until it is started. From Python, call `fastmm.enable_logging(level="warn", path=None)` to write records at `level` or above to a file (appended) or to stderr; warnings and errors are always mirrored to stderr as well. `fastmm.disable_logging()` flushes and stops it; interpreter exit does the same.

Development install, tests, the type stub and wheels: [Python packages](contributing/python-packages.md).
