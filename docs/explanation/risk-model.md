# Risk model

The risk layer decides whether each new order and replace the engine is about to send may go. The code is `include/fastmm/core/risk.hpp`; the limits are the `[risk]` section of the [Configuration](../reference/configuration.md#risk).

## Where the checks sit

Every new order and every replace passes through `RiskEngine` on the engine thread, after the strategy decided and before the OMS records the order: quotes from the quote manager and direct orders from `ctx.send` alike. A refused order is never sent. The strategy sees the `RejectReason` in the `Result` of `ctx.send`, or a quote that does not appear; the engine counts rejects per reason (`fastmm-top`, the shutdown summary) and logs the first reject of each reason, then at most one line per reason every 10 s.

Cancels skip the checks, also after the kill switch has tripped, so a strategy or the engine can always reduce what is in the market.

The checks are integer compares on state the engine already keeps; price bounds are recomputed on every book and trade update. `BM_Risk_CheckNew_Pass`, an order that passes all checks, takes 6.5 ns ([bench/README.md](../../bench/README.md)).

## The checks, in order

The first failing check decides the reason.

| # | Reason | Refuses an order when | Setting |
|---:|---|---|---|
| 1 | `KillSwitch` | the global kill switch is set | |
| 2 | `VenueKilled` | the venue's kill bit is set | |
| 3 | `InstrumentDisabled` | the instrument is disabled or out of range | `[[instruments]] enabled` |
| 4 | `InvalidTick` | a limit price is not a multiple of the tick | `tick` |
| 5 | `InvalidLot` | the quantity is not a multiple of the lot or outside `min_qty`/`max_qty` | `lot`, `min_qty`, `max_qty` |
| 6 | `BelowMinNotional` | the order notional (the mid for market orders) is below the minimum | `min_notional` |
| 7 | `StaleMarketData` | the instrument's book is older than the limit, or there is none | `stale_md_ms` |
| 8 | `FeedLag` | the venue's feed-lag gate holds, the order could rest (not IOC, FOK or market) and it does not only reduce the position ([Feed-lag gate](#feed-lag-gate)) | `max_feed_lag_ms` |
| 9 | `PriceCollar` | a limit price is further from the mid than the collar | `price_collar_bps` |
| 10 | `FatFinger` | a limit price is further from the last trade than the band | `fat_finger_bps` |
| 11 | `MaxOrderQty` | the quantity exceeds the limit | `max_order_qty` |
| 12 | `MaxOrderNotional` | the order value exceeds the limit | `max_order_notional` |
| 13 | `MaxPosition` | position plus same-side open orders plus this order would exceed the limit in absolute value and increase exposure | `max_position` |
| 14 | `FxRateUnknown` | the order adds to exposure in a settlement currency whose rate is unknown or stale, while a limit reads the totals | `[accounting]` |
| 15 | `MaxGrossNotional` | the portfolio's summed \|position\| at the last marks would pass the cap, and this order adds to it | `max_gross_notional` |
| 16 | `MaxNetNotional` | the portfolio's signed position sum would move further past the cap | `max_net_notional` |
| 17 | `MaxOpenOrders` | the instrument already has this many open orders (new orders only) | `max_open_orders` |
| 18 | `SelfTradePrevention` | a limit price would trade against one of our own resting orders | `stp` |
| 19 | `RateLimit` | the token bucket is empty | `orders_per_sec`, `burst` |

- A limit of 0 turns its check off; the checks against the instrument's reference data always run.
- A replace excludes the existing order's remaining quantity from the position prediction and is not counted against `max_open_orders`.
- `MaxPosition` counts same-side open orders, so a quote ladder cannot exceed `max_position` even if it fills entirely.
- Market orders skip the price checks (4, 9, 10, 18).
- The notional of checks 6 and 12 is in the instrument's settlement currency: `price * qty * multiplier` for a linear contract, `qty * multiplier / price` (the base coin) for an inverse one.
- Checks 15 and 16 compare in the reporting currency when `[accounting]` converts ([Currencies](#currencies)): the order's notional at its currency's rate, on top of the converted totals.

## Feed-lag gate

The stale-market-data check (7) measures the time since the book last changed. During venue congestion messages keep arriving, each tens of milliseconds old, so it never fires. The feed-lag gate measures the age of each message instead (`include/fastmm/core/venue_health.hpp`):

- Every market-data message with a venue time (book deltas, trades, tickers; snapshots excluded) gives a lag `recv_ts - exch_ts`. The host and venue clocks differ by an unknown constant, so the lag is compared with its baseline, the minimum over the last 8 s (eight 1 s buckets on the engine clock). A host clock step moves every lag by the step: a step down is absorbed at once, a step up after at most 8 s. A congestion episode shorter than 8 s does not raise the baseline.
- With `[risk] max_feed_lag_ms` set, a message whose lag exceeds the baseline by more than the limit engages the gate for its venue, and every such message holds it for 100 ms more. While it holds, the venue's quotes are pulled, `set_quotes` on its instruments returns false and new orders and replaces that could rest are refused (`FeedLag`, check 8). IOC, FOK and market orders pass, and so does an order that, with our open orders on its side, takes the position towards zero without crossing it. The first message 100 ms after the last late one releases it; the strategy's next `set_quotes` places the quotes again.
- The inputs are the journaled message headers and the engine clock, so a replay gates at the same events. A backtest over a live journal gates only with `[backtest] md_arrival = "recorded"`, which delivers each message at its recorded `recv_ts`.
- Venue times of the WebSocket JSON streams are in milliseconds, so the lag has 1 ms steps there; SBE streams carry microseconds.

Binance Spot BTCU (session of 2026-09-26, 3 h, SBE market data): the excess lag of each message rises from 0.2 ms to about 36 ms within 100 ms of a price burst and decays in about 300 ms; the ack latency of our orders regressed on it gives R² 0.41. A 2 ms limit held the gate for 1.4 % of the session, a 5 ms limit for 0.6 %. In backtests of that session (`configs/research/lead-mm-btcu-live-bt.toml`, `md_arrival = "recorded"`), 2 ms took the fills from 235 to 180 and 5 ms to 191, with 11 % and 2 % more orders; the 1 s markout moved from -0.08 bps to -0.05 and -0.04 bps, inside each other's 95 % intervals.

## Currencies

`Price`, `Qty` and `Notional` carry no currency. PnL, fees, `max_order_notional`, `min_notional` and `max_loss` are all denominated in the instrument's **settlement currency**: the quote currency for a linear contract, the base coin for an inverse (coin-margined) one.

Without `[accounting]`, instruments of one session must settle in the same currency; otherwise the PnL totals add unrelated numbers and `max_loss` compares the sum with one limit. `fastmm-live` checks this after the venues' reference data has loaded (`InstrumentTable::settlement_mix()`): with `max_loss` set it refuses to start, otherwise it warns.

With `[accounting]` ([Configuration](../reference/configuration.md#accounting)) they can mix. Positions, PnL and fees stay in each instrument's currency (`PositionTracker` also keeps them summed per currency); the totals, `max_loss` and the exposure caps are in the reporting currency, each other currency converted at the mid of its FX source's book (`core/fx.hpp`). The conversion runs where the totals change, on a fill, a mark or a new rate, not per order. The rules fail closed:

- A rate is unknown until its source's book is valid. While it is invalid, or older than `stale_md_ms`, an order that adds to exposure in that currency is refused (`FxRateUnknown`); one that reduces a position passes, and so does a flatten.
- PnL already booked stays converted at the last valid rate: a stale source does not hide a loss that was already measured, and `max_loss` keeps being evaluated on it. A currency whose rate was never known counts as zero; nothing can be traded in it before it is known, so only a restored or reconciled position can sit there.
- With neither `max_loss` nor an exposure cap set, the rate refuses nothing: the conversion only feeds the reports.

## Inverse contracts

An inverse contract's size is quoted in the quote currency (Deribit `BTC-PERPETUAL`: 10 USD per contract) and settles in the base coin, so its PnL is not linear in the price:

```text
realized   = qty * multiplier * (1 / avg_entry - 1 / exit)     coins
unrealized = qty * multiplier * (1 / avg_entry - 1 / mark)     coins
notional   = qty * multiplier / price                          coins
```

The average entry price of an inverse position is the size-weighted **harmonic** mean: two lots of 10 contracts at 50000 and 40000 average to 44444.44444444, not 45000. `Instrument::pnl_per_tick()` is zero for an inverse contract; its tick value depends on the price.

`Instrument::kInverse` is set from Deribit reference data, not from the configuration. An inverse contract on another connector is booked as a linear one.

## The kill switch

The kill switch is a 32-bit flag word that any thread can set: bit 0 is global, bit 1 + v is venue v. While a bit is set, checks 1 and 2 refuse every new order and replace. Tripping the global bit also turns quoting off (`on_quoting(false)`), pulls every quote and cancels every working order; tripping a venue's bit does that for that venue's instruments only. The engine records why each bit was first set (a `KillReason`, shown by `fastmm-top`).

The global switch trips when:

- the session shuts down: Ctrl-C, SIGTERM, `--duration` or an order ring overflow (the control thread requests it, then cancels all orders on every venue over a separate REST connection);
- `[risk] max_loss` is reached: net PnL (realised plus unrealised, marked at the mid, minus fees) plus the PnL carried over from earlier sessions is re-evaluated on every book update, fill, funding payment and position snapshot;
- the outbound ring to a venue or the journal ring is full, because the engine can no longer guarantee that what it sends is what it records;
- every venue with instruments has been killed;
- a strategy hook reports an error (`StrategyError`, Python hot hooks);
- the session's 32-bit client order id sequence is used up.

A venue's switch trips when its connector reports an error that makes the venue unusable (a bad key, signature or permission, failed authentication, a Binance IP ban), when the venue-side dead man's switch could not be refreshed in time, or when a multicast feed cannot rebuild its books. Behind `fastmm-gateway`, the gateway's `max_loss` or an operator's `kill` trips every venue of every attached strategy (`GatewayMaxLoss`, `GatewayOperator`). The command travels through the venue's order ring, so it is journaled and a replay trips it at the same point.

A `max_loss` trip is latched on disk (`[engine] kill_file`) together with the cumulative realized PnL and fees of every session since the file was last cleared. The next start reads that carry into the budget and refuses to trade while the trip is latched. Unrealized PnL is not carried; the position is remeasured from the venue's view after the restart.

Nothing resets a kill switch automatically. After a kill the engine tripped itself (every cause but the shutdown), `[engine] on_kill` decides whether `fastmm-live` shuts down. The default, `exit`, cancels all orders and exits with code 6. [Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md) has the behaviour, the log lines and the shutdown sequence.

## What the layer does not do

- It does not replace the strategy's own limits. `first_mm` stops quoting a side at `max_position`; `[risk] max_position` is a second, independent limit that holds even when the strategy has a bug. Set the risk limit above the strategy's.
- Position limits are per instrument. The portfolio-wide limits are `max_gross_notional`, `max_net_notional` and `max_loss`, over one session; with instruments in more than one settlement currency they need `[accounting]` ([Currencies](#currencies)). Across strategies only `fastmm-gateway`'s `[gateway]` limits see the account.
- It knows no venue rules beyond reference data and its own order rate limit (check 18). Margin and account balances are enforced by the venue. The connectors back off on the venue's rate-limit responses ([Venue connectors](../reference/venues.md)), and the engine pauses a side after venue rejects (`[engine] reject_backoff_ms`).
- It does not protect against a venue that stops answering. Order-channel loss triggers a REST cancel-all in the connector; beyond that, see [Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md).
