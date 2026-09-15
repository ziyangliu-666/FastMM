# Write hot hooks in Python

Hot hooks are strategy methods that Numba compiles and the engine thread calls without the GIL; they run in backtests and [live sessions](python-live.md). Reference: [Hot hooks](../../reference/python-api.md#hot-hooks).

## Install

Install numba with the `hot` extra (CPython 3.10 or later):

```bash
pip install "fastmm-engine[hot]"
```

## Declare the strategy

Declare parameters with `fastmm.Param` and per-instrument values that persist between calls with `fastmm.State`. Mark each hook with `@fastmm.hot` (`on_book`, `on_fill`, `on_quoting`, `on_connection`) or with `@fastmm.hot(every="100ms")` for a timer; every hook takes `(self, ctx, book)`:

<!-- snippet: examples/python/strategies/basic_mm_hot.py#class -->
```python
class BasicMMHot(fastmm.Strategy):
    """Symmetric quotes around mid, skewed by inventory, with a per-side inventory cap. Parameter
    names, defaults and ranges match the C++ BasicMM."""

    half_spread_bps = Param(5.0, min=0.0, max=10000.0, doc="half spread around mid, basis points")
    skew_bps_per_unit = Param(
        1.0, min=0.0, max=10000.0,
        doc="shift both quotes by this many bps per quote_qty of inventory")
    quote_qty = Param(0.01, min=0.0, max=1000000000.0, doc="quantity per level (base units)")
    max_inventory = Param(
        0.1, min=0.0, max=1000000000.0,
        doc="stop quoting the side that would grow |position| past this (0 = no cap)")
    requote_threshold_ticks = Param(
        1, min=0, max=1000000, doc="ignore mid moves smaller than this many ticks")
    pull_on_stale_ms = Param(
        2000, min=0, max=3600000, doc="pull quotes when the book is older than this (0 = never)")
    levels = Param(1, min=1, max=8, doc="quote levels per side")
    level_step_ticks = Param(1, min=1, max=100000, doc="tick distance between successive levels")
    last_mid = State(0, doc="mid of the last accepted quotes, raw")

    @fastmm.hot
    def on_book(self, ctx, book):
        if not book.valid:
            ctx.pull()
            self.last_mid = 0
            return
        mid = book.mid_raw
        if self.last_mid > 0 and abs(mid - self.last_mid) < ctx.tick_raw * self.requote_threshold_ticks:
            return
        requote(self, ctx, book)

    @fastmm.hot
    def on_fill(self, ctx, book):
        # Inventory changed: re-skew immediately (bypasses the mid-move threshold).
        if book.valid:
            requote(self, ctx, book)

    @fastmm.hot
    def on_quoting(self, ctx, book):
        self.last_mid = 0
        if ctx.quoting_enabled and book.valid:
            requote(self, ctx, book)

    @fastmm.hot
    def on_connection(self, ctx, book):
        self.last_mid = 0
        if ctx.connected and book.valid:
            requote(self, ctx, book)

    @fastmm.hot(every="100ms")
    def check_stale(self, ctx, book):
        if self.pull_on_stale_ms > 0 and book.ts_ns != 0:
            if (ctx.now_ns - book.ts_ns) // 1_000_000 > self.pull_on_stale_ms:
                ctx.pull()
                self.last_mid = 0
```

## Write intents

Write intents through `ctx` (`quote`, `bid`, `ask`, `clear`, `pull`, `uncross`, `keep_passive`); the engine sends them when the hook returns. Put code that several hooks share in a `numba.njit` function:

<!-- snippet: examples/python/strategies/basic_mm_hot.py#requote -->
```python
# A helper called from hot hooks is a numba.njit function; FastMM compiles it with bounds checks and
# runs the IR check on it with the hook.
@numba.njit
def requote(self, ctx, book):
    ctx.clear()  # quote nothing unless levels follow (C++: an empty DesiredQuotes)
    mid = book.mid_raw
    pos = ctx.position_raw
    q = self.quote_qty_raw
    if q != 0:
        units = fx.tdiv(pos, q)  # Qty / Qty, truncating
        half = fx.mul_ratio(mid, fx.bps_ratio(self.half_spread_bps_raw))
        centre = mid - fx.mul_ratio(mid, fx.bps_ratio(self.skew_bps_per_unit_raw) * units)
        tick = ctx.tick_raw
        step = tick * self.level_step_ticks
        qty = fx.round_qty(q, ctx.lot_raw)
        limit = self.max_inventory_raw
        can_buy = limit == 0 or pos + q <= limit
        can_sell = limit == 0 or pos - q >= -limit
        for level in range(self.levels):
            if can_buy:
                ctx.bid_raw(fx.round_price(centre - half - step * level, tick, fx.BUY), qty)
            if can_sell:
                ctx.ask_raw(fx.round_price(centre + half + step * level, tick, fx.SELL), qty)
        ctx.uncross()
    ctx.keep_passive()
    # C++ remembers the mid only if set_quotes takes the quotes, which it does while quoting is
    # enabled for the instrument.
    self.last_mid = mid if ctx.quoting_enabled else 0
```

To send the same orders as a C++ strategy, use the `_raw` fields, `ctx.bid_raw` and `ctx.ask_raw`, and `fastmm.fx` for the C++ operators, as `requote` does. Float prices and quantities are rounded to the nearest 1e-8, then to the tick and lot.

## Run a backtest

Pass the class to `fastmm.run_backtest(cfg, data=..., strategy=BasicMMHot)`. The first run compiles the hooks; later runs load them from the Numba cache. The example compares the hot ports with the C++ `basic_mm`:

```bash
python examples/python/strategies/basic_mm_hot.py
```

```text
C++ basic_mm        482b01d7194101894d9492627ea2aa455b53a6ee99fa6bb10914a2169828c186  (54 messages)
py:BasicMMHot       482b01d7194101894d9492627ea2aa455b53a6ee99fa6bb10914a2169828c186  (54 messages)
py:BasicMMHotFloat  482b01d7194101894d9492627ea2aa455b53a6ee99fa6bb10914a2169828c186  (54 messages)
committed           482b01d7194101894d9492627ea2aa455b53a6ee99fa6bb10914a2169828c186
identical
```

## Fix what the checks report

| Error | Cause |
|---|---|
| `TypeError` when the class is defined | a plain `fastmm.Strategy` hook in the class, a wrong hook name or signature, a name declared twice, or an assignment to a parameter |
| `HotCompileError: ... does not compile in Numba nopython mode` | code outside Numba's nopython subset; Numba's message follows, with the line |
| `HotCompileError: ... is rejected by the IR check: it calls <symbol>` | an array, list, string, `print` or Python object in the hook or a helper |
| `StrategyError` with `status == 1` | the hook raised, for example an index outside an array or a division by zero |
| `StrategyError` with `status == 3` | a float price or quantity that is not finite, out of range or a negative quantity |
