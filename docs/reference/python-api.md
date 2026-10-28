# Python strategy API

A Python strategy is a `fastmm.Strategy` subclass that runs inside the C++ engine, with the same risk checks, OMS, quote manager, journal and outbound hash as a C++ strategy. Backtests, data sources and results: [Python](../python.md).

| Class has | Backtest, sweep, walk-forward | Replay | Live |
|---|---|---|---|
| plain hooks only | yes | no | no |
| `@fastmm.hot` hooks, compiled by Numba ([Hot hooks](#hot-hooks)) | yes | [yes](#replay) | [yes](#live-sessions) |

[What runs where](../python.md#what-runs-where) compares the two.

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

| `strategy=` | Runs |
|---|---|
| `None` | `config.strategy`, a registered C++ strategy |
| `"basic_mm"` | the registered C++ strategy |
| a `Strategy` subclass | a fresh instance of it |
| a `Strategy` instance | that instance, once, so its attributes can be read afterwards; a second run raises `ValueError` |

- `params=` applies on top of `config.params` for every kind of strategy.
- The TOML example configures `basic_mm`, so call `cfg.clear_params()` before running a strategy with other parameters.
- The result's `strategy` is `py:<QualName>`. With `config.journal_out` it is also the journal header's strategy, truncated to 31 characters.

## Hooks

Hook names and argument order match the [C++ hooks](strategy-api.md#hooks). Define any subset; an undefined hook is never looked up or called.

| Hook | Called |
|---|---|
| `on_start(self, ctx)` | once before the first event |
| `on_stop(self, ctx)` | once after the last event |
| `on_book(self, ctx, inst, book)` | every book update of an instrument in the table |
| `on_book_ticker(self, ctx, inst, msg)` | every best bid and ask update |
| `on_trade(self, ctx, inst, trade)` | every public trade |
| `on_option_ticker(self, ctx, inst, msg)` | every option mark and greeks update |
| `on_fill(self, ctx, fill)` | every execution, after position and fees are updated, before that execution's `on_order_update` |
| `on_order_update(self, ctx, update)` | every change of one of our orders |
| `on_timer(self, ctx, timer_id, tag)` | a timer from `ctx.every` or `ctx.once` fired |
| `on_connection(self, ctx, msg)` | a venue connection state change; the simulator produces none |
| `on_quoting(self, ctx, enabled)` | `ctx.quoting_enabled` changed: kill switch, operator pull, reconciliation |
| `on_balance(self, ctx, msg)` | a venue reported one asset of the account; `ctx.balance` already holds it |
| `on_perp_state(self, ctx, inst, msg)` | a derivative's mark, index or funding arrived; `ctx.mark` and `ctx.funding` already hold it |
| `on_risk_reject(self, ctx, reject)` | the engine's risk check refused a `send`, `replace` or quote, reported after the calling hook returns ([on_risk_reject](strategy-api.md#on_risk_reject)) |

`inst` is an `Instrument`; `book`, `msg`, `trade`, `fill` and `update` are [views](#views-and-values).

`reject` is a copied `RiskReject`: `inst`, `side`, `reason`, `replace`, `order_id`, `tag`, `post_only`, `price`, `qty`, `time_ns`.

`Strategy.hooks()` lists the defined hooks. A wrong signature raises `TypeError`:

```text
fastmm: X.on_trade has the wrong signature; expected on_trade(self, ctx, inst, trade)
```

A likely misspelling such as `on_fills` warns; `fastmm_allow_near_miss_names = True` silences it.

A class may also carry state between sessions ([State across sessions](strategy-api.md#state-across-sessions)):

| Method | With `BacktestConfig.state_file` or `[strategy] state_file` |
|---|---|
| `restore(self, data: bytes) -> bool` | gets the file's bytes once, after `on_start` |
| `state(self) -> bytes \| str` | written to the file every `state_interval_s` and at the end |

## Parameters

`Param(default, min=None, max=None, doc="")` declares a typed parameter, read as an attribute (`self.quote_qty`).

| Default's type | Schema type |
|---|---|
| `bool` | `bool` |
| `int` | `int` |
| `float` | `double` |

Values from `config.params` (strings) and `params=` (Python scalars) are parsed like `FASTMM_PARAM` values in C++. Errors have the same text with the strategy name first:

```text
py:SkewMM: unknown parameter 'levles'
py:SkewMM: parameter 'levels': value 9 outside [1, 8]
py:SkewMM: parameter 'levels': cannot parse '2.5' as int (a whole number)
```

- An unbounded side prints as `-inf` or `inf`.
- `validate(self) -> Optional[str]` checks across fields after all keys are applied; on any error the previous values stay.
- `Strategy.schema()` returns `[{"name", "type", "default", "min", "max", "doc"}, ...]`, like `fastmm.strategies()`.

## Context

`ctx` mirrors the C++ [`StrategyContext`](strategy-api.md#context). It works only inside a hook, on the thread running the backtest; elsewhere it raises `RuntimeError`. Instrument arguments take an `Instrument` or its integer id; `side` is `fastmm.BUY` (0) or `fastmm.SELL` (1).

| Group | API |
|---|---|
| Time, reference data | `now_ns`, `instruments` (tuple, index is id), `instrument(inst)`, `contains(inst)` |
| Market data | `book(inst)` -> `BookView` |
| Execution view | `own_qty(inst, side, price, at_ns=None)`, `best_ex_self(inst, side)` -> `(price, qty)`, `queue_ahead(order_id)` -> float or `None` ([Execution view](strategy-api.md#execution-view)) |
| Portfolio | `position(inst)` -> `PositionView`, `portfolio()` -> `Portfolio` (`realized`, `unrealized`, `fees`, `net`) |
| Quoting | `set_quotes(inst, bids, asks) -> bool`, `pull_quotes(inst)`, `pull_all_quotes()`, `working_quote(inst, side, level=0) -> Order or None` |
| Direct orders | `send(inst, side, price, qty, *, post_only=False, reduce_only=False, ioc=False, tag=0) -> int`, `cancel(order_id) -> bool`, `replace(order_id, price, qty)`, `order(order_id) -> Order or None`, `open_qty(inst, side)` |
| Timers | `every(period_ns, tag=0) -> int`, `once(delay_ns, tag=0) -> int`, `cancel_timer(timer_id) -> bool` |
| Control | `quoting_enabled`, `killed`, `request_stop()`: the run ends after the current event |
| Venue state | `fees(inst)` -> `Fees`, `risk_headroom(inst)` -> `RiskHeadroom`, `venue_health(venue=0)` -> `VenueHealth`, `order_budget(venue=0)` -> `OrderBudget` |
| Balances | `balance(venue, asset)` -> `Balance`, `margin(venue=0)` -> `Margin`, `balance_room(inst, side, price)` -> float or `None`, `balances_live()` ([Balances](strategy-api.md#balances)) |
| Perpetuals | `mark(inst)`, `index(inst)` -> `RefPrice`, `funding(inst)` -> `Funding` ([Perpetuals](strategy-api.md#perpetuals)) |
| Randomness | `random()` in [0, 1), `randint(lo, hi)` inclusive, from the engine's seeded RNG |

Methods taking or returning prices and quantities have a `_raw` twin on ints ([Numbers](#numbers)): `own_qty_raw`, `best_ex_self_raw`, `queue_ahead_raw`, `set_quotes_raw`, `send_raw`, `replace_raw`, `open_qty_raw`, `balance_room_raw`.

### set_quotes

`bids` and `asks` are sequences of `(price, qty)` pairs, level 0 first, at most 8 per side; `None` is an empty side.

| Input | Result |
|---|---|
| quoting disabled | returns `False`, quotes ignored |
| more than 8 levels, a non-finite price or quantity, a negative quantity | `ValueError` before the engine is touched |
| a non-positive price or zero quantity | the level is dropped, as `DesiredQuotes::bid/ask` do in C++ |

### Orders

- `send` and `replace` raise `fastmm.OrderRejected`; `.reason` is the `RejectReason` name.
- Reasons include `"InvalidTag"` for a tag in the quote manager's range, `"UnknownOrder"`, and risk reasons such as `"MaxPosition"`.
- `cancel` returns `False` for an unknown or terminal order.

### Venue state values

The fields and their meaning are in [Fees, risk headroom and venue health](strategy-api.md#fees-risk-headroom-and-venue-health). Hot hooks do not see these.

| Value | Fields |
|---|---|
| `Fees` | `maker_bps`, `taker_bps`, `maker_cbps`, `taker_cbps` |
| `RiskHeadroom` | `order_tokens`, `open_orders` (int); `max_order_qty`, `max_order_notional`, `buy_qty`, `sell_qty`, `underlying_buy_qty`, `underlying_sell_qty`, `gross_notional`, `net_buy_notional`, `net_sell_notional`, `exposure_buy_notional`, `exposure_sell_notional`, `loss_budget`, `balance_buy_qty`, `balance_sell_qty` (float, each with a `_raw` int); `None` for a limit that is off |
| `VenueHealth` | `feed_lag_ns`, `feed_lag_base_ns`, `feed_lag_excess_ns`, `ack_rtt_ns`, `ack_rtt_smoothed_ns`, `md_updated_ns`, `ack_updated_ns`, `md_samples`, `ack_samples`, `gate_engagements`, `gated` |
| `OrderBudget` | `local_tokens`, `orders_10s`, `orders_1m`, `orders_1d`, `weight`, `venue_paused`, `venue_known`, `orders_remaining` |

- `OrderBudget.local_tokens` is the `[risk] orders_per_sec` bucket, `None` with the limit off.
- Each window is `(used, limit)`, or `None` where the venue declares no such window. `orders_remaining` counts up to where the connector stops, 90 % of each limit.
- `venue_known` is `False` in a backtest.

### Balance and perpetual values

| Value | Fields |
|---|---|
| `Balance` | `free`, `locked`, `total`, `equity`, `maintenance` (float, each with `_raw`), `as_of_ns`, `known` |
| `Margin` | `available`, `initial`, `maintenance`, `equity`, `wallet` (float and `_raw`), `asset`, `as_of_ns`, `account`, `known` |
| `RefPrice` | `price`, `price_raw`, `at_ns`, `stale`, `usable` |
| `Funding` | `rate`, `interval_ns`, `next_ns`, `at_ns`, `stale`, `usable`, `over(seconds)` |

- `Balance.total` is the venue's last report moved by this engine's orders and fills since; `known` is `False` until the venue reports the asset.
- `Margin.account` marks the account-wide margin.
- `balance_room` is `None` until the venue reports the balance the side draws on.
- `Funding.rate` is per interval, positive when longs pay; `next_ns` 0 means continuous; `over(seconds)` is the rate over a holding time.

## Views and values

A view is valid only inside the hook that received it, or that called `ctx.book()` or `ctx.position()`. Views are reused, so reading one later raises `fastmm.StaleViewError`, a `RuntimeError`. Copy the numbers you want to keep.

| View | Fields |
|---|---|
| `BookView` | `instrument`, `valid`, `mid`, `spread`, `best_bid` and `best_ask` as `(price, qty)`, `level(side, i)`, `depth(side)`, `last_update_ns`, `seq`, `microprice()`, `imbalance(levels=1)` |
| `PositionView` | `instrument`, `qty`, `avg_price`, `realized`, `unrealized`, `fees`, `net_pnl`, `fills` |
| `TradeView` | `instrument`, `price`, `qty`, `aggressor`, `trade_id`, `exch_ts_ns`, `recv_ts_ns`, `buyer`, `seller` |
| `BookTickerView` | `instrument`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`, `exch_ts_ns`, `recv_ts_ns` |
| `OptionTickerView` | `instrument`, `mark_price`, `underlying_price`, `index_price`, `mark_iv`, `bid_iv`, `ask_iv`, `delta`, `gamma`, `vega`, `theta`, `rho`, `interest_rate`, `exch_ts_ns`, `recv_ts_ns` |
| `BalanceView` | `venue`, `asset`, `free`, `locked`, `total`, `equity`, `maintenance`, `snapshot`, `snapshot_end`, `account`, `exch_ts_ns`, `recv_ts_ns` |
| `PerpStateView` | `instrument`, `venue`, `has_mark`, `has_index`, `has_funding`, `has_open_interest`, `mark_price`, `index_price`, `funding_rate`, `funding_interval_ns`, `next_funding_ns`, `open_interest`, `exch_ts_ns`, `recv_ts_ns` |
| `ConnectionView` | `venue`, `state` (`"Live"`, `"Disconnected"`, ...), `live`, `channel`, `reason_code`, `recv_ts_ns` |
| `FillView` | `instrument`, `side`, `price`, `qty`, `position_delta`, `fee`, `fee_converted`, `liquidity` (`fastmm.MAKER`, `fastmm.TAKER`), `known`, `late`, `order_done`, `order_id`, `exch_ts_ns`, `update` (`OrderUpdateView` or `None`) |
| `OrderUpdateView` | `order_id`, `instrument`, `side`, `state`, `prev_state`, `reject_reason`, `user_tag`, `known`, `changed`, `terminal`, `price`, `qty`, `filled`, `leaves`, `fill_price`, `fill_qty`, `sent_ns`, `venue_ack_ns`, `local_ack_ns` |

`TradeView.buyer` and `seller` are an `int` account id on Lighter, a `"0x..."` address on Hyperliquid, and `None` elsewhere.

These values are copies and can be kept:

| Value | Fields |
|---|---|
| `Instrument` | `id`, `symbol`, `venue`, `base`, `quote`, `asset_class`, `tick`, `lot`, `min_qty`, `min_notional`, `contract_multiplier`, `round_price(price, side)`, `round_qty(qty)` |
| `Order` | `id`, `instrument`, `side`, `state`, `price`, `qty`, `filled`, `leaves`, `user_tag`, `post_only`, `reduce_only`, `created_ns`, `sent_ns`, `venue_ack_ns`, `local_ack_ns` |
| `Portfolio` | `realized`, `unrealized`, `fees`, `net` |

- An `Instrument` compares and hashes like its id and works as a list index.
- `Order`'s `sent_ns`, `venue_ack_ns` and `local_ack_ns` are its [order times](strategy-api.md#execution-view), 0 when not known.

## Numbers

Every price, quantity and amount comes two ways: a float (`book.mid`, `fill.price`), and the exact int64 at 1e-8 scale with a `_raw` suffix (`book.mid_raw`). The raw value is C++ `Price::raw` and the unit of the result columns. There is no `Decimal`.

| Call | Takes | Conversion |
|---|---|---|
| `set_quotes`, `send`, `replace` | floats | prices to 1e-8 half away from zero, then to the tick passively (bids down, asks up); quantities down to the lot |
| `set_quotes_raw`, `send_raw`, `replace_raw` | ints | used as given |

To send the same orders as a C++ port, use raw values and the C++ operators. `Fixed * Ratio` truncates toward zero, while Python's `//` floors: see `tdiv` and `mul_ratio` in [`fastmm.fx`](#fastmmfx) and `examples/python/strategies/basic_mm_exact.py`.

## Errors

A hook that raises stops the strategy:

1. The exception is stored, all quotes are pulled and the engine is asked to stop.
2. No hook of that strategy runs again, `on_stop` included.
3. The simulator ends the run after the failing event, and `run_backtest` raises `fastmm.StrategyError`.

```python
try:
    fastmm.run_backtest(cfg, data="synthetic", strategy=MyMM)
except fastmm.StrategyError as e:
    e.__cause__          # the original exception, with its traceback
    e.result             # the partial BacktestResult
    e.hook, e.now_ns, e.events
```

`KeyboardInterrupt` and `SystemExit` propagate unchanged, with the partial result as `.result`. Ctrl-C is checked every 1,024 hook calls and every 4,096 engine steps.

## Determinism

The engine clock, event order, RNG and outbound hash are those of C++ strategies. Two runs with the same configuration, data and seed give the same `outbound_sha256`, across `PYTHONHASHSEED` values too, provided the strategy's own code is deterministic:

- Use `ctx.now_ns`, never `time.time()`.
- Use `ctx.random()` and `ctx.randint()`, never `random` or `numpy.random` without a fixed seed.
- No threads.
- No decisions that depend on the iteration order of a `set` of strings or on `id()`.
- Float results are reproducible on one platform and Python version.

## Performance

The GIL is held for the whole run of a plain Python strategy; C++ strategies release it. A one-hour synthetic L2 market, 1,356,426 market-data events, on an AMD Ryzen 7 7800X3D under WSL2 ([`bench/python/bench_py_strategy.py`](../../bench/python/bench_py_strategy.py)):

| Case | Wall time | Events/s end to end |
|---|---|---|
| C++ `basic_mm` | 0.670 s | 2.02 M |
| Python `BasicMMExact` (same orders as C++) | 1.035 s | 1.31 M |
| Python `SkewMM` | 1.230 s | 1.10 M |
| Python, no hooks defined | 0.477 s | 2.84 M |
| Python, an empty hook on every event | 0.530 s | 2.56 M |
| Python, two field reads on every event | 0.821 s | 1.65 M |

The synthetic market and the simulated venue dominate these times. Per call, an empty hook adds about 40 ns, reading two or three fields about 250 ns, and `BasicMMExact` about 3.1 µs over C++ `basic_mm`.

## Limits

| Limit | |
|---|---|
| Live | `fastmm-live` never links Python; a Python strategy trades live through `run_live` or `python -m fastmm run` |
| `on_connection` | the simulator produces no connection state changes |
| `on_option_ticker` | the numpy source carries no option tickers |
| `on_balance`, `ctx.balance` | no backtest source carries balances: the hook does not fire and balances stay unknown |
| `on_perp_state` | only a journal recorded by `fastmm-live` carries marks and funding; the CSV, Tardis, Binance and numpy sources do not |
| Threads | a hook must not start threads that call the context |

## Hot hooks

A class with `@fastmm.hot` methods is compiled by Numba in nopython mode. The engine thread calls the compiled hooks through function pointers with the GIL released. It needs numba 0.61 to 0.67 and the `hot` extra ([Install](../getting-started/install.md#python)). Steps: [Write hot hooks in Python](../how-to/strategies/python-hot-hooks.md).

### Declarations

| Declaration | Meaning |
|---|---|
| `@fastmm.hot` on `on_book`, `on_fill`, `on_quoting`, `on_connection` or `on_params` | an event hook |
| `@fastmm.hot(every="100ms")` on any other name | a timer hook, at most 16; units `ns`, `us`, `ms`, `s`, `m`, `h` |
| `fastmm.Param(default, min=, max=, doc=)` | a parameter, typed, parsed and validated as in [Parameters](#parameters) |
| `fastmm.State(default, doc="")` | a per-instrument bool, int64 or float the hooks read and write; it starts at the default and keeps its value between calls |

Every hot hook takes `(self, ctx, book)` and runs once per instrument:

| Hook | Runs for |
|---|---|
| `on_book` | the instrument whose book changed |
| `on_fill` | the fill's instrument, after position and fees are updated |
| `on_quoting` | every instrument, when quoting is enabled or disabled |
| `on_connection` | every instrument of the venue whose connection changed |
| `on_params` | every instrument a parameter update applied to, with the new values in `self` |
| a timer hook | every instrument, once per period of engine time from the start of the run |

- `self` holds the instrument's parameters and `State` fields as attributes.
- A float parameter also has `self.<name>_raw`, its nearest 1e-8 fixed-point int64.
- The engine copies the parameters into `self` before every call. An assignment to a parameter, also through an alias such as `t = self`, is lost at the next call.

Defining the class raises `TypeError` when:

- it defines a `fastmm.Strategy` hook other than `on_start` and `on_stop` as a plain method;
- a hot hook has another name or signature;
- a name is both a `Param` and a `State`, or clashes with a float parameter's `_raw` field or a ctx method;
- a hook assigns `self.<parameter>`, found by a check of the source.

Without numba it raises `ImportError` naming the `hot` extra and [Install from source](../getting-started/install.md#install-from-source).

### ctx

The hot `ctx` has no execution view: no `own_qty`, `queue_ahead` or order times. A strategy that needs them runs as a plain `Strategy`.

| Field | Meaning |
|---|---|
| `now_ns`, `instrument` | engine time in ns and the instrument id |
| `tick`, `lot`, `min_qty`, `position` | floats, the tick in the quote currency and the rest in base units, each with a `_raw` field |
| `quoting_enabled` | 1 while quoting is enabled and the instrument's venue is not killed; the engine ignores quotes otherwise |
| `connected` | in `on_connection`: 1 when the venue's connection is live |
| `fill_side`, `fill_maker`, `fill_price`, `fill_qty` | in `on_fill`: `fastmm.BUY` (0) or `fastmm.SELL` (1), 1 for a maker fill, price and quantity with `_raw` fields; 0 in other hooks |

| Method | Intent |
|---|---|
| `quote(bid_px, ask_px, qty)` | replace the ladder with one level per side |
| `bid(px, qty)`, `ask(px, qty)` | append a level; levels after the eighth per side are dropped |
| `quote_raw`, `bid_raw`, `ask_raw` | the same with raw ints at 1e-8 scale |
| `clear()` | an empty ladder: the working quotes are cancelled |
| `pull()` | pull the instrument's quotes |
| `uncross()` | move a level-0 ask at or below the level-0 bid to one tick above it (`DesiredQuotes::uncross`) |
| `keep_passive()` | after `uncross`, shift each side so level 0 is one tick inside the book's touch (`keep_passive`) |
| `quiet_book(lo, hi)`, `quiet_book_raw(lo, hi)` | call `on_book` only once the mid reaches `lo` or `hi` or the book turns invalid; floats are rounded inward to 1e-8 |
| `quiet_book()` | ends `quiet_book(lo, hi)` |
| `fail(code)` | stop the strategy with an int code |

`quiet_book` suits a hook that would return at once inside the band: it gives the same result and runs faster in a sweep ([Many runs in one pass](../explanation/backtesting.md#many-runs-in-one-pass)).

When the hook returns, the engine makes at most one call for the instrument, after whichever intent came last:

| Last intent | Engine call |
|---|---|
| `quote`, `bid`, `ask` or `clear` | `set_quotes` |
| `pull` | `pull_quotes` |

A float price or quantity is rounded to the nearest 1e-8, then prices to the tick (bids down, asks up) and quantities down to the lot. A raw level is used as given. A level with a non-positive price or a zero quantity is dropped.

### book

| Field | Meaning |
|---|---|
| `valid`, `ts_ns` | 1 when the book is valid, as `BookView.valid`; last update in engine time, 0 before the first |
| `mid`, `best_bid`, `best_ask`, `best_bid_qty`, `best_ask_qty` | floats, each with a `_raw` field |
| `n_bids`, `n_asks` | levels present in the arrays, at most 10 per side |
| `bid_px[i]`, `bid_qty[i]`, `ask_px[i]`, `ask_qty[i]` | level `i`, 0 best, each with a `_raw` array; entries from `n_bids` or `n_asks` on are 0 |

### fastmm.fx

The C++ fixed-point operators for raw values, usable in plain Python and in hooks. Constants: `SCALE` (100,000,000), `RATIO_PER_BP` (10,000), `BUY`, `SELL`.

| Function | Result |
|---|---|
| `tdiv(a, b)` | integer division truncating toward zero, as in C++; `ZeroDivisionError` when `b` is 0 |
| `mul_ratio(v, r)` | `Fixed * Ratio`: the 128-bit product divided by 1e8, truncating toward zero |
| `bps_ratio(bps_raw)` | the `Ratio` raw value of a bps parameter's `_raw` field: 0.01 bps, raw 1,000,000, gives 100 |
| `round_price(p, tick, side)`, `round_qty(q, lot)` | `round_to_tick` (bids down, asks up) and `round_to_lot` (down) |
| `to_raw(x)`, `to_float(raw)` | the nearest raw value, halves away from zero, `ValueError` when not finite; and back |

### What compiles

Hooks use Numba's nopython subset: numbers, the fields and methods above, loops, tuples and functions decorated with `numba.njit`.

- Bounds checks are on: an index outside an array raises `IndexError`.
- Python's error model applies: a division by zero raises `ZeroDivisionError`.
- Helpers are compiled with bounds checks when a hook first calls them.
- A hook Numba cannot compile raises `HotCompileError` with the hook's name and Numba's message.

Before a run, FastMM scans the LLVM IR of each hook and the helpers it calls. Anything other than an LLVM intrinsic, a libm function or `NRT_MemInfo_call_dtor` raises `fastmm.HotCompileError`, a `TypeError`:

```text
fastmm: Allocates.on_book is rejected by the IR check: it creates an array at strategy.py:21. Hot hooks may not allocate, print or use Python objects (it calls NRT_MemInfo_alloc_aligned).
```

The message names the hook, the symbols, and the allocating or printing line when Numba's typed IR shows it. This rejects arrays, lists, strings, `print` and Python objects. Calls through ctypes or cffi function pointers are not detected and are not supported.

### Running and errors

`run_backtest(..., hot_cache=True)` compiles the hooks and calls each once on scratch copies of `ctx`, `book` and `self`; nothing is sent and `State` is unchanged. It then runs the backtest with the GIL released.

These stop the strategy:

- a hook raises;
- a hook calls `ctx.fail(code)`;
- a hook sets a float price or quantity that is not finite or beyond 9.2e10 in magnitude, or a negative quantity.

No hook runs again and the kill switch trips with `StrategyError`: quotes pulled, working orders cancelled. The backtest ends after that event, and `run_backtest` raises `fastmm.StrategyError`:

| Attribute | Value |
|---|---|
| `hook` | the hook's name |
| `status` | 1 raised, 2 `ctx.fail`, 3 bad float level |
| `fail_code` | the code passed to `ctx.fail` |
| `kill_reason` | `"StrategyError"` |
| `symbol` | the instrument the failing call ran for |
| `now_ns`, `events`, `result` | engine time, engine events and the partial `BacktestResult` |

Numba keeps neither the type nor the message of the exception.

### Numba cache

With `hot_cache=True` compiled hooks are stored under `$FASTMM_CACHE_DIR/numba/`; the default is `$XDG_CACHE_HOME/fastmm` or `~/.cache/fastmm`.

| Level | Directory name is a hash of |
|---|---|
| cache directory | the fastmm version, the hot ABI, FastMM's compiler sources, the numba and llvmlite versions, the CPU |
| one per strategy source | the hooks, the `numba.njit` helpers they reach and the global constants they read, wherever defined, and the bounds-check setting |

A changed helper in another file compiles again.

### Performance

One `on_book` call with `levels = 2`, without the engine, on the same books, positions and parameters, one core of an AMD Ryzen 7 7800X3D ([`bench/python/bench_hot_strategy.py`](../../bench/python/bench_hot_strategy.py)):

| Case | ns per call |
|---|---|
| C++ `BasicMM::on_book` | 31.7 |
| `BasicMMHot` (exact) | 36.8 |
| `BasicMMHot` with bounds checks off | 37.0 |
| `BasicMMHot` with the 10 levels per side copied | 72.0 |
| `BasicMMHotFloat` | 30.4 |

- Bounds checks cost under 1 ns per call.
- The engine copies the level arrays, about 35 ns, only when a hook or a helper it calls names one, or calls code the check cannot follow.
- On the synthetic market C++ `basic_mm` runs at 1.93 M events/s and `BasicMMHot` at 1.90 M, with the same orders.
- Compiling `BasicMMHot`, 5 hooks, takes 1.1 s in a fresh process and 0.5 s from the cache; importing numba takes 0.14 s.

## Slow methods

A class with hot hooks may also define plain Python methods that run one at a time on a slow thread and may use any library. They cannot place orders; they change the hot hooks' parameters by [publishing](#publishing-parameters). Steps: [Run slow methods beside hot hooks](../how-to/strategies/python-slow-methods.md).

| Method | Runs |
|---|---|
| `on_start(self, ctx)` | once, before the engine starts |
| `@fastmm.every("1s", timeout="10s")` on a method taking `(self, ctx)` | when the session starts, then once per period of session time; units `ns`, `us`, `ms`, `s`, `m`, `h` |
| `on_stop(self, ctx)` | once, after the engine stops |

Defining the class raises `TypeError` when:

- an `@fastmm.every` method has another signature or a hook's name;
- the class has `@fastmm.every` methods but no hot hooks;
- it has more than 128 parameters;
- a parameter or `State` field is named `publish` or `inst`.

Hot hooks run on the engine thread. `on_start`, the `@fastmm.every` methods and `on_stop` run on a slow thread: they read the snapshot, recent rows and fills the engine publishes, and send values back with `publish()`.

![The engine thread runs the hot hooks and alone talks to the venue; the slow thread reads a snapshot, recent rows and fills and sends parameters back through the publish ring](../assets/diagrams/slow-methods.svg#only-light){ .fastmm-diagram }
![The engine thread runs the hot hooks and alone talks to the venue; the slow thread reads a snapshot, recent rows and fills and sends parameters back through the publish ring](../assets/diagrams/slow-methods-dark.svg#only-dark){ .fastmm-diagram }

### Slow ctx

| Member | Result |
|---|---|
| `now_ns` | session time of the call in ns: simulated in backtests, `time.time_ns()` live |
| `instruments` | instrument symbols by id |
| `snapshot()` | a [`Snapshot`](#snapshot) of the latest state the engine published |
| `recent(inst)` | `Recent(rows, dropped)`: recent top-of-book changes and trades of one instrument ([Recent rows](#recent-rows)) |
| `fills()` | the fills since the previous call ([Fills](#fills)) |
| `publish(inst=None, **values)` | new parameter values; `True` when the session took the update |

`inst` is an instrument id or symbol.

### Snapshot

The engine publishes a snapshot at most once per 10 ms of engine time; a change held back is published at the end of the interval.

| `Snapshot` field | Meaning |
|---|---|
| `ts_ns` | engine time |
| `age_ms` | `now_ns` minus `ts_ns` in ms; `None` before the first snapshot |
| `version` | the snapshot's number |
| `quoting_enabled` | the engine's quoting flag, false while parameters are stale |
| `killed` | the global kill switch |
| `snap[id]`, `snap["BTCUSDT"]` | the `InstrumentSnapshot` of one instrument, by id or symbol |

| `InstrumentSnapshot` field | Meaning |
|---|---|
| `book_valid`, `book_ts_ns` | the book is valid; its last update in engine time |
| `bid`, `bid_qty`, `ask`, `ask_qty`, `mid` | top of book: prices in the quote currency, quantities in base units |
| `position`, `avg_price`, `fills` | the position, its average entry price and the fill count |
| `realized_pnl`, `unrealized_pnl`, `fees` | quote currency; `unrealized_pnl` at the position's last mark |
| `bid_open_qty`, `ask_open_qty` | unfilled quantity of open orders, quotes included |
| `quoting` | quoting is enabled and the instrument's venue is not killed |
| `param_seq`, `param_age_ms` | number and age in ms of the last update applied to the instrument; `None` before the first |

### Recent rows

`recent(inst).rows` is a numpy array of the newest rows, oldest first. A row is written when the top of book changes and for every trade; the engine keeps `recent_rows` (4,096) per instrument.

| Column | Meaning |
|---|---|
| `ts_ns` | engine time |
| `kind` | 0 the top of book changed, 1 a trade |
| `bid`, `bid_qty`, `ask`, `ask_qty` | top of book at that time |
| `mid` | the mean of `bid` and `ask`; NaN when a side is empty |
| `price`, `qty`, `side` | the trade and its aggressor, `fastmm.BUY` or `fastmm.SELL`; 0 in `kind` 0 rows |

`dropped` counts the rows written since the previous `recent` call that left the window before this one.

### Fills

`fills()` returns a numpy array; order updates do not reach slow methods.

| Column | Meaning |
|---|---|
| `seq` | from 1, consecutive over the session |
| `ts_ns`, `instrument`, `side`, `maker`, `price`, `qty` | the fill |
| `fee` | quote currency |
| `position` | after the fill |

The engine writes fills into a ring of `fills_capacity` fills, which the slow tier empties whenever it runs. A full ring stops the session. The default capacity is:

- 4 fills per order at `[risk] orders_per_sec`, or 1,000 orders/s when unlimited,
- over the longer of the shortest period and the longest timeout plus 1 s,
- at least 4,096, rounded up to a power of two.

### Publishing parameters

`publish` names any subset of the parameters. The values apply together at one engine event; the others keep their values. `inst=None` applies them to every instrument. The hot `on_params` then runs for each instrument the update reached.

- Each name is checked and each value parsed and range-checked as `fastmm.Param` does. `validate()` then runs on a copy of the instance with the new values.
- The first error raises `ValueError` and nothing is sent. An update names at most 32 parameters.
- An update without values changes nothing and renews `max_param_age_ms`.
- With `max_param_age_ms` above 0, quoting is disabled before the first update and while none has applied for that long ([Parameter updates](strategy-api.md#parameter-updates)).
- For a class with `@fastmm.every` methods, a configured `max_param_age_ms` of 0 means 3 times the shortest period, at least 1,000 ms.

`strategy.publish(inst=None, **values)` does the same from any Python thread while a session of that instance runs, for example from a thread started in `on_start`.

- It returns `False` before `on_start` runs and after the session stops.
- In a backtest its update applies at the simulated time the engine has reached when it next checks, which depends on thread timing.

### In backtests

`run_backtest` calls `on_start` before the engine starts and `on_stop` after the run. Each `@fastmm.every` method runs at its simulated times, after the other events of that time. Pending slow runs do not extend a run past its data.

| Argument | Default | Meaning |
|---|---|---|
| `slow_delay_ms` | 0 | simulated ms from a publish to the event that applies it |
| `max_param_age_ms` | `None` | overrides `[strategy] max_param_age_ms`; `None` keeps the configured value or the default above, 0 disables |
| `fills_capacity` | `None` | fills the ring holds |
| `recent_rows` | 4096 | rows per instrument that `recent` keeps |

- `BacktestResult.slow_methods` maps each `@fastmm.every` method to `calls`, `p50_ms`, `p99_ms` and `max_ms` of its wall time.
- A `RuntimeWarning` warns when a method's median wall time is at least 1 ms and above `slow_delay_ms`. Live, that method's publishes apply at least that much later.
- `timeout` is not enforced in backtests.

A slow method that raises, or a full fills ring, ends the run after that event, and `run_backtest` raises `fastmm.StrategyError`:

| Attribute | Value |
|---|---|
| `slow_failure` | `"exception"` or `"fills overflow"` |
| `hook` | the method that raised |
| `__cause__` | the exception |
| `result` | the partial `BacktestResult` |

### Replay

`fastmm.replay(journal, MyMM, verify=True, config=None, params=None, param_updates=True)` replays a journal from a backtest with `config.journal_out` or from a live session.

- The engine consumes the recorded inputs, parameter updates included, with `MyMM`'s hot hooks. Slow methods do not run.
- Parameters start at the recorded values and `max_param_age_ms` is the recorded one.
- The configuration is the one the journal embeds unless `config` is given.
- With `verify=True` every sent message is compared with the recording.

A journal's `strategy_meta` holds the keys listed under [Live sessions](#live-sessions), `max_param_age_ms` and `param.<name>` per starting parameter. A journal without `param.` keys, from an earlier version, takes the parameters from the embedded configuration.

The replay becomes a what-if run, with `what_if` `True` and `what_if_reasons` listing why, when:

- `class`, `hot_source_sha256`, `fastmm` or `numba` differ from the recording;
- `params` is given;
- `param_updates=False`.

| `ReplayResult` field | Meaning |
|---|---|
| `ok` | the replay sent the recorded messages |
| `outbound_sha256`, `recorded_sha256` | hashes of the replayed and recorded messages |
| `outbound_messages`, `recorded_messages`, `events` | message and inbound event counts |
| `first_mismatch`, `expected_message`, `actual_message` | the first differing message; -1 when none |
| `what_if`, `what_if_reasons` | see above |

## Live sessions

`fastmm.run_live` runs a class with hot hooks against the venues in `config`, in this process, and returns the [exit code](cli.md#exit-codes). It needs the live runtime `fastmm-engine-live` ([Install](../getting-started/install.md#python)) and raises `ImportError` without it. Steps: [Run a Python strategy live](../how-to/strategies/python-live.md).

```python
fastmm.run_live(strategy, config, params=None, *, duration=None, dry_run=False, journal=None,
                no_journal=False, status=None, no_status=False, log=None, record_raw=None,
                allow_inline_secrets=False, fills_capacity=None, recent_rows=4096,
                slow_tier_timeout_ms=10000)
```

| Argument | Meaning |
|---|---|
| `strategy` | a `fastmm.Strategy` subclass with `@fastmm.hot` methods, or an unused instance |
| `config` | a `fastmm-live` configuration ([Configuration](configuration.md)) |
| `params` | overrides on top of `[strategy.params]`, which apply only when `[strategy] name` is `py:<Class>`, `<module>:<Class>` or `<Class>` |
| `duration` | seconds, or a string such as `"60s"`; `None` runs until SIGINT or SIGTERM |
| `fills_capacity`, `recent_rows` | as in `run_backtest` ([In backtests](#in-backtests)) |
| `slow_tier_timeout_ms` | how long to wait for the slow thread after the session stops |
| the other keywords | the `fastmm-live` options of the same names ([Command lines](cli.md#fastmm-live)) |

Before any venue is contacted, `run_live` applies the parameters, compiles the hooks without the Numba cache and calls each once on scratch data. It then runs the `fastmm-live` session with the GIL released: the same threads, log, status file, journal, kill switch, `on_kill` and exit codes.

| Case | Result |
|---|---|
| a configuration, parameter or compilation error, a class without hot hooks, or `on_start` raises | a message on stderr; returns 3 |
| a hook raises, calls `ctx.fail` or sets a bad float level | the kill switch trips with `StrategyError` and stderr names the hook; returns 6 with `on_kill = "exit"` |
| the slow tier fails ([Slow methods live](#slow-methods-live)) | the session stops as on SIGTERM; returns 7 |
| `run_live` while a session runs in the process | `RuntimeError`, before `on_start` runs |
| `run_live` in a child forked while a session runs | `RuntimeError`; start children with the `spawn` or `forkserver` method |

- SIGINT and SIGTERM stop the session; `KeyboardInterrupt` is not raised. The session replaces the process's handlers and restores them when it returns.
- While the session starts, loading venue reference data and fees, SIGINT ends the process at once, as in `fastmm-live`.
- Before the engine starts, every thread of the process is limited to the CPUs not in `[engine] cpu` and `net_cpus`, numpy's BLAS threads included. Later threads inherit that; without pinned CPUs nothing changes.
- Thread-count variables such as `OPENBLAS_NUM_THREADS` act only when set before numpy loads.
- `strategy.publish` works from `on_start` until the session stops; after that it returns `False`.

The journal's parameter table lists the class's parameters, and `inspect_journal(path)["strategy_meta"]` returns ([Journal format](journal-format.md)):

| Key | Value |
|---|---|
| `class` | `module:qualname` |
| `hot_source_sha256` | hash of the hot hooks' source and the `numba.njit` functions they call |
| `fastmm`, `numba`, `llvmlite`, `python` | versions |
| `max_param_age_ms` | as configured |
| `param.<name>` | each starting parameter |

`fastmm.replay` replays it ([Replay](#replay)).

### Slow methods live

1. `run_live` moves the calling thread off the CPUs in `[engine] cpu` and `net_cpus`, so the threads it starts next inherit that. It runs `on_start` on the calling thread before any venue is contacted; `on_start` has no timeout.
2. The slow thread `fastmm-slow` starts and runs each `@fastmm.every` method at once, then once per period of wall-clock time. Between calls it empties the fills ring every 10 ms.
3. The session starts. The engine publishes snapshots, recent rows and fills as in backtests. `ctx.publish` and `strategy.publish` share one ring to the engine; a publish returns `False` when it is full.
4. The control thread checks the slow tier every 50 ms without the GIL. A failure stops the session like SIGTERM: kill switch, cancel-all, exit code 7, or 5 when a cancel-all fails.
5. After the session stops, the slow thread finishes its current call, runs `on_stop` and ends. An exception in `on_stop` is printed and does not change the exit code.
6. If the thread has not ended `slow_tier_timeout_ms` after the session stopped, `run_live` warns with `RuntimeWarning` and returns. The thread keeps running as a daemon thread.

| Failure | Log line (ERROR) | stderr |
|---|---|---|
| a slow method raised | `fastmm-live: slow tier failed (Exception): a slow method raised` | `fastmm: py:<Class>.<method> raised <Type>: <message>` and the traceback |
| a call ran past its `timeout` | `fastmm-live: slow tier failed (Timeout): a slow method ran past its timeout` | `fastmm: py:<Class>.<method> ran past its timeout (<n> s); the slow tier stopped the session` |
| the fills ring was full | `fastmm-live: slow tier failed (FillsOverflow): the fills ring was full` | `fastmm: py:<Class>: the fills ring (<n> fills) was full; ...; pass a larger fills_capacity` |
| the slow thread ended while the session ran | `fastmm-live: slow tier failed (ThreadExited): the slow thread ended` | `fastmm: py:<Class>: the slow thread ended while the session ran; ...` |

The log line is followed by `fastmm-live: shutting down (slow tier failed)`.

A method that stalls and stops publishing disables quoting after `max_param_age_ms`. The engine logs `no parameter update for <n> ms (max_param_age_ms): quotes pulled until the next`. With `max_param_age_ms` below the method's `timeout`, this comes before the timeout stops the session.

### python -m fastmm run

`python -m fastmm run <module>:<Class> --config <file> [options]` imports the class, current directory first on the module path, and exits with the code `run_live` returns.

| Option | Meaning |
|---|---|
| `--param key=value` (repeatable), `--duration`, `--dry-run`, `--record-raw`, `--journal`, `--no-journal`, `--status`, `--no-status`, `--log`, `--allow-inline-secrets` | as for `fastmm-live` |
| `--slow-tier-timeout-ms <ms>` | default 10000; past it the process prints `fastmm: exiting with code <n> without waiting for the slow thread` and exits with `os._exit` |

| Exit | Cause |
|---|---|
| 2 | a bad command line |
| 3 | the class cannot be imported |
| otherwise | what `run_live` returns |

Each of `OPENBLAS_NUM_THREADS`, `OMP_NUM_THREADS` and `MKL_NUM_THREADS` that is not set is set to 1, and the interpreter restarts once so that numpy sees them.
