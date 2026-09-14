# Python strategy API

Strategies written in Python run inside the C++ engine in backtests. They use the same engine, risk
checks, OMS, quote manager, journal and outbound hash as C++ strategies (ADR-0012, section 7).
Running backtests, data sources and results are covered in [../python.md](../python.md).

Scope: in-process backtests only. Replay of Python strategies, process-pool sweeps, live trading and
the simulated exchange over the network are not supported.

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

`params=` is applied on top of `config.params` for every kind of strategy. The TOML example
configures `basic_mm`, so call `cfg.clear_params()` before running a strategy with other
parameters. The result's `strategy` is `py:<QualName>`, which is also written to the journal header
(truncated to 31 characters) when `config.journal_out` is set.

## Hooks

Hook names and argument order match the C++ hooks. Define any subset; a hook the class does not
define is never looked up or called. `Strategy.hooks()` lists the defined hooks and raises
`TypeError` for a wrong signature (`fastmm: X.on_trade has the wrong signature; expected
on_trade(self, ctx, inst, trade)`); likely misspellings such as `on_fills` warn (silence with
`fastmm_allow_near_miss_names = True`).

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

`Param(default, min=None, max=None, doc="")` declares a typed parameter: the type comes from the
default (`bool`, `int` or `float`; schema types `bool`, `int`, `double`). Read it as an attribute
(`self.quote_qty`). Values from `config.params` (strings) and `params=` (Python scalars) are parsed
like `FASTMM_PARAM` values in C++, and errors have the same text with the strategy name first:

```text
py:SkewMM: unknown parameter 'levles'
py:SkewMM: parameter 'levels': value 9 outside [1, 8]
py:SkewMM: parameter 'levels': cannot parse '2.5' as int (a whole number)
```

An unbounded side prints as `-inf` or `inf`. Override `validate(self) -> Optional[str]` for
cross-field checks; it runs after all keys are applied, and on any error the previous values stay.
`Strategy.schema()` returns `[{"name", "type", "default", "min", "max", "doc"}, ...]` like
`fastmm.strategies()`.

## Context

`ctx` mirrors the C++ `StrategyContext`. It is usable only inside a hook, on the thread running the
backtest; anywhere else it raises `RuntimeError`. Instrument arguments accept an `Instrument` or
its integer id; `side` is `fastmm.BUY` (0) or `fastmm.SELL` (1).

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

`set_quotes` takes sequences of `(price, qty)` pairs, level 0 first, at most 8 per side (`None` is
an empty side). It returns `False`, and the quotes are ignored, while quoting is disabled.
More than 8 levels, a non-finite price or quantity, or a negative quantity raise `ValueError`
before the engine is touched; a non-positive price or a zero quantity drops the level, as
`DesiredQuotes::bid/ask` do in C++.

`send` and `replace` raise `fastmm.OrderRejected` with `.reason`, the `RejectReason` name
(`"InvalidTag"` for a tag in the quote manager's range, `"UnknownOrder"`, risk reasons such as
`"MaxPosition"`). `cancel` returns `False` for an unknown or terminal order.

## Views and values

Views are reused: the adapter keeps one per instrument (`BookView`, `PositionView`) and one per
event type, and repoints it before each hook. A view is valid only inside the hook that received
it, or the hook that called `ctx.book()` / `ctx.position()`; reading it later raises
`fastmm.StaleViewError` (a `RuntimeError`). Copy the numbers you want to keep.

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

`Instrument` (`id`, `symbol`, `venue`, `base`, `quote`, `asset_class`, `tick`, `lot`, `min_qty`,
`min_notional`, `contract_multiplier`, `round_price(price, side)`, `round_qty(qty)`), `Order`
(`id`, `instrument`, `side`, `state`, `price`, `qty`, `filled`, `leaves`, `user_tag`, `post_only`,
`reduce_only`, `created_ns`) and `Portfolio` are copies and can be kept. An `Instrument` compares
and hashes like its id and works as a list index.

