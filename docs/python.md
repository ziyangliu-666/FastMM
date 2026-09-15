# Python research bindings

`fastmm` wraps the in-process backtester (`fastmm::backtest`) with pybind11. The wheel contains only the backtest/sim code: no networking, no OpenSSL (`FASTMM_BUILD_NET=OFF`).

```bash
python -m venv .venv
.venv/bin/pip install -e ".[dev]"              # editable build (scikit-build-core)
.venv/bin/python -m pytest python/tests -q
.venv/bin/python examples/python/backtest_quickstart.py
.venv/bin/python examples/python/sweep_spread.py
```

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
frames = r.to_pandas()                  # {"fills", "equity", "orders"} DataFrames
```

`BacktestConfig.single_instrument("BTCUSDT", tick="0.01", lot="0.00001")` builds a config by hand. Other fields: `strategy`, `params`, `engine_seed`, `start_ns`, `equity_bar_s`, `initial_capital`, `queue_conservatism`, `latency_fixed_us`, `latency_jitter_us`, `latency_md_us`, `latency_md_jitter_us`, `p_drop`, `maker_fee_bps`, `taker_fee_bps`, `supports_replace`, `start_mid`, `limit_rate_per_s`, `market_rate_per_s`, `mid_step_rate_per_s`, `cancel_rate_per_order_s`, `source`, `path`, `output_dir`, `journal_out`, `measure_wall_clock`.

### Data

| `data=` | source |
|---|---|
| `None` | the config's `[backtest] source / path` |
| `"synthetic"` | the seeded synthetic market |
| `"x.fmj"` / `"x.csv"` (str or `os.PathLike`) | journal / CSV (`ts_ns,type,inst,side,price,qty,seq`) |
| `dict` of numpy arrays | `ArraySource`, zero copy |

Array columns: `ts` int64 (ns), `type` uint8 (0 snapshot level, 1 delta level, 2 trade, 3 book ticker), `inst` uint32, `side` int8 (0 bid/buy, 1 ask/sell), `price` and `qty` int64 (raw 1e-8) or float64, optional `seq` uint64. Dtypes must match exactly and arrays must be 1-D, C-contiguous, aligned and native-endian: a float32 column raises `TypeError` and a strided slice raises `ValueError` instead of being copied. `fastmm.load_csv(path)` reads a CSV into int64 columns (identical outbound hash to running the file by path).

The GIL is released while a C++ strategy's backtest runs, so several runs can proceed in Python threads. A strategy written in Python holds the GIL for its whole run.

## Python strategies

Subclass `fastmm.Strategy`, define the hooks you need and pass the class as `strategy=`:

```python
class Joiner(fastmm.Strategy):
    qty = fastmm.Param(0.002, min=0.0, doc="size per side")

    def on_book(self, ctx, inst, book):
        if book.valid:
            ctx.set_quotes(inst, [(book.best_bid[0], self.qty)], [(book.best_ask[0], self.qty)])

cfg.clear_params()                     # the example file configures basic_mm
r = fastmm.run_backtest(cfg, data="synthetic", strategy=Joiner, params={"qty": 0.001})
```

Reference: [Python strategy API](reference/python-api.md). `examples/python/strategies/basic_mm_exact.py` is an integer port of the C++ BasicMM with the same outbound hash; `skew_mm.py` is a float market maker.

Methods marked `@fastmm.hot` are compiled with Numba and run without the GIL: [Write hot hooks in Python](how-to/strategies/python-hot-hooks.md). `basic_mm_hot.py` ports BasicMM that way, with the same outbound hash.

### Results

`r.fills`, `r.equity` and `r.orders` are dicts of read-only numpy views over the C++ result vectors (no copy; the arrays keep the result alive). Prices, quantities, fees and PnL are raw int64 with a 1e-8 scale (`fastmm.FIXED_SCALE`); timestamps are int64 ns. `to_pandas()` converts to floats and `datetime64[ns]`. `stats()` returns the summary metrics; `engine_stats()` and `transport_stats()` the component counters; `write_all(dir)` writes the same CSV/JSON files as `fastmm-backtest`.

## Sweeps

```python
points = fastmm.sweep(cfg, {"half_spread_bps": [0.01, 0.02], "skew_bps_per_unit": [0, 0.01]},
                      data="synthetic", threads=0)
df = fastmm.sweep_frame(points)           # one row per grid point
```

Points run on a C++ thread pool (GIL released), every worker with its own cursor over `data`; results come back in grid order with the first parameter varying slowest.

## Other helpers

- `fastmm.strategies()`: `{name: [{"name", "type", "default", "min", "max", "doc"}]}`.
- `fastmm.OrderBook()`: L2 book (256 levels/side) with `apply_snapshot(bids, asks)`, `apply_delta(bids, asks)` (`(n, 2)` arrays or `(price, qty)` pairs, qty 0 deletes), `best_bid()`, `best_ask()`, `mid()`, `spread()`, `microprice()`, `weighted_mid(levels)`, `imbalance(levels)`, `bids(n)`, `asks(n)`.
- `fastmm.inspect_journal(path)`: header and message counts of an `.fmj`.

Regenerate the type stub after changing the bindings:

```bash
.venv/bin/pybind11-stubgen fastmm._core -o /tmp/stubs && cp /tmp/stubs/fastmm/_core.pyi python/fastmm/
```

## Logging

The C++ engine logs through an asynchronous logger that has no output until it is started. From Python, call `fastmm.enable_logging(level="warn", path=None)` to write records at `level` or above to a file (appended) or to stderr; warnings and errors are always mirrored to stderr as well. `fastmm.disable_logging()` flushes and stops it; interpreter exit does the same.

## Building and publishing wheels

`.github/workflows/wheels.yml` builds manylinux_2_28 x86_64 wheels of `fastmm` for CPython 3.9-3.14 and of `fastmm-live` for CPython 3.10-3.14 (each one tested with its test suite) and the `fastmm` sdist, for a `v*` tag or when run by hand, and keeps them as workflow artifacts; publishing is manual. `fastmm-live` has no sdist.

`fastmm-live` links OpenSSL statically, built by `scripts/wheels/build-openssl.sh` from a pinned, checksum-verified release; every OpenSSL security release needs a new `fastmm-live` release. To build it locally:

```bash
./scripts/wheels/build-openssl.sh "$HOME/.cache/fastmm-openssl"
OPENSSL_ROOT_DIR="$HOME/.cache/fastmm-openssl" .venv/bin/pip wheel ./python/live --no-deps -w dist
./scripts/wheels/check-live-wheel.sh dist/fastmm_live-*.whl
```

`FASTMM_OPENSSL_STATIC=OFF` links the system's shared OpenSSL instead; the check script then fails. Publishing to PyPI makes the package and its source public. To publish:

1. On PyPI, add a trusted publisher for this repository, workflow `wheels.yml`, environment `pypi`.
2. In the repository settings, create the `pypi` environment (optionally with required reviewers).
3. Run the `wheels` workflow manually with **publish** checked.

Locally, `python -m build --sdist && python -m twine check dist/*` checks the sdist metadata.

