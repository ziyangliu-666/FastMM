# Risk model

Every new order and every replace passes a fixed list of checks before it leaves the engine; the first one that fails refuses the order. The limits are the `[risk]` section of the [Configuration](../reference/configuration.md#risk).

## Where the checks sit

The checks run on the engine thread, after the strategy decides and before the OMS records the order. Quotes from the quote manager and orders from `ctx.send` take the same path.

A refused order is never sent. The strategy sees the `RejectReason` in the `Result` of `ctx.send`, or a quote that does not appear. The engine counts rejects per reason in `fastmm-top` and the shutdown summary, and logs at most one line per reason every 10 s.

Cancels skip the checks, also after the kill switch has tripped, so a strategy or the engine can always reduce what is in the market.

The checks are integer compares on state the engine already keeps, and an order that passes all of them takes 7.1 ns ([Benchmarks](benchmarks.md)).

## The checks, in order

The first failing check decides the reason.

| # | Reason | Refuses an order when | Setting |
|---:|---|---|---|
| 1 | `KillSwitch` | the global kill switch is set, unless the order is a `flatten`'s | |
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
| 14 | `FxRateUnknown` | the order opens exposure in a settlement currency whose rate is unknown or stale, while a limit reads the totals | `[accounting]` |
| 15 | `MaxGrossNotional` | the portfolio's summed \|position\| at the last marks plus what this order opens would pass the cap | `max_gross_notional` |
| 16 | `MaxNetNotional` | the portfolio's signed position sum would move further past the cap, and this order opens exposure | `max_net_notional` |
| 17 | `UnderlyingMarkUnknown` | an inverse contract of the order's underlying has a position or open orders, or is the order's instrument, and no current mark | `[risk.underlying]` |
| 18 | `MaxUnderlyingNet` | the net position in the order's underlying, with the open orders on the order's side and this order, would move further past the limit | `[risk.underlying.<BASE>] max_net` |
| 19 | `MaxOpenOrders` | the instrument already has this many open orders (new orders only) | `max_open_orders` |
| 20 | `SelfTradePrevention` | a limit price would trade against one of our own resting orders | `stp` |
| 21 | `BalanceShort` | the account's balance on the venue does not cover the order ([Balance check](#balance-check)) | `check_balance` |
| 22 | `VenueKilled` | a pool member's order and the pool's primary venue is killed ([account pools](../reference/configuration.md#account-pools)) | |
| 23 | `RateLimit` | a pool order's account cannot send it now: its connector is paused or an order window is at its cap (`ctx.order_budget`) | |
| 24 | `RateLimit` | the token bucket is empty | `orders_per_sec`, `burst` |

A limit of 0 turns its check off, and every limit defaults to off. The checks against the instrument's reference data always run. `fastmm-live` with real keys refuses to start without `max_position` or `max_order_notional`, and `max_loss`, unless `[risk] allow_unlimited = true`.

- Market orders skip the price checks 4, 9, 10 and 20.
- A replace leaves the existing order's remaining quantity out of the position prediction and does not count against `max_open_orders`.
- `MaxPosition` counts same-side open orders, so a quote ladder cannot pass `max_position` even if it fills entirely.
- Only an order that passes every check takes a token from the bucket. An order refused for another reason, a full or paused pool account included, leaves the tokens to the orders that are sent.
- The notional of checks 6 and 12 is in the instrument's settlement currency: `price * qty * multiplier` for a linear contract, `qty * multiplier / price` for an inverse one.
- Checks 14 to 16 pass the part of an order that takes its position towards zero, after our open orders on that side. The rest opens exposure and is checked as a new order.
- Long 1 and selling 3, the 2 past zero meet the gross cap on their own notional, and the net cap on the net the whole order leaves. An order that only closes passes, even where it moves a hedged net further from zero.
- Checks 15 and 16 compare in the reporting currency when `[accounting]` converts ([Currencies](#currencies)).

## Feed-lag gate

The feed-lag gate stops resting orders while a venue's market data arrives late. Check 7 cannot catch this: during congestion messages keep arriving, each tens of milliseconds old, so the book never looks stale.

The gate measures each message's lag, its receive time minus its venue time, for book deltas, trades and tickers. Host and venue clocks differ by an unknown constant, so the lag is compared with a baseline: the minimum lag over the last 8 s. A congestion episode shorter than 8 s does not raise the baseline.

With `[risk] max_feed_lag_ms` set, a message whose lag exceeds the baseline by more than the limit engages the gate for its venue, and every such message holds it 100 ms more. While it holds:

- the venue's quotes are pulled and `set_quotes` on its instruments returns false;
- new orders and replaces that could rest are refused with `FeedLag`;
- IOC, FOK and market orders pass, and so does an order that takes the position towards zero without crossing it.

The first message 100 ms after the last late one releases the gate, and the strategy's next `set_quotes` places the quotes again.

| Case | Behaviour |
|---|---|
| Host clock steps down | absorbed at once |
| Host clock steps up | absorbed after at most 8 s |
| WebSocket JSON streams | venue times in milliseconds, so the lag moves in 1 ms steps |
| SBE streams | venue times in microseconds |
| Replay | gates at the same events, from the journaled headers and engine clock |
| Backtest over a live journal | gates only with `[backtest] md_arrival = "recorded"` |

## Balance check

Check 21 refuses an order that the account's free balance on its venue does not cover. The engine keeps one balance row per venue and asset: the base and quote of each spot instrument, the settlement asset of each derivative, and an account row for venues that report account-wide margin.

Connectors report balances as snapshots after every reconciliation and at start-up, plus the private stream's updates where the venue has one ([Venues](../reference/venues/connector-behaviour.md#balances)). In a backtest the simulated venue sends them ([Backtesting](backtesting.md#balances)). The reports are journaled, so a replay rebuilds the same rows.

Between reports the engine moves each row with its own order events:

| Event | Spot buy | Spot sell | Derivative |
|---|---|---|---|
| Order rests | holds notional plus taker fee, in quote | holds quantity, in base | holds notional × `[[instruments]] initial_margin` of the instrument's larger side, in its margin row; reduce-only holds nothing |
| Fill | adds base less any base fee, takes notional and any quote fee from quote | the reverse | moves the position's initial margin at the fill price into locked, takes the fee in the settlement asset |
| Order ends | releases its hold | releases its hold | releases its hold |

A fill releases the filled part of the hold and an amend changes it.

A report counts the orders the venue had acknowledged by the report's venue time. The engine skips its own events the venue stamped at or before that time, and keeps the hold of an order not yet acknowledged. Connectors stamp acks with a venue time from the order response, so a report arriving before the ack does not hold the order twice.

A report can also arrive after an event the venue stamped later. Binance's WebSocket API answers an order 1 to 5 ms before the user stream pushes the account. The engine reapplies such events on top of every older report, so a new order keeps its hold and a cancelled order is not held again.

Two cases can double-count until the next report: an event with no venue time that reaches the engine after a report that already had it, and the hold of an order whose ack carries no time. Coinbase Advanced Trade's responses carry no time, so its acks stay unstamped.

The check itself:

- An order is refused when its hold, less what the order it replaces holds, exceeds the row's free estimate.
- A derivative order counts what it adds to its instrument's larger side, and is checked only while available margin is zero or more.
- A derivative order that is reduce-only or reduces the position passes, as does one without `initial_margin` while available margin is positive.
- A row the venue has not reported refuses nothing, so a venue without balance reports is not checked.
- `[risk] check_balance = false` keeps the rows and turns the check off.

On a venue without replace (`[engine] supports_replace = false`), a requote cancels the resting order and sends the new one after the cancel ack releases the old hold. The two never rest together. If the old order filled instead, the new one is checked against the balance after the fill, and one the balance no longer covers is withheld rather than refused.

`fastmm-gateway` checks the account's balance over every attached strategy's orders as well ([Account guards](../how-to/operations/run-behind-a-gateway.md#account-guards)).

## Currencies

PnL, fees, `max_order_notional`, `min_notional` and `max_loss` are in the instrument's **settlement currency**: the quote currency for a linear contract, the base coin for an inverse one. Prices, quantities and notionals carry no currency of their own.

Without `[accounting]`, the instruments of one session must settle in the same currency. Deribit's `BTC-PERPETUAL` and BTC options both settle in BTC, so they mix. With mixed currencies and `max_loss` set, `fastmm-live` refuses to start once reference data has loaded; without `max_loss` it warns.

With `[accounting]` ([Configuration](../reference/configuration.md#accounting)) currencies can mix. Positions, PnL and fees stay in each instrument's currency. The totals, `max_loss` and the exposure caps are in the reporting currency, converted at the mid of each currency's FX source book whenever a fill, mark or rate changes them.

The conversion fails closed:

- A rate is unknown until its source book is valid. While it is invalid or older than `[accounting] stale_fx_ms` (60 s by default), orders that open exposure in that currency are refused with `FxRateUnknown`. Orders towards zero and flattens pass.
- PnL already booked stays converted at the last valid rate, so a stale source does not hide a loss and `max_loss` keeps reading it.
- A currency whose rate was never known counts as zero. Nothing can trade in it before then, so only a restored or reconciled position can sit there.
- With neither `max_loss` nor an exposure cap set, the rate refuses nothing and only feeds the reports.

## Net position per underlying

`[risk.underlying.<BASE>] max_net` limits the net position in one base asset across every instrument with that `base`, on every venue ([Configuration](../reference/configuration.md#riskunderlying)). Long 0.3 BTC on Binance USD-M and short 0.3 BTC on Bybit is 0 BTC. The unit is the base asset, so the limit needs no price and no `[accounting]`.

| Instrument | Counts as | Example |
|---|---|---|
| Spot or linear contract | `qty * contract_multiplier` | 30 OKX `BTC-USDT-SWAP` of 0.01 BTC = 0.3 BTC |
| Inverse contract | `qty * multiplier / mark` | 150 contracts of 100 USD = 0.3 BTC at 50,000 USD, 0.375 BTC at 40,000 USD |
| Option | not counted, not checked | its exposure is a delta, which needs a pricing model |

The check is the underlying's version of `max_position`. It adds the position now, the open orders on the order's side across the underlying's instruments, and the order. The order is refused when that would pass `max_net` and be further from zero than now; an order towards zero always passes, even over the limit.

The mark is the instrument's last valid mid, no older than `stale_md_ms` when that is set. An inverse contract with a position or open orders and no such mark refuses every order on its underlying (`UnderlyingMarkUnknown`) until the book returns. A flatten is not checked.

The check does not count balances or margin held in the base coin, or positions outside the session. A `[risk.underlying]` section for a base that only options have is a configuration error. Behind `fastmm-gateway`, `[gateway.underlying]` holds the account's net over every strategy ([Run behind a gateway](../how-to/operations/run-behind-a-gateway.md#account-guards)).

`fastmm-ctl limits underlying.BTC.max_net=...` changes a limit at run time, for an underlying the configuration names; `max_net = 0` names one without a limit.

## Inverse contracts

An inverse contract's size is in the quote currency and it settles in the base coin, so its PnL is not linear in the price. Deribit `BTC-PERPETUAL` is 10 USD per contract.

```text
realized   = qty * multiplier * (1 / avg_entry - 1 / exit)     coins
unrealized = qty * multiplier * (1 / avg_entry - 1 / mark)     coins
notional   = qty * multiplier / price                          coins
```

The average entry price is the size-weighted harmonic mean: 10 contracts at 50,000 and 10 at 40,000 average to 44,444.44, not 45,000. The tick value depends on the price, so `pnl_per_tick()` is zero for an inverse contract.

The inverse flag comes from Deribit reference data, not from the configuration. An inverse contract on another connector is booked as linear.

## Valuation at the venue mark

With `[accounting] mark = "venue"`, the default, a derivative position is valued at the venue's published mark price while the mark is younger than `stale_mark_ms` (15 s by default). Liquidations and the venue's own unrealised PnL use the same mark ([Venues](../reference/venues/connector-behaviour.md#mark-index-and-funding)).

The mark sets the unrealised PnL, the net PnL `max_loss` measures and the exposure the notional caps sum. A new mark revalues the position and checks `max_loss` at once; book updates do not move it. When the mark goes stale, the next book update values the position at the mid, logged and counted, until a fresh mark arrives.

Spot and other instruments without marks are valued at the mid. `mark = "mid"` values every position at its mid.

The price collar, the fat-finger check, `[risk.underlying]` and the FX rates use the mid in either mode. The decision runs on the engine clock, which the journal records, so a replay values and trips at the same events. `fastmm-gateway` follows the same setting, so its `max_loss` and caps see the strategies' values.

## The kill switch

The kill switch has one global bit and one bit per venue, and any thread can set them. While a bit is set, checks 1 and 2 refuse every new order and replace. The global bit lets through the reduce-only orders of an operator's `flatten`; a venue bit refuses them too.

Tripping the global bit turns quoting off (`on_quoting(false)`), pulls every quote and cancels every working order. Tripping a venue bit does the same for that venue's instruments. `fastmm-top` shows the reason each bit was first set.

| Switch | Trips when |
|---|---|
| Global | the session shuts down: Ctrl-C, SIGTERM, `--duration` or an order ring overflow; all orders are then cancelled over a separate REST connection |
| Global | `[risk] max_loss` is reached |
| Global | an outbound ring to a venue or the journal ring is full, so what is sent may no longer match what is recorded |
| Global | every venue with instruments has been killed |
| Global | a strategy hook reports an error (`StrategyError`, Python hot hooks) |
| Global | the session's 32-bit client order id sequence is used up |
| Venue | its connector reports an error that makes the venue unusable: bad key, signature or permission, failed authentication, a Binance IP ban |
| Venue | the venue-side dead man's switch could not be refreshed in time |
| Venue | a multicast feed cannot rebuild its books |
| Every venue | behind `fastmm-gateway`, the gateway's `max_loss` or an operator's `kill` (`GatewayMaxLoss`, `GatewayOperator`) |

`max_loss` compares net PnL with the limit on every book update, venue mark, fill, funding payment and position snapshot. Net PnL is realised plus unrealised, valued at the venue mark or the mid ([Valuation](#valuation-at-the-venue-mark)), minus fees, plus the PnL carried from earlier sessions.

A `max_loss` trip is latched on disk in `[engine] kill_file`, with the cumulative realised PnL and fees of every session since the file was last cleared. The next start reads that carry into the budget and refuses to trade while the trip is latched. Unrealised PnL is not carried; the position is remeasured from the venue after the restart.

Gateway kill commands travel through the venue's order ring, so they are journaled and a replay trips at the same point.

Nothing resets a kill switch automatically. After a kill the engine tripped itself, every cause but shutdown, `[engine] on_kill` decides whether `fastmm-live` exits. The default, `exit`, cancels all orders and exits with code 6. [Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md) has the log lines and the shutdown sequence.

## What the layer does not do

- It does not replace the strategy's own limits. `first_mm` stops quoting a side at `max_position`; `[risk] max_position` is a second limit that holds when the strategy has a bug. Set the risk limit above the strategy's.
- Position limits are per instrument, and per base asset with `[risk.underlying]`. The portfolio limits `max_gross_notional`, `max_net_notional` and `max_loss` cover one session, and need `[accounting]` across settlement currencies. Across strategies only `fastmm-gateway`'s `[gateway]` limits see the account.
- It knows no venue rules beyond reference data, the account's balances (check 21) and its own order rate limit (check 24). Connectors back off on the venue's rate-limit responses ([Venue connectors](../reference/venues.md)), and the engine pauses a side after venue rejects and after price-collar or fat-finger refusals of its quotes (`[engine] reject_backoff_ms`).
- Cancels never take a token. A `set_quotes` short of a token is not refused: it waits, and the engine places the latest waiting quotes as the bucket refills. `[engine] quote_token_reserve` keeps tokens for the strategy's own orders and for quotes that reduce the position.
- It does not protect against a venue that stops answering. Loss of the order channel triggers a REST cancel-all in the connector; beyond that, see [Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md).
- It does not share an account. Unknown open orders on its symbols are cancelled at start-up and after a reconnect (`[engine] cancel_unknown`), and cancel-alls take every order on the symbol.
