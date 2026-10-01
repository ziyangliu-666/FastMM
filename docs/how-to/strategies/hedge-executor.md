# Hedge with HedgeExecutor

`HedgeExecutor` keeps a strategy's exposure hedged with taker orders on one or more other instruments. The strategy owns one, configures it in `on_start` and forwards its hooks; the executor sizes, sends, fails over and de-risks. Source: [`include/fastmm/strategies/hedge_executor.hpp`](../../../include/fastmm/strategies/hedge_executor.hpp). `xmm` is built on it ([Quote on one venue, hedge on another](xmm.md)); [`examples/cpp/hedged_mm.cpp`](../../../examples/cpp/hedged_mm.cpp) is a small quoter that uses it.

## Set it up

Add a `HedgeExecutor` member to the strategy. In `on_start`, name the legs and start it. Sources are the instruments whose position is the exposure; hedges are the instruments that take the other side, in order of preference, each with its own tolerance (how far through the touch its IOC is priced). `start` returns false, with the reason in the log, for an instrument that is not configured, is inverse or is named twice.

<!-- snippet: examples/cpp/hedged_mm.cpp#start -->
```cpp
void on_start(auto& ctx) noexcept {
  const HedgedParams& p = params();
  HedgeExecutor::Config c;
  c.name = "hedged_mm";
  c.tag = 0x4844'4745;  // any tag outside the quote manager's range
  c.derisk_tag = 0x4844'5253;
  c.stale = milliseconds(2000);
  c.derisk_after = p.derisk_after_ms;
  c.derisk_step = p.quote_qty;
  c.derisk_interval = milliseconds(500);
  c.derisk_tolerance = p.hedge_tolerance_bps;
  hedge_.reset();
  static_cast<void>(hedge_.add_source(kQuote));
  static_cast<void>(hedge_.add_hedge(InstrumentId{1}, p.hedge_tolerance_bps));
  static_cast<void>(hedge_.add_hedge(InstrumentId{2}, p.hedge_tolerance_bps));
  if (!hedge_.start(ctx, c)) return;
  static_cast<void>(ctx.every(milliseconds(100), kTimer));
}
```

Forward the hooks. Each returns whether the event concerned the executor, so the strategy can requote only then. Call `on_timer` from a timer of the strategy: the retry, the uncertain hold and the de-risk steps end at the first call after them.

<!-- snippet: examples/cpp/hedged_mm.cpp#hooks -->
```cpp
// The executor sees every hook it needs; the quotes follow what it can do.
void on_book(auto& ctx, InstrumentId id, const auto&) noexcept {
  hedge_.on_book(ctx, id);
  quote(ctx);
}
void on_fill(auto& ctx, const Fill& f) noexcept {
  if (hedge_.on_fill(ctx, f)) quote(ctx);
}
void on_order_update(auto& ctx, const OmsUpdate& u) noexcept {
  if (hedge_.on_order_update(ctx, u)) quote(ctx);
}
void on_timer(auto& ctx, TimerId, std::uint64_t) noexcept {
  hedge_.on_timer(ctx);
  quote(ctx);
}
void on_connection(auto& ctx, const ConnectionStateMsg& m) noexcept {
  hedge_.on_connection(ctx, m);
  quote(ctx);
}
void on_balance(auto& ctx, const BalanceMsg& m) noexcept {
  if (hedge_.on_balance(ctx, m)) quote(ctx);
}
```

Quote from what the executor can do: pull the quotes while `can_hedge(ctx)` is false (no hedge instrument can take a hedge, or hedging is halted), and quote only the side that reduces the residual while `held()`. A de-risk order then never crosses a quote of the strategy.

<!-- snippet: examples/cpp/hedged_mm.cpp#quote -->
```cpp
void quote(auto& ctx) noexcept {
  const auto& b = ctx.book(kQuote);
  if (!hedge_.ready() || !b.is_valid() || !hedge_.can_hedge(ctx)) {
    ctx.pull_quotes(kQuote);
    return;
  }
  const Instrument& inst = ctx.instrument(kQuote);
  const Price m = b.mid();
  const Price half = m * params().half_spread_bps;
  const Qty qty = inst.round_qty(params().quote_qty);
  const Qty open = hedge_.residual(ctx);
  DesiredQuotes q;
  if (!(hedge_.held() && open.is_positive())) q.bid(inst.round_price(m - half, Side::Buy), qty);
  if (!(hedge_.held() && open.is_negative())) q.ask(inst.round_price(m + half, Side::Sell), qty);
  keep_passive(q, b.best_bid().price, b.best_ask().price, inst.tick);
  ctx.set_quotes(kQuote, q);
}
```

`set_target(qty)` moves the net base position the executor hedges to (0 by default). `set_config` and `set_tolerance` take new parameters while it runs (from `on_params`); the legs are fixed at start.

## What it sends

```text
residual = sum of source positions + sum of hedge positions - target     (base units: qty * contract_multiplier)
hedge    = IOC limit on the first usable hedge instrument, -residual in its contracts,
           rounded down to the lot, capped at max_qty, priced its tolerance through the touch,
           then cut to what [risk] admits for one order
```

