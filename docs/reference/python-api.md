# Python strategy API

Strategies written in Python run inside the C++ engine in backtests. They use the same engine, risk checks, OMS, quote manager, journal and outbound hash as C++ strategies (ADR-0012, section 7). Backtests, data sources and results: [Python](../python.md).

Methods marked `@fastmm.hot` are compiled with Numba instead and follow [Hot hooks](#hot-hooks).

Scope: in-process backtests only. Replay of Python strategies, process-pool sweeps, live trading and the simulated exchange over the network are not supported.

## A strategy

```python
import fastmm
from fastmm import BUY, SELL, Param, Strategy

class SkewMM(Strategy):
    half_spread_bps = Param(0.01, min=0.0, max=10_000.0, doc="half spread around mid, bps")
    quote_qty = Param(0.002, min=0.0, doc="size per level, base units")

    def on_start(self, ctx):
        ctx.every(100_000_000)                         # on_timer every 100 ms

    def on_book(self, ctx, inst, book):
        if not book.valid:
            return ctx.pull_quotes(inst)
        half = book.mid * self.half_spread_bps * 1e-4
        ctx.set_quotes(inst, [(book.mid - half, self.quote_qty)],
                       [(book.mid + half, self.quote_qty)])

    def on_fill(self, ctx, fill):
        print(fill.side, fill.price, ctx.position(fill.instrument).qty)

cfg = fastmm.BacktestConfig.from_toml("configs/backtest-example.toml")
cfg.clear_params()
r = fastmm.run_backtest(cfg, data="synthetic", strategy=SkewMM, params={"quote_qty": 0.001})
r.strategy, r.outbound_sha256, r.stats()["net_pnl"]
```

`fastmm.run_backtest(config, data=None, strategy=None, params=None)`:

| `strategy=` | runs |
|---|---|
| `None` | `config.strategy` (a registered C++ strategy) |
| `"basic_mm"` | the registered C++ strategy |
| a `Strategy` subclass | a fresh instance of it |
| a `Strategy` instance | that instance, once (read its attributes afterwards); a second run raises `ValueError` |

`params=` is applied on top of `config.params` for every kind of strategy. The TOML example configures `basic_mm`, so call `cfg.clear_params()` before running a strategy with other parameters. The result's `strategy` is `py:<QualName>`, which is also written to the journal header (truncated to 31 characters) when `config.journal_out` is set.

## Hooks

Hook names and argument order match the C++ hooks. Define any subset; a hook the class does not define is never looked up or called. `Strategy.hooks()` lists the defined hooks and raises `TypeError` for a wrong signature (`fastmm: X.on_trade has the wrong signature; expected on_trade(self, ctx, inst, trade)`); likely misspellings such as `on_fills` warn (silence with `fastmm_allow_near_miss_names = True`).

| Hook | Called |
|---|---|
| `on_start(self, ctx)` | once before the first event |
| `on_stop(self, ctx)` | once after the last event |
| `on_book(self, ctx, inst, book)` | every book update of an instrument in the table |
| `on_book_ticker(self, ctx, inst, msg)` | every best bid and ask update |
| `on_trade(self, ctx, inst, trade)` | every public trade |
| `on_option_ticker(self, ctx, inst, msg)` | every option mark and greeks update |
| `on_fill(self, ctx, fill)` | every execution, after position and fees are updated and before the `on_order_update` of the same execution |
| `on_order_update(self, ctx, update)` | every change of one of our orders |
| `on_timer(self, ctx, timer_id, tag)` | a timer from `ctx.every` or `ctx.once` fired |
| `on_connection(self, ctx, msg)` | a venue connection state change (not produced by the simulator) |
| `on_quoting(self, ctx, enabled)` | `ctx.quoting_enabled` changed (kill switch, operator pull, reconciliation) |

`inst` is an `Instrument`; `book`, `msg`, `trade`, `fill` and `update` are views (below).

## Parameters

`Param(default, min=None, max=None, doc="")` declares a typed parameter: the type comes from the default (`bool`, `int` or `float`; schema types `bool`, `int`, `double`). Read it as an attribute (`self.quote_qty`). Values from `config.params` (strings) and `params=` (Python scalars) are parsed like `FASTMM_PARAM` values in C++, and errors have the same text with the strategy name first:

```text
py:SkewMM: unknown parameter 'levles'
py:SkewMM: parameter 'levels': value 9 outside [1, 8]
py:SkewMM: parameter 'levels': cannot parse '2.5' as int (a whole number)
```

An unbounded side prints as `-inf` or `inf`. Override `validate(self) -> Optional[str]` for cross-field checks; it runs after all keys are applied, and on any error the previous values stay. `Strategy.schema()` returns `[{"name", "type", "default", "min", "max", "doc"}, ...]` like `fastmm.strategies()`.

## Context

`ctx` mirrors the C++ `StrategyContext`. It is usable only inside a hook, on the thread running the backtest; anywhere else it raises `RuntimeError`. Instrument arguments accept an `Instrument` or its integer id; `side` is `fastmm.BUY` (0) or `fastmm.SELL` (1).

| Group | API |
|---|---|
| Time, reference data | `now_ns`, `instruments` (tuple, index == id), `instrument(inst)`, `contains(inst)` |
| Market data | `book(inst)` -> `BookView` |
| Portfolio | `position(inst)` -> `PositionView`, `portfolio()` -> `Portfolio` (`realized`, `unrealized`, `fees`, `net`) |
| Quoting | `set_quotes(inst, bids, asks) -> bool`, `set_quotes_raw(inst, bids, asks) -> bool`, `pull_quotes(inst)`, `pull_all_quotes()`, `working_quote(inst, side, level=0) -> Order or None` |
| Direct orders | `send(inst, side, price, qty, *, post_only=False, reduce_only=False, ioc=False, tag=0) -> int`, `send_raw(...)`, `cancel(order_id) -> bool`, `replace(order_id, price, qty)`, `replace_raw(...)`, `order(order_id) -> Order or None`, `open_qty(inst, side)`, `open_qty_raw(inst, side)` |
| Timers | `every(period_ns, tag=0) -> int`, `once(delay_ns, tag=0) -> int`, `cancel_timer(timer_id) -> bool` |
| Control | `quoting_enabled`, `killed`, `request_stop()` (the run ends after the current event) |
| Randomness | `random()` in [0, 1), `randint(lo, hi)` inclusive, from the engine's seeded RNG |

`set_quotes` takes sequences of `(price, qty)` pairs, level 0 first, at most 8 per side (`None` is an empty side). It returns `False`, and the quotes are ignored, while quoting is disabled. More than 8 levels, a non-finite price or quantity, or a negative quantity raise `ValueError` before the engine is touched; a non-positive price or a zero quantity drops the level, as `DesiredQuotes::bid/ask` do in C++.

`send` and `replace` raise `fastmm.OrderRejected` with `.reason`, the `RejectReason` name (`"InvalidTag"` for a tag in the quote manager's range, `"UnknownOrder"`, risk reasons such as `"MaxPosition"`). `cancel` returns `False` for an unknown or terminal order.

## Views and values

Views are reused between hooks. A view is valid only inside the hook that received it, or the hook that called `ctx.book()` / `ctx.position()`; reading it later raises `fastmm.StaleViewError` (a `RuntimeError`). Copy the numbers you want to keep.

| View | Fields |
|---|---|
| `BookView` | `instrument`, `valid`, `mid`, `spread`, `best_bid` and `best_ask` as `(price, qty)`, `level(side, i)`, `depth(side)`, `last_update_ns`, `seq`, `microprice()`, `imbalance(levels=1)` |
| `PositionView` | `instrument`, `qty`, `avg_price`, `realized`, `unrealized`, `fees`, `net_pnl`, `fills` |
| `TradeView` | `instrument`, `price`, `qty`, `aggressor` (side), `trade_id`, `exch_ts_ns`, `recv_ts_ns` |
| `BookTickerView` | `instrument`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`, `exch_ts_ns`, `recv_ts_ns` |
| `OptionTickerView` | `instrument`, `mark_price`, `underlying_price`, `index_price`, `mark_iv`, `bid_iv`, `ask_iv`, `delta`, `gamma`, `vega`, `theta`, `rho`, `interest_rate`, `exch_ts_ns`, `recv_ts_ns` |
| `ConnectionView` | `venue`, `state` (`"Live"`, `"Disconnected"`, ...), `live`, `channel`, `reason_code`, `recv_ts_ns` |
| `FillView` | `instrument`, `side`, `price`, `qty`, `position_delta`, `fee`, `fee_converted`, `liquidity` (`fastmm.MAKER`, `fastmm.TAKER`), `known`, `late`, `order_done`, `order_id`, `exch_ts_ns`, `update` (`OrderUpdateView` or `None`) |
| `OrderUpdateView` | `order_id`, `instrument`, `side`, `state`, `prev_state`, `reject_reason`, `user_tag`, `known`, `changed`, `terminal`, `price`, `qty`, `filled`, `leaves`, `fill_price`, `fill_qty` |

`Instrument` (`id`, `symbol`, `venue`, `base`, `quote`, `asset_class`, `tick`, `lot`, `min_qty`, `min_notional`, `contract_multiplier`, `round_price(price, side)`, `round_qty(qty)`), `Order` (`id`, `instrument`, `side`, `state`, `price`, `qty`, `filled`, `leaves`, `user_tag`, `post_only`, `reduce_only`, `created_ns`) and `Portfolio` are copies and can be kept. An `Instrument` compares and hashes like its id and works as a list index.

## Numbers

Every price, quantity and amount is available two ways: a float (`book.mid`, `fill.price`) and the exact raw int64 at a 1e-8 scale with a `_raw` suffix (`book.mid_raw`, `fill.price_raw`), the units of C++ `Price::raw` and of the result columns. There is no `Decimal`.

- `set_quotes`, `send` and `replace` take floats: prices are converted to 1e-8 (half away from zero) and rounded to the tick passively (bids down, asks up), quantities down to the lot.
- `set_quotes_raw`, `send_raw` and `replace_raw` take ints and use them as given.
- To send the same orders as a C++ port, use the raw values and the C++ operators: `Fixed * Ratio` truncates toward zero (Python's `//` floors; see `tdiv` and `mul_ratio` in `examples/python/strategies/basic_mm_exact.py`).

## Errors

A hook that raises stores the exception, pulls all quotes and asks the engine to stop; no hook of that strategy is called again (including `on_stop`). The simulator ends the run after the failing event and `run_backtest` raises `fastmm.StrategyError`:

```python
try:
    fastmm.run_backtest(cfg, data="synthetic", strategy=MyMM)
except fastmm.StrategyError as e:
    e.__cause__          # the original exception, with its traceback
    e.result             # the partial BacktestResult
    e.hook, e.now_ns, e.events
```

`KeyboardInterrupt` and `SystemExit` propagate unchanged, with the partial result as `.result`. The adapter checks for Ctrl-C (`PyErr_CheckSignals()`, GIL released briefly) every 1,024 hook calls and every 4,096 engine steps.

## Determinism

A Python strategy is as deterministic as its own code. The engine clock, event order, RNG and outbound hash are the ones C++ strategies use, so two runs with the same configuration, data and seed give the same `outbound_sha256` (also across `PYTHONHASHSEED` values, which the tests check).

- Use `ctx.now_ns`, never `time.time()`.
- Use `ctx.random()` and `ctx.randint()`, never `random` or `numpy.random` without a fixed seed.
- No threads.
- No decisions that depend on the iteration order of a `set` of strings or on `id()`.
- Float results are reproducible on one platform and Python version.

## Performance

The GIL is held for the whole run of a Python strategy (C++ strategies release it). Measured with `python bench/python/bench_py_strategy.py --seconds 3600 --repeat 5` (1,356,426 market-data events of the synthetic L2 queue market; gcc 13 release module, Python 3.12, WSL2 on an 8-thread desktop):

| Case | Wall time | Events/s end to end |
|---|---|---|
| C++ `basic_mm` | 0.670 s | 2.02 M |
| Python `BasicMMExact` (same orders as C++) | 1.035 s | 1.31 M |
| Python `SkewMM` | 1.230 s | 1.10 M |
| Python, no hooks defined | 0.477 s | 2.84 M |
| Python, an empty hook on every event | 0.530 s | 2.56 M |
| Python, two field reads on every event | 0.821 s | 1.65 M |

The end-to-end numbers include the synthetic market and the simulated venue, which dominate here. Per call: an empty hook adds about 40 ns, reading two or three fields about 250 ns, and `BasicMMExact` about 3.1 µs per `on_book` or `on_fill` call over C++ `basic_mm`.

## Limits

- `fastmm.sweep` takes registered C++ strategy names only; `fastmm-live` never links Python.
- The simulator produces no connection state changes and the numpy source no option tickers, so `on_connection` and `on_option_ticker` fire only with data sources that contain them.
- One thread: a hook must not start threads that call the context.

## Hot hooks

A class with `@fastmm.hot` methods is compiled with Numba in nopython mode, and the engine thread calls the compiled hooks through function pointers with the GIL released. It needs `pip install "fastmm-engine[hot]"` (numba 0.61 to 0.67, CPython 3.10 or later). Steps: [Write hot hooks in Python](../how-to/strategies/python-hot-hooks.md).

### Declarations

| Declaration | Meaning |
|---|---|
| `@fastmm.hot` on `on_book`, `on_fill`, `on_quoting` or `on_connection` | an event hook |
| `@fastmm.hot(every="100ms")` on any other name | a timer hook, at most 16; units `ns`, `us`, `ms`, `s`, `m`, `h` |
| `fastmm.Param(default, min=, max=, doc=)` | a parameter, typed, parsed and validated as in [Parameters](#parameters) |
| `fastmm.State(default, doc="")` | a per-instrument bool, int64 or float that hooks read and write; it starts at the default and keeps its value between calls |

Every hot hook takes `(self, ctx, book)` and runs once per instrument:

| Hook | Runs for |
|---|---|
| `on_book` | the instrument whose book changed |
| `on_fill` | the fill's instrument, after position and fees are updated |
| `on_quoting` | every instrument, when quoting is enabled or disabled |
| `on_connection` | every instrument of the venue whose connection changed |
| a timer hook | every instrument, once per period of engine time from the start of the run |

`self` holds the instrument's parameters and `State` fields as attributes. A float parameter also has `self.<name>_raw`, its value as a 1e-8 fixed-point int64 (nearest). The engine copies the parameters into `self` before every call, so an assignment to a parameter, also through an alias such as `t = self`, is gone at the next call.

Defining the class raises `TypeError` when it also defines a `fastmm.Strategy` hook as a plain method, a hot hook has another name or signature, a name is both a `Param` and a `State`, a name clashes with a float parameter's `_raw` field or with a ctx method, or a hook assigns `self.<parameter>` (a check of the source). Without numba it raises `ImportError` with `pip install "fastmm-engine[hot]"`.

### ctx

| Field | Meaning |
|---|---|
| `now_ns`, `instrument` | engine time (ns) and the instrument id |
| `tick`, `lot`, `min_qty`, `position` | floats (quote currency for the tick, base units otherwise), each with a `_raw` field |
| `quoting_enabled` | 1 while quoting is enabled and the instrument's venue is not killed; the engine ignores quotes otherwise |
| `connected` | in `on_connection`: 1 when the venue's connection is live |
| `fill_side`, `fill_maker`, `fill_price`, `fill_qty` | in `on_fill`: `fastmm.BUY` (0) or `fastmm.SELL` (1), 1 for a maker fill, price and quantity with `_raw` fields; 0 in other hooks |

| Method | Intent |
|---|---|
| `quote(bid_px, ask_px, qty)` | replace the ladder with one level per side |
| `bid(px, qty)`, `ask(px, qty)` | append a level; levels after the eighth per side are dropped |
| `quote_raw`, `bid_raw`, `ask_raw` | the same with int raw values (1e-8 scale) |
| `clear()` | an empty ladder: the working quotes are cancelled |
| `pull()` | pull the instrument's quotes |
| `uncross()` | move a level-0 ask at or below the level-0 bid to one tick above it (`DesiredQuotes::uncross`) |
| `keep_passive()` | shift each side so level 0 is one tick inside the book's touch (`keep_passive`), after `uncross` |
| `fail(code)` | stop the strategy with an int code |

When the hook returns, the engine makes at most one call for the instrument: `set_quotes` after `quote`, `bid`, `ask` or `clear`, or `pull_quotes` after `pull`, whichever came last. A float price or quantity is rounded to the nearest 1e-8, then prices to the tick (bids down, asks up) and quantities down to the lot; a raw level is used as given. A level with a non-positive price or a zero quantity is dropped.

### book

| Field | Meaning |
|---|---|
| `valid`, `ts_ns` | 1 when the book is valid (`BookView.valid`); last update in engine time, 0 before the first |
| `mid`, `best_bid`, `best_ask`, `best_bid_qty`, `best_ask_qty` | floats, each with a `_raw` field |
| `n_bids`, `n_asks` | levels present in the arrays, at most 10 per side |
| `bid_px[i]`, `bid_qty[i]`, `ask_px[i]`, `ask_qty[i]` | level `i`, 0 best, each with a `_raw` array; entries from `n_bids` or `n_asks` on are 0 |

### fastmm.fx

`fastmm.fx` has the C++ fixed-point operators for raw values; each works in plain Python and in hooks. Constants: `SCALE` (100,000,000), `RATIO_PER_BP` (10,000), `BUY`, `SELL`.

| Function | Result |
|---|---|
| `tdiv(a, b)` | integer division truncating toward zero, as in C++; `ZeroDivisionError` when `b` is 0 |
| `mul_ratio(v, r)` | `Fixed * Ratio`: the 128-bit product divided by 1e8, truncating toward zero |
| `bps_ratio(bps_raw)` | the `Ratio` raw value of a bps parameter's `_raw` field (0.01 bps: 1,000,000 gives 100) |
| `round_price(p, tick, side)`, `round_qty(q, lot)` | `round_to_tick` (bids down, asks up) and `round_to_lot` (down) |
| `to_raw(x)`, `to_float(raw)` | the nearest raw value (halves away from zero; `ValueError` when not finite) and back |

### What compiles

Hooks use Numba's nopython subset: numbers, the fields and methods above, loops, tuples and functions decorated with `numba.njit`. The code runs with bounds checks (an index outside an array raises `IndexError`) and Python's error model (a division by zero raises `ZeroDivisionError`); helpers are compiled with bounds checks when a hook first calls them.

Before a run, FastMM scans the LLVM IR of each hook and of the helpers it calls. A call to anything other than an LLVM intrinsic, a libm function or `NRT_MemInfo_call_dtor` raises `fastmm.HotCompileError` (a `TypeError`) with the hook and the symbol:

```text
fastmm: Allocates.on_book is rejected by the IR check: it calls NRT_MemInfo_alloc_aligned (memory allocation (arrays, lists, dicts, strings)). Hot hooks may not allocate, print or use Python objects.
```

This rejects arrays, lists, strings, `print` and Python objects. Calls through ctypes or cffi function pointers are not detected and are not supported. A hook that Numba cannot compile raises `HotCompileError` with the hook's name and Numba's message.

### Running and errors

`run_backtest(..., hot_cache=True)` compiles the hooks, calls each once on scratch copies of `ctx`, `book` and `self` (nothing is sent and `State` is unchanged), then runs the backtest with the GIL released.

A hook that raises, calls `ctx.fail(code)`, or sets a float price or quantity that is not finite, beyond 9.2e10 in magnitude, or a negative quantity, stops the strategy: no hook runs again, the kill switch trips with reason `StrategyError` (quotes pulled, working orders cancelled) and the backtest ends after that event. `run_backtest` raises `fastmm.StrategyError`:

| Attribute | Value |
|---|---|
| `hook` | the hook's name |
| `status` | 1 raised, 2 `ctx.fail`, 3 bad float level |
| `fail_code` | the code passed to `ctx.fail` |
| `kill_reason` | `"StrategyError"` |
| `now_ns`, `events`, `result` | engine time, engine events and the partial `BacktestResult` |

Numba keeps neither the type nor the message of the exception.

### Numba cache

With `hot_cache=True` compiled hooks are stored under `$FASTMM_CACHE_DIR/numba/` (default `$XDG_CACHE_HOME/fastmm` or `~/.cache/fastmm`) in a directory whose name holds the fastmm version, the hot ABI, a hash of FastMM's compiler sources, the numba and llvmlite versions and the CPU. Numba checks only the source file of each hook: after changing a helper in another file, pass `hot_cache=False` or delete the directory.

### Performance

Measured with `python bench/python/bench_hot_strategy.py --build build/release` (gcc 13 release module, numba 0.67.0, Python 3.12, WSL2 on a Zen 4 desktop, one core). Cost of one `on_book` call without the engine, on the same books, positions and parameters (`levels = 2`):

| Case | ns per call |
|---|---|
| C++ `BasicMM::on_book` | 31.7 |
| `BasicMMHot` (exact) | 36.8 |
| `BasicMMHot` with bounds checks off | 37.0 |
| `BasicMMHot` with the 10 levels per side copied | 72.0 |
| `BasicMMHotFloat` | 30.4 |

Bounds checks cost less than the run-to-run spread (under 1 ns per call). The engine copies the level arrays only when a hook, or a `numba.njit` function it calls, names one of them, or calls code the check cannot follow; the copy costs about 35 ns per call.

On the synthetic market (3,600 s, 1,356,426 market-data events) C++ `basic_mm` runs at 1.93 M events/s and `BasicMMHot` at 1.90 M events/s with the same orders. Compiling `BasicMMHot` (5 hooks) takes 1.1 s in a fresh process, 0.5 s from the Numba cache; importing numba takes 0.14 s.

## Live sessions

`fastmm.run_live(strategy, config, params=None, *, duration=None, dry_run=False, journal=None, no_journal=False, status=None, no_status=False, log=None, record_raw=None, allow_inline_secrets=False)` runs a class with hot hooks against the venues in `config`, in this process, and returns the exit code ([Exit codes](cli.md#exit-codes)). It needs `pip install "fastmm-engine[live]"` (CPython 3.10 or later) and raises `ImportError` without it. Steps: [Run a Python strategy live](../how-to/strategies/python-live.md).

| Argument | Meaning |
|---|---|
| `strategy` | a `fastmm.Strategy` subclass with `@fastmm.hot` methods, or an unused instance |
| `config` | a `fastmm-live` configuration ([Configuration](configuration.md)) |
| `params` | overrides on top of `[strategy.params]`, which apply only when `[strategy] name` is `py:<Class>`, `<module>:<Class>` or `<Class>` |
| `duration` | seconds, or a string such as `"60s"`; `None` runs until SIGINT or SIGTERM |
| the other keywords | the `fastmm-live` options of the same names ([Command lines](cli.md#fastmm-live)) |

Before any venue is contacted, `run_live` applies the parameters, compiles the hooks without the Numba cache and calls each once on scratch data. It then runs the `fastmm-live` session with the GIL released: the same threads, log, status file, journal, kill switch, `on_kill` and exit codes.

| Case | Result |
|---|---|
| a configuration, parameter or compilation error, or a class without hot hooks | a message on stderr; returns 3 |
| a hook raises, calls `ctx.fail` or sets a bad float level | the kill switch trips with `StrategyError` and stderr names the hook; returns 6 with `on_kill = "exit"` |
| `run_live` while a session runs in the process | `RuntimeError` |
| `run_live` in a child forked while a session runs | `RuntimeError`; start children with the `spawn` or `forkserver` method |

- SIGINT and SIGTERM stop the session. The session replaces the process's handlers while it runs and restores them when it returns, so `KeyboardInterrupt` is not raised.
- Before the engine starts, every thread of the process, including numpy's BLAS threads, is limited to the CPUs not listed in `[engine] cpu` and `net_cpus`; threads started later inherit that. Without pinned CPUs nothing changes. Thread-count variables such as `OPENBLAS_NUM_THREADS` act only when set before numpy loads.
- The journal's parameter table lists the class's parameters. `inspect_journal(path)["strategy_meta"]` returns `class` (`module:qualname`), `hot_source_sha256` (the source of the hot hooks and the `numba.njit` functions they call) and the `fastmm`, `numba`, `llvmlite` and `python` versions ([Journal format](journal-format.md)).

### python -m fastmm run

`python -m fastmm run <module>:<Class> --config <file> [options]` imports the class, with the current directory first on the module path, and exits with the code `run_live` returns. The options are the `fastmm-live` ones: `--param key=value` (repeatable), `--duration`, `--dry-run`, `--record-raw`, `--journal`, `--no-journal`, `--status`, `--no-status`, `--log` and `--allow-inline-secrets`. A bad command line exits with 2 and a class that cannot be imported with 3. Each of `OPENBLAS_NUM_THREADS`, `OMP_NUM_THREADS` and `MKL_NUM_THREADS` that is not set is set to 1, and the interpreter starts again once so that numpy sees them.
