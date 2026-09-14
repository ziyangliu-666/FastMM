# Risk model

FastMM's risk layer answers one question for every order the engine is about to send: may it go?
This page explains where the checks sit, what each one compares, how the kill switch works, and
what the layer deliberately does not do. The code is `include/fastmm/core/risk.hpp`; the limits are
the `[risk]` section of the [Configuration](../reference/configuration.md).

## Where the checks sit

Every new order and every replace passes through `RiskEngine` on the engine thread, after the
strategy decided and before the OMS records the order: quotes from the quote manager and direct
orders from `ctx.send` alike. A refused order is never sent. The strategy sees the
`RejectReason` in the `Result` of `ctx.send`, or a quote that does not appear; the engine counts
rejects per reason (`fastmm-top`, the shutdown summary) and logs the first reject of each reason,
then at most one line per reason every 10 s.

Cancels never pass through the checks: **cancels are always allowed**, also after the kill switch
has tripped, so a strategy or the engine can always reduce what is in the market.

The checks are integer compares on state the engine already keeps. Price bounds are recomputed on
every book and trade update, so the check itself costs a few nanoseconds
([README](../../README.md#latency): 8.1 ns for all of them).

## The checks, in order

The first failing check decides the reason.

| # | Reason | Refuses an order when | Setting |
|---:|---|---|---|
| 1 | `KillSwitch` | the global kill switch is set | |
| 2 | `VenueKilled` | the venue's kill bit is set | |
| 3 | `InstrumentDisabled` | the instrument is disabled or out of range | `[[instruments]] enabled` |
| 4 | `InvalidTick` | a limit price is not a multiple of the tick | `tick` |
| 5 | `InvalidLot` | the quantity is not a multiple of the lot or outside `min_qty`/`max_qty` | `lot`, `min_qty`, `max_qty` |
| 6 | `BelowMinNotional` | price times quantity (the mid for market orders) is below the minimum | `min_notional` |
| 7 | `StaleMarketData` | the instrument's book is older than the limit, or there is none | `stale_md_ms` |
| 8 | `PriceCollar` | a limit price is further from the mid than the collar | `price_collar_bps` |
| 9 | `FatFinger` | a limit price is further from the last trade than the band | `fat_finger_bps` |
| 10 | `MaxOrderQty` | the quantity exceeds the limit | `max_order_qty` |
| 11 | `MaxOrderNotional` | the order value exceeds the limit | `max_order_notional` |
| 12 | `MaxPosition` | position plus same-side open orders plus this order would exceed the limit in absolute value and increase exposure | `max_position` |
| 13 | `MaxOpenOrders` | the instrument already has this many open orders (new orders only) | `max_open_orders` |
| 14 | `SelfTradePrevention` | a limit price would trade against one of our own resting orders | `stp` |
| 15 | `RateLimit` | the token bucket is empty | `orders_per_sec`, `burst` |

- A limit of 0 turns its check off; the checks against the instrument's reference data always run.
- A replace excludes the existing order's remaining quantity from the position prediction and is
  not counted against `max_open_orders`.
- `MaxPosition` counts open orders on the same side, so a ladder of quotes cannot add up to more
  than the limit even if every order fills. An order that reduces exposure passes.
- Market orders skip the price checks (4, 8, 9, 14).

## The kill switch

The kill switch is a 32-bit flag word that any thread can set: bit 0 is global, bit 1 + v is venue
v. While a bit is set, checks 1 and 2 refuse every new order and replace. Tripping the global bit
also turns quoting off (`on_quoting(false)`), pulls every quote and cancels every working order;
tripping a venue's bit does that for that venue's instruments only, and the other venues keep
trading. The engine records why each bit was first set (a `KillReason`, shown by `fastmm-top`).

The global switch trips when:

- the session shuts down: Ctrl-C, SIGTERM, `--duration` or an order ring overflow (the control
  thread requests it, then cancels all orders on every venue over a separate REST connection);
- `[risk] max_loss` is reached: net PnL (realised plus unrealised, marked at the mid, minus fees)
  is re-evaluated on every book update and fill;
- the outbound ring to a venue or the journal ring is full, because the engine can no longer
  guarantee that what it sends is what it records;
- every venue with instruments has been killed.

A venue's switch trips when its connector reports an error that makes the venue unusable: a bad
key, signature or permission, failed authentication, or a Binance IP ban. The command travels
through the venue's order ring, so it is journaled and a replay trips it at the same point.

After a kill the engine tripped itself (the last three causes), `fastmm-live` runs the normal
shutdown and exits with code 6; with `[engine] on_kill = "stay"` it keeps running with quoting off
so an operator can inspect it. Nothing resets a kill switch automatically.
[Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md) has the log lines and
the shutdown sequence.

## What the layer does not do

- **It does not replace the strategy's own limits.** `first_mm` stops quoting a side at
  `max_position`; `[risk] max_position` is a second, independent limit that holds even when the
  strategy has a bug. Set the risk limit above the strategy's.
- **It keeps no cross-instrument or cross-venue exposure.** Position limits are per instrument;
  `max_loss` is the only portfolio-wide limit.
- **It does not know the venue's rules beyond reference data.** Rate limits, margin and account
  balances are enforced by the venue; the connectors handle rate-limit responses and the engine
  pauses a side after venue rejects (`[engine] reject_backoff_ms`).
- **It does not protect against a venue that stops answering.** Order-channel loss triggers a
  REST cancel-all in the connector, and the operator's last resort is the venue's own cancel-all
  ([Go-live checklist](../how-to/operations/go-live-checklist.md)).
