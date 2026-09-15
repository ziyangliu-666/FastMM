# Run slow methods beside hot hooks

Slow methods are plain Python methods of a hot strategy that read the engine's state and publish new parameter values for the hot hooks. Reference: [Slow methods](../../reference/python-api.md#slow-methods).

## Declare the strategy

Write the quoting as hot hooks over parameters and the model as an `@fastmm.every` method. The engine does not quote before the first publish, so `on_start` publishes the starting values:

<!-- snippet: examples/python/strategies/hot_slow_mm.py#class -->
```python
class HotSlowMM(fastmm.Strategy):
    half_spread_bps = Param(0.1, min=0.0, max=1000.0, doc="half spread around the fair value, bps")
    fair_offset_bps = Param(0.0, min=-20.0, max=20.0, doc="fair value minus mid, bps")
    quote_qty = Param(0.001, min=0.0, max=1.0, doc="quantity per side, base units")
    horizon_rows = Param(20, min=1, max=1000, doc="book changes ahead that the model predicts")
    fair = State(0.0, doc="the last fair value quoted around")

    @fastmm.hot
    def on_book(self, ctx, book):
        quote(self, ctx, book)

    @fastmm.hot
    def on_params(self, ctx, book):
        quote(self, ctx, book)  # requote as soon as the offset changes

    def on_start(self, ctx):
        self.fits = 0
        ctx.publish(fair_offset_bps=0.0)  # quoting starts with the first publish

    @fastmm.every("1s")
    def refit(self, ctx):
        rows = ctx.recent(0).rows
        book = rows[rows["kind"] == 0]
        depth = book["bid_qty"] + book["ask_qty"]
        book = book[(depth > 0) & np.isfinite(book["mid"])]
        h = self.horizon_rows
        if len(book) < h + 50:
            ctx.publish()  # no new values; keeps the parameters within max_param_age_ms
            return
        imbalance = (book["bid_qty"] - book["ask_qty"]) / (book["bid_qty"] + book["ask_qty"])
        move_bps = (book["mid"][h:] / book["mid"][:-h] - 1.0) * 1e4
        beta = np.linalg.lstsq(imbalance[:-h, None], move_bps, rcond=None)[0][0]
        offset = float(np.clip(beta * imbalance[-1], -20.0, 20.0))
        self.fits += 1
        ctx.publish(fair_offset_bps=offset if np.isfinite(offset) else 0.0)
```

The hot `on_params` hook requotes when an update arrives. Both hooks call a `numba.njit` helper:

<!-- snippet: examples/python/strategies/hot_slow_mm.py#quote -->
```python
@numba.njit
def quote(self, ctx, book):
    if not book.valid:
        ctx.pull()
        return
    fair = book.mid * (1.0 + self.fair_offset_bps * 1e-4)
    half = fair * self.half_spread_bps * 1e-4
    ctx.quote(fair - half, fair + half, self.quote_qty)
    ctx.keep_passive()
    self.fair = fair
```

## Read the engine's state

`ctx.recent(inst).rows` holds recent top-of-book changes and trades as a numpy array. `ctx.snapshot()` holds each instrument's book, position, PnL, quoting state and parameter age. `ctx.fills()` returns the fills since the previous call.

Both lag the engine: `snapshot().age_ms` and each instrument's `param_age_ms` give the age in ms.

## Publish

`ctx.publish(fair_offset_bps=offset)` changes the named parameters for every instrument; `inst=` limits the update to one instrument. An unknown name or an invalid value raises `ValueError` in the slow method, and nothing is sent.

The engine stops quoting when no update has applied for `max_param_age_ms`, which defaults to 3 periods of the fastest slow method. A method with nothing to change calls `ctx.publish()` to renew it.

## Run a backtest and replay it

Set `slow_delay_ms` to about the wall time the model takes, record a journal and replay it:

<!-- snippet: examples/python/strategies/hot_slow_mm.py#run -->
```python
cfg.journal_out = str(Path(tmp) / "hot_slow_mm.fmj")
result = fastmm.run_backtest(cfg, data="synthetic", strategy=HotSlowMM, slow_delay_ms=5)
refit = result.slow_methods["refit"]
print(f"{result.strategy}: {len(result.fills['ts'])} fills, {result.outbound_messages} "
      f"messages, net PnL {result.stats()['net_pnl']:.4f}")
print(f"refit: {refit['calls']} calls, wall p50 {refit['p50_ms']:.3f} ms, "
      f"p99 {refit['p99_ms']:.3f} ms")
replayed = fastmm.replay(cfg.journal_out, HotSlowMM)
print(f"replay: {'identical' if replayed.ok else 'different'} outbound hash, "
      f"what-if: {replayed.what_if}")
```

```bash
python examples/python/strategies/hot_slow_mm.py
```

`result.slow_methods` reports each slow method's wall time, and `run_backtest` warns when the median is above `slow_delay_ms`. The replay does not run slow methods; the parameter updates come from the journal.

## Fix what the checks report

| Error | Cause |
|---|---|
| `TypeError` when the class is defined | a slow method with another signature or a hook's name, `@fastmm.every` without hot hooks, or more than 32 parameters |
| `ValueError` from `ctx.publish` | an unknown or `State` name, a value of the wrong type or out of range, a failed `validate()`, or more than 32 names |
| `StrategyError` with `slow_failure == "exception"` | a slow method raised; `hook` names it |
| `StrategyError` with `slow_failure == "fills overflow"` | more fills arrived between two runs of the slow methods than `fills_capacity` |
| `ReplayResult.what_if` is `True` | the class, hot-hook source, fastmm or numba version, or parameters differ from the recording (`what_if_reasons`) |