## Numbers

Every price, quantity and amount is available two ways: a float (`book.mid`, `fill.price`) and the
exact raw int64 at a 1e-8 scale with a `_raw` suffix (`book.mid_raw`, `fill.price_raw`), the units of
C++ `Price::raw` and of the result columns. There is no `Decimal`.

- `set_quotes`, `send` and `replace` take floats: prices are converted to 1e-8 (half away from
  zero) and rounded to the tick passively (bids down, asks up), quantities down to the lot.
- `set_quotes_raw`, `send_raw` and `replace_raw` take ints and use them as given.
- Floats are fine for research. For a strategy you intend to port to C++ with identical orders, use
  the raw values and reproduce the C++ operators: `Fixed * Ratio` truncates toward zero (Python's
  `//` floors; see `tdiv` and `mul_ratio` in `examples/python/strategies/basic_mm_exact.py`).

## Errors

A hook that raises stores the exception, pulls all quotes and asks the engine to stop; no hook of
that strategy is called again (including `on_stop`). The simulator ends the run after the failing
event and `run_backtest` raises `fastmm.StrategyError`:

```python
try:
    fastmm.run_backtest(cfg, data="synthetic", strategy=MyMM)
except fastmm.StrategyError as e:
    e.__cause__          # the original exception, with its traceback
    e.result             # the partial BacktestResult
    e.hook, e.now_ns, e.events
```

`KeyboardInterrupt` and `SystemExit` propagate unchanged, with the partial result as `.result`.
Ctrl-C works: every 1,024 hook calls and every 4,096 engine steps the adapter runs
`PyErr_CheckSignals()` and releases the GIL briefly.

## Determinism

A Python strategy is as deterministic as its own code. The engine clock, event order, RNG and
outbound hash are the ones C++ strategies use, so two runs with the same configuration, data and
seed give the same `outbound_sha256` (also across `PYTHONHASHSEED` values, which the tests check).

- Use `ctx.now_ns`, never `time.time()`.
- Use `ctx.random()` and `ctx.randint()`, never `random` or `numpy.random` without a fixed seed.
- No threads.
- No decisions that depend on the iteration order of a `set` of strings or on `id()`.
- Float results are reproducible on one platform and Python version.

## Performance

The GIL is held for the whole run of a Python strategy (C++ strategies release it). Measured with
`python bench/python/bench_py_strategy.py --seconds 3600 --repeat 5` (1,356,426 market-data events
of the synthetic L2 queue market; gcc 13 release module, Python 3.12, WSL2 on an 8-thread desktop):

| Case | Wall time | Events/s end to end |
|---|---|---|
| C++ `basic_mm` | 0.670 s | 2.02 M |
| Python `BasicMMExact` (same orders as C++) | 1.035 s | 1.31 M |
| Python `SkewMM` | 1.230 s | 1.10 M |
| Python, no hooks defined | 0.477 s | 2.84 M |
| Python, an empty hook on every event | 0.530 s | 2.56 M |
| Python, two field reads on every event | 0.821 s | 1.65 M |

The end-to-end numbers include the synthetic market and the simulated venue, which dominate here.
Per call: an empty hook adds about 40 ns, reading two or three fields about 250 ns, and
`BasicMMExact` about 3.1 µs per `on_book` or `on_fill` call over C++ `basic_mm`. Most of a
strategy's cost is its own Python arithmetic and attribute access, so define only the hooks you
need and return early.

## Limits

- Backtests only: no replay of Python strategies (`fastmm.replay`), no process-pool sweeps
  (`fastmm.sweep` takes registered names), no live trading; `fastmm-live` never links Python.
- The simulator produces no connection state changes and the numpy source no option tickers, so
  `on_connection` and `on_option_ticker` fire only with data sources that contain them.
- One thread: a hook must not start threads that call the context.