- Sizing comes from positions, never from fill counts: a restart, a replayed or duplicated fill and a fill booked late by reconciliation lead to the same hedge.
- One order is in flight at a time, across every hedge instrument and the de-risk orders. Two orders sized from the same residual on two venues could both fill; a hedge still open on one venue blocks the next one on another until it ends. The residual over `max_qty` goes out in pieces, each after the previous one ends.
- A hedge larger than a `[risk]` limit goes out in pieces too. Each order is cut to `ctx.risk_headroom` of its instrument: `max_order_qty`, `max_order_notional` at the order's price, on its side the room of `max_position` and of `[risk.underlying]` `max_net`, and what `max_gross_notional` and `max_net_notional` leave for one order on that side (`exposure_buy_notional` / `exposure_sell_notional`, in the instrument's currency), rounded down to the lot. With `max_order_notional = "60"`, 0.1 contracts of 0.01 BTC at 83914.2 (83.91 USDT) go out as 0.07 and then 0.03. The log says so once per episode. De-risk orders are cut the same way.
- When those limits admit nothing the instrument's minimums allow (the position room is used up, or the cap is under one lot or `min_notional`), the order goes out whole, the engine refuses it and that is a failure of the instrument: it fails over or halts as below.
- A residual that rounds under the lot, `min_qty` or `min_notional` of the first usable instrument waits for more exposure. It is not sent to be refused, and it is not moved to another venue.
- No order goes out while a venue reconciles (`ctx.reconciling()`).
- An order the engine refuses, or that ends with nothing filled, is a failure of its instrument; the next waits `retry`.
- An order that ends without the venue saying how (the ack-timeout cancel, a reconciliation drop with quantity unaccounted for, a generic `VenueReject` such as a REST timeout) holds every order for `uncertain_hold`, so a fill still on its way is booked first.

## Failover

A hedge instrument is skipped, and the next one in the list takes the hedge, while:

| Condition | `Reason` |
|---|---|
| an order channel of its venue is not `Live` | `Down` |
| its venue's kill switch is on (`ctx.venue_killed`) | `Killed` |
| its book is invalid | `NoBook` |
| its book is older than `stale` | `Stale` |
| the feed-lag gate holds its venue (`[risk] max_feed_lag_ms`) | `Gated` |
| it failed `max_failures` times within `failure_window` | `Benched` for `bench` |
| its balance or margin cannot cover the hedge (`ctx.balance_room`) | hedge sent to the next usable one |

Hedges come back to an earlier instrument as soon as it is usable again. When no usable instrument's balance covers the hedge, it is held (`held()`, `Stats::hedges_held` once per episode) until a balance report or a fill changes that. When the last instrument that is not benched fails `max_failures` times, the executor halts: no more hedges until `restart()`.

## De-risk

With `derisk_after` set, a residual that no hedge instrument has taken for that long (all unusable, balance short, or halted) is reduced on a source whose position has the residual's sign: reduce-only IOC orders priced `derisk_tolerance` through its touch, each at most `derisk_step` base units, `derisk_interval` apart, one in flight. It stops when a hedge instrument takes the residual again or the residual is gone. The executor sends these orders itself rather than asking the strategy: they need the same sizing, in-flight rule and holds as the hedges, and the quoting rule above keeps the strategy's quotes off their side.

## Configuration

| `Config` field | Default | Meaning |
|---|---|---|
| `name` | `"hedge"` | log prefix |
| `tag`, `derisk_tag` | 0 | `user_tag` of hedge and de-risk orders, outside the quote manager's range |
| `retry` | 200 ms | wait after a failure |
| `max_failures`, `failure_window` | 5, 60 s | failures that bench an instrument, or halt on the last one |
| `bench` | 60 s | how long a benched instrument sits out |
| `uncertain_hold` | 5 s | wait after an unreported outcome |
| `stale` | 0 (never) | book age that makes a hedge instrument unusable |
| `derisk_after` | 0 (never) | time without a hedge before de-risking |
| `derisk_step` | 0 (the residual) | largest de-risk order, base units |
| `derisk_interval` | 1 s | wait between de-risk orders |
| `derisk_tolerance` | 0 | de-risk IOC distance through the touch |

## Monitor it

`status(ctx)` returns the `State` of the last evaluation (`Flat`, `Waiting`, `InFlight`, `Reconciling`, `Holding`, `Held`, `Unavailable`, `Halted`), the residual, the active hedge instrument, whether an order is in flight, held, halted or de-risking, and the end of the last retry or uncertain hold. `stats()` counts hedges sent, failures, unreported outcomes, halts, held episodes, failovers, benches, de-risk episodes and de-risk orders. `why(ctx, i, now)` gives an instrument's `Reason`. Each transition is logged once: a failover or a return, a bench, a halt, a held hedge and its release, no instrument usable, a de-risk episode's start and end.

## Test it

[`tests/strategies/hedge_executor_test.cpp`](../../../tests/strategies/hedge_executor_test.cpp) drives the executor against a fake context and runs `xmm` with a fallback through the engine with three venues. [`tests/integration/xmm_failover_test.cpp`](../../../tests/integration/xmm_failover_test.cpp) runs `fastmm-live` against three simulators, kills it with `SIGKILL` while the fallback holds a hedge's reply, and restarts it. The example runs as `ctest -R examples.hedged_mm`.
