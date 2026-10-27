# Hedge with HedgeExecutor

`HedgeExecutor` keeps a strategy's exposure hedged with taker orders on other instruments. The strategy configures it and forwards its hooks; the executor sizes, sends, fails over and de-risks.

`xmm` is built on it ([Quote on one venue, hedge on another](xmm.md)). [`examples/cpp/hedged_mm.cpp`](../../../examples/cpp/hedged_mm.cpp) is a small quoter that uses it.

## Set it up

Add a `HedgeExecutor` member to the strategy. In `on_start`, add the sources and the hedges, then call `start`. Sources are the instruments whose position is the exposure. Hedges take the other side, in order of preference, each with its own tolerance: how far through the touch its IOC is priced.

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

`start` returns false, with the reason in the log, for an instrument that is not configured, is inverse or is named twice.

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

Quote from what the executor can do. Pull the quotes while `can_hedge(ctx)` is false, and quote only the side that reduces the residual while `held()`. A de-risk order then never crosses a quote of the strategy.

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

`can_hedge` is false when no hedge instrument can take a hedge or hedging is halted. `set_target(qty)` moves the net base position the executor hedges to, 0 by default. `set_config` and `set_tolerance` change parameters while it runs, from `on_params`; the legs are fixed at start.

## What it sends

```text
residual = sum of source positions + sum of hedge positions - target     (base units: qty * contract_multiplier)
hedge    = IOC limit on the first usable hedge instrument, -residual in its contracts,
           rounded down to the lot, capped at max_qty, priced its tolerance through the touch,
           then cut to what [risk] admits for one order
```

Sizing comes from positions, not from fill counts. A restart, a replayed or duplicated fill and a fill booked late by reconciliation lead to the same hedge.

One order is in flight at a time, across all hedge instruments and the de-risk orders. Two orders sized from the same residual on two venues could both fill, so a hedge still open on one venue blocks the next one on another. A residual over `max_qty` goes out in pieces, each after the previous one ends.

| Situation | What the executor does |
|---|---|
| hedge larger than a `[risk]` limit | cuts each order to `ctx.risk_headroom` of its instrument and sends the rest after; logs it once per episode |
| `[risk]` admits less than one lot or `min_notional` | sends the order whole; the engine refuses it, which counts as a failure of the instrument |
| residual under the lot, `min_qty` or `min_notional` of the first usable instrument | waits for more exposure; does not move it to another venue |
| residual under the lot or `min_qty` of every hedge instrument | waits, usable or not; does not de-risk it |
| a venue is reconciling (`ctx.reconciling()`) | sends nothing |
| order refused, or ends with nothing filled | counts a failure of the instrument; the next order waits `retry` |
| order ends without the venue saying how | holds all orders for `uncertain_hold`, so a fill still on its way is booked first |

The `ctx.risk_headroom` cut covers `max_order_qty`, `max_order_notional` at the order's price, the side's room under `max_position` and `[risk.underlying]` `max_net`, and what `max_gross_notional` and `max_net_notional` leave on that side. With `max_order_notional = "60"`, a hedge of 0.1 contracts of 0.01 BTC at 83914.2 USDT (83.91 USDT) goes out as 0.07 and then 0.03. De-risk orders are cut the same way.

An outcome the venue does not report is an ack-timeout cancel, a reconciliation drop with quantity unaccounted for, or a generic `VenueReject` such as a REST timeout.

## Failover

The executor skips a hedge instrument, and the next one in the list takes the hedge, while one of these holds:

| Condition | `Reason` |
|---|---|
| an order channel of its venue is not `Live` | `Down` |
| its venue's kill switch is on (`ctx.venue_killed`) | `Killed` |
| its book is invalid | `NoBook` |
| its book is older than `stale` | `Stale` |
| the feed-lag gate holds its venue (`[risk] max_feed_lag_ms`) | `Gated` |
| it failed `max_failures` times within `failure_window` | `Benched` for `bench` |
| its balance or margin cannot cover the hedge (`ctx.balance_room`) | hedge sent to the next usable one |

Hedges return to an earlier instrument as soon as it is usable again. When no usable instrument's balance covers the hedge, the hedge is held (`held()`) until a balance report or a fill changes that. When the last instrument that is not benched fails `max_failures` times, the executor halts until `restart()`.

## De-risk

With `derisk_after` set, a residual that no hedge instrument has taken for that long is reduced on a source whose position has the residual's sign. This covers all hedges unusable, balance short, or halted.

| De-risk order | Value |
|---|---|
| type | reduce-only IOC, `derisk_tolerance` through the source's touch |
| size | at most `derisk_step` base units |
| pacing | `derisk_interval` apart, one in flight |
| stops | when a hedge instrument takes the residual again, or the residual is gone |

The executor sends these orders itself so they share the hedges' sizing, in-flight rule and holds. The quoting rule in [Set it up](#set-it-up) keeps the strategy's quotes off their side.

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

| Call | Returns |
|---|---|
| `status(ctx)` | `State` of the last evaluation, the residual, the active hedge instrument, in-flight, held, halted and de-risking flags, the end of the last retry or uncertain hold |
| `stats()` | counts of hedges sent, failures, unreported outcomes, halts, held episodes, failovers, benches, de-risk episodes and de-risk orders |
| `why(ctx, i, now)` | the `Reason` instrument `i` is skipped |

The states are `Flat`, `Waiting`, `InFlight`, `Reconciling`, `Holding`, `Held`, `Unavailable` and `Halted`. Each transition is logged once: a failover or a return, a bench, a halt, a held hedge and its release, no instrument usable, a de-risk episode's start and end.

## Test it

```bash
ctest --test-dir build/release -R 'hedged_mm|hedge_executor|xmm failover'
```

| Test | Covers |
|---|---|
| [`tests/strategies/hedge_executor_test.cpp`](../../../tests/strategies/hedge_executor_test.cpp) | the executor against a fake context; `xmm` with a fallback through the engine with three venues |
| [`tests/integration/xmm_failover_test.cpp`](../../../tests/integration/xmm_failover_test.cpp) | `fastmm-live` against three simulators, killed with `SIGKILL` while the fallback holds a hedge's reply, then restarted |
| `examples.hedged_mm` | the example above |
