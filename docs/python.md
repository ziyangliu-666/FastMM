# Python

The `fastmm` package runs Python strategies inside the C++ engine: in backtests, in replays of their journals and, with `fastmm-engine-live`, against venues. It also backtests the C++ strategies. Install: `pip install "fastmm-engine[hot]"`, plus the `live` extra for venues ([Install](getting-started/install.md#python)).

## What runs where

A strategy class uses one of two styles:

| Style | Runs as | Backtest | Replay | Live |
|---|---|---|---|---|
| hot hooks, `@fastmm.hot` | compiled by Numba, called on the engine thread without the GIL | yes | yes | yes |
| slow methods beside hot hooks: `on_start`, `on_stop`, `@fastmm.every` | plain Python on another thread; they change the hooks' parameters | yes | the recorded parameter updates are applied | yes |
| plain hooks, no decorator | plain Python on the engine thread, with the GIL | yes | no | no |

The two styles share hook names, not arguments. A plain hook is handed the instrument, `on_book(self, ctx, inst, book)` ([Python strategy API](reference/python-api.md#hooks)); a hot hook reads it from the book, `on_book(self, ctx, book)` ([Hot hooks](how-to/strategies/python-hot-hooks.md)). Only hot hooks share a sweep's passes over the data ([Sweeps](#sweeps)).

## First strategy

`fastmm init my-mm` writes a starter project with hot hooks, a backtest and configs for the local simulated exchange and Binance ([Quick start](getting-started/quickstart.md)). Hot hooks are the only style that trades live:

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

| Build a config | Does |
|---|---|
| `BacktestConfig.from_toml(path)` | reads a TOML file; raises `ConfigError` with the line for an unknown key, and a `UserWarning` for each other warning, kept in `cfg.warnings` |
| `BacktestConfig.single_instrument("BTCUSDT", tick="0.01", lot="0.00001")` | one instrument on the default synthetic market |
| `cfg.set_market(spread_bps=4.0, mid_step_bps=0.5, depth_bps=1.0)` | reshapes the synthetic market after `start_mid` |

Every field of `BacktestConfig`, from latency and fees to the output paths, is in the [Python API](api/python.md).

### Data

| `data=` | source |
|---|---|
| `None` | the config's `[backtest] source / path` |
| `"synthetic"` | the seeded synthetic market |
| `"x.fmj"` / `"x.csv"` (str or `os.PathLike`) | journal / CSV (`ts_ns,type,inst,side,price,qty,seq`) |
| `"binance:BTCUSDT,2024-03-27"` | a registered source and its options; `fastmm.data_sources()` lists them ([Market-data sources](reference/data-sources.md)) |
| `dict` of numpy arrays | `ArraySource`, zero copy |

| Helper | Does |
|---|---|
| `fastmm.data.binance.fetch(symbol, dates)`, `fastmm.data.tardis.fetch(exchange, symbol, dates)` | download public archives into the cache |
| `python3 -m fastmm.data fetch` | the same from a shell |
| `fastmm.convert_data(spec, out, config)` | packs a source into an `.fmj` for faster repeated runs |

Downloading and running Binance's archives: [Backtest on real BTCUSDT data](how-to/backtesting/binance-public-data.md).

An array source takes these columns, each 1-D, C-contiguous, aligned and native-endian:

| Column | Dtype | Values |
|---|---|---|
| `ts` | int64 | ns |
| `type` | uint8 | 0 snapshot level, 1 delta level, 2 trade, 3 book ticker |
| `inst` | uint32 | instrument index |
| `side` | int8 | 0 bid or buy, 1 ask or sell |
| `price`, `qty` | int64 raw 1e-8, or float64 | |
| `seq` | uint64, optional | |

A wrong dtype raises `TypeError` and a strided slice `ValueError`; nothing is copied. `fastmm.load_csv(path)` reads a CSV into int64 columns, with the same outbound hash as running the file by path.

The GIL is released while a C++ strategy or hot hooks run, so several runs can proceed in Python threads. A strategy with plain hooks holds the GIL for its whole run.

### Real BTC/USDT spot data

The public archives have no Binance spot book, so record one: a `--dry-run` session reads Binance's public streams without keys, quotes on paper and journals the book and the trades it saw. The journal is the backtest's data.

```bash
pip install "fastmm-engine[hot,live]"
fastmm init btc --venue binance && cd btc
python -m fastmm run strategy:Btc --config live.toml --dry-run --duration 1h
python backtest.py --config live.toml --data "$(ls -t runs/*.fmj | head -1)"
```

`--venue binance` writes a `live.toml` for Binance Spot itself, with `[backtest] fill_model = "l2_queue"` for the replay. The same run from Python, where `fastmm.sweep` and `fastmm.walk_forward` take the journal as `data=` too:

```python
cfg = fastmm.BacktestConfig.from_toml("live.toml")
r = fastmm.run_backtest(cfg, data="runs/<file>.fmj", strategy=Btc)
```

| Data | Market | Book | Free | Suits |
|---|---|---|---|---|
| `--dry-run` journal of `--venue binance` | Binance spot, any symbol in the config | depth, trades, book ticker | yes, as long as you record | quoting at and behind the touch |
| `tardis:binance,BTCUSDT,<date>` ([Tardis](reference/data-sources.md#tardis)) | Binance spot and the other venues Tardis covers | full L2, trades | the 1st of each month | quoting several levels deep, on days you did not record |
| `binance:BTCUSDT,<date>` ([data.binance.vision](how-to/backtesting/binance-public-data.md)) | USDⓈ-M perpetual, not spot | top of book only, 2023-05-16 to 2024-03-30 | yes | quoting one level at the touch |
| `binance:BTCUSDT,<date>,market=spot` | spot | none, trades only | yes | nothing that quotes: there is no book to fill against |
| `--dry-run` journal of `--venue binance-demo` or `binance-testnet` | Demo Mode, Spot testnet | depth, trades | yes | the live session's plumbing; Demo Mode's prices are realistic, not real, and the testnet's book is its own |

The archive sources read what `python -m fastmm.data fetch` downloaded, for Tardis spot:

```bash
python -m fastmm.data fetch --source tardis --exchange binance --symbol BTCUSDT --date 2026-10-01
```

### Results

A `BacktestResult` gives the run in several shapes:

| Member | Gives |
|---|---|
| `r.fills`, `r.equity`, `r.orders` | dicts of read-only numpy views over the C++ results, no copy; money is raw int64 at 1e-8 (`fastmm.FIXED_SCALE`), time int64 ns |
| `r.to_pandas()` | the same as DataFrames, in floats and `datetime64[ns]` |
| `r.stats()` | the summary metrics, the PnL decomposition and the fill-quality diagnostics |
| `r.markouts()` | one dict per horizon of `cfg.markout_horizons_s`; `fastmm.markout_frame(r)` as a DataFrame |
| `r.engine_stats()`, `r.transport_stats()` | the component counters |
| `r.write_all(dir)` | the CSV and JSON files `fastmm-backtest` writes |
| `fastmm.write_report(r, "report.html")` | one HTML page; `fastmm report <run-dir or journal.fmj>` from a shell ([Run report](reference/run-report.md)) |

Some metrics can be empty:

| Metric | Empty when |
|---|---|
| `sharpe_annualized` | NaN for a run more than one equity bar short of a day |
| `max_drawdown_pct` | NaN without `initial_capital` ([`[backtest]`](reference/configuration.md#backtest)) |
| `kill_reason`, `kill_at_s` | None unless the kill switch stopped the run; then the reason, such as `MaxLoss`, and the simulated time in s, and the summary table opens with a `stopped` line |

Each markout horizon has the buckets `total`, `buy`, `sell`, `maker`, `taker` and `instrument`. A bucket holds `markout`, `capture` and `adverse_selection` in quote currency and in bps of notional, and counts the fills that could not be marked in `excluded_past_end` and `excluded_no_mid`.

`r.fills` adds `best_bid`, `best_ask`, `queue_ahead` and one `markout_mid_<ns>ns` column per horizon. `queue_ahead` is -1 when the fill model has no queue position, and a mid is 0 where the fill could not be marked ([Backtesting](explanation/backtesting.md)).

### Names in each output

The same quantity has a different name in each output. `summary.json` nests the PnL decomposition under `pnl_decomposition` and the markouts under `markouts`; `stats()` flattens the first and leaves the second to `markouts()`.

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

Two spread metrics have near names:

| Metric | Is | In `summary.json` |
|---|---|---|
| `spread_captured_bps`, the summary table's `spread captured (bps)` | the mean over fills of each fill's capture, in bps of its mid | top level |
| `spread_capture_bps` | the decomposition's total capture over the notional of the fills that had a mid | under `pnl_decomposition` |

## Sweeps

```python
points = fastmm.sweep(cfg, {"half_spread_bps": [0.01, 0.02], "skew_bps_per_unit": [0, 0.01]},
                      data="synthetic", threads=0)
df = fastmm.sweep_frame(points)           # one row per grid point
```

A row of `sweep_frame` holds the point's values, the `stats()` columns of `fastmm.results.SWEEP_STATS` or of `stats=[...]`, the markout of each horizon in bps (`markout_bps_1s`, ...) and the outbound hash.

A grid can also be a list of points, `[{"half_spread_bps": 0.01, "latency_fixed_us": 2000}, ...]`, and a name can be a backtest setting such as a latency or a fee ([Many runs in one pass](explanation/backtesting.md#many-runs-in-one-pass) lists them). Results come back in the order of the points, a grid's first name varying slowest. Each is the result `run_backtest` gives for that point, without its order rows; the metrics and the outbound hash still cover the orders.

How the points run depends on the strategy and the fill model:

| Strategy | `l2_queue` or `l3_queue` | `matching` |
|---|---|---|
| registered C++ strategy | the points share their passes over the data, on C++ threads, GIL released | one backtest per point on a C++ thread pool |
| `fastmm.Strategy` with `@fastmm.hot` hooks | shared passes, as above | one backtest per point in worker processes |
| `fastmm.Strategy` with plain hooks | one backtest per point in worker processes | the same |

Worker processes fork from the calling one, so a class defined in a script or a notebook works. Without `fork`, on Windows, the points run in the calling process:

```python
points = fastmm.sweep(cfg, {"half_spread_bps": [1.0, 2.0, 4.0]}, data="synthetic",
                      strategy=MyMM, threads=4)
```

| Argument | Effect |
|---|---|
| `threads` | threads or processes; 0 uses every CPU, 1 runs the points one after another in the calling process |
| `verify=K` | reruns K points spread over the grid as backtests of their own; raises `RuntimeError` naming the first fill, order, bar, count or hash that differs |
| `summary_only=True` | keeps each point's metrics, counts and markouts, which `sweep_frame` reads, and drops its fills and equity bars; hundreds of points over hours of data otherwise hold gigabytes |

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

The data's time range is cut into `folds` equal consecutive slices, and the whole grid runs on each slice as fresh backtests from a flat engine. A slice starts on the book the full stream had at its first instant, and on the orders resting then when the data has every order.

For each fold after the first, the choice is the best point of the previous fold by `metric`, scored on this fold. `metric` is `net_pnl`, `realized_pnl`, `sharpe_bar`, `spread_captured_bps` or a function of a `BacktestResult`; higher is better, and ties go to the earlier point. Choosing on the previous fold alone works for metrics that do not add up across folds, such as a Sharpe ratio, and matches re-running the sweep on the latest data before trading.

```text
walk-forward: 4 folds, 8 points, metric net_pnl, chosen on the previous fold
fold  from (UTC)               chosen     in_sample  out_of_sample    best     hindsight
   0  2023-11-14 22:13:20.000       -             -              -      #1     0.0139937
   1  2023-11-14 22:14:20.000      #1     0.0139937       0.136523      #2      0.179756
   2  2023-11-14 22:15:20.000      #2      0.179756       0.156102      #2      0.156102
   3  2023-11-14 22:16:20.000      #2      0.156102      0.0280221      #2     0.0280221
  #1  half_spread_bps=0.005 skew_bps_per_unit=0.01
  #2  half_spread_bps=0.01 skew_bps_per_unit=0.0
mean in-sample best   0.116617
mean out-of-sample    0.106882
mean hindsight best   0.121293
choice changes        1 of 2
```

| Column | Is |
|---|---|
| `from` | the fold's first instant in the data's time |
| `chosen` | the point carried over from the previous fold, `#n` in grid order as listed under the table |
| `in_sample` | the chosen point's score on the fold it was chosen on |
| `out_of_sample` | its score on this fold |
| `best`, `hindsight` | the point that scores best on this fold, and its score |

The summary averages folds 1 to K−1 and counts how often the choice changed. The choice generalizes when the out-of-sample mean stays close to the hindsight mean and the choice rarely changes. An out-of-sample mean well below both, or a choice that changes every fold, means the ranking fits noise.

`folds=1` is `fastmm.sweep`. `strategy=` takes a `fastmm.Strategy` class, run in worker processes, and the result does not depend on `threads`. The synthetic market needs `fill_model = "l2_queue"` for more than one fold, because the coupled matching market has no stream to cut. The returned dict also holds every fold's `points` and `scores`; from C++ it is `bt::walk_forward`.

## Plain hooks

A `fastmm.Strategy` subclass without `@fastmm.hot` runs in backtests only ([What runs where](#what-runs-where)). Define the hooks you need and pass the class as `strategy=`:

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

| Helper | Gives |
|---|---|
| `fastmm.strategies()` | the registered strategies and their parameters: name, type, default, min, max, doc |
| `fastmm.OrderBook()` | an L2 book of 256 levels per side with `apply_snapshot`, `apply_delta`, `mid()`, `microprice()`, `imbalance(levels)` and more ([Python API](api/python.md)) |
| `fastmm.features(data=..., horizons=[...])` | book features and forward mid moves as numpy columns; `fastmm.evaluate_signal(table, values)` scores a signal ([Judging a signal](explanation/signal-research.md)) |
| `fastmm.inspect_journal(path)` | the header and message counts of an `.fmj` |
| `fastmm.open_store(path)` | a live session's fills, orders, positions, PnL, funding and kills as DataFrames ([Query what you traded](how-to/operations/query-trading-records.md)) |

## Logging

The engine's log is silent until started. `fastmm.enable_logging(level="warn", path=None)` writes records at `level` or above to a file, appended, or to stderr; warnings and errors also go to stderr. `fastmm.disable_logging()` or interpreter exit flushes and stops it.

Development install, tests, the type stub and wheels: [Python packages](contributing/python-packages.md).
