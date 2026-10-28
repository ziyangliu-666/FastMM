# Quote on one venue, hedge on another (xmm)

`xmm` quotes one instrument as a maker and hedges each fill with a taker IOC on a second instrument, usually the same underlying on another venue. A third instrument can take the hedge when the second cannot.

The hedging is a [`HedgeExecutor`](hedge-executor.md); `xmm` adds the pricing and the guards.

![xmm quotes on the quote venue, prices from the hedge venue's book and hedges each fill with an IOC on the hedge venue, or on the fallback venue while the hedge venue is unusable](../../assets/diagrams/xmm.svg#only-light){ .fastmm-diagram }
![xmm quotes on the quote venue, prices from the hedge venue's book and hedges each fill with an IOC on the hedge venue, or on the fallback venue while the hedge venue is unusable](../../assets/diagrams/xmm-dark.svg#only-dark){ .fastmm-diagram }

## Run it

Prerequisites: a release build (`cmake --preset release && cmake --build --preset release -j`) or the [release tarball](../operations/deploy.md). `xmm` runs in `fastmm-live`; `fastmm run` from the pip package runs Python classes only.

| Config | Quotes | Hedges |
|---|---|---|
| [`configs/xmm-binance-demo.toml`](../../../configs/xmm-binance-demo.toml) | Binance Spot Demo Mode, BTCUSDT | Binance USDⓈ-M Demo Trading, BTCUSDT perpetual |
| [`configs/xmm-demo.toml`](../../../configs/xmm-demo.toml) | Binance USDⓈ-M Demo Trading, BTCUSDT perpetual | Bybit testnet, BTCUSDT linear perpetual |

1. Check both venues' market data with a dry run. It needs no keys and sends no order:

    ```bash
    ./build/release/bin/fastmm-live --config configs/xmm-binance-demo.toml \
        --dry-run --duration 60s
    ```

    Each venue prints a status line every 10 s. Both should show `md=live` and `books=1/1`; the order channels stay down in a dry run:

    ```text
    [binance] md=live user=down order=down books=1/1 md_msgs=526 resyncs=0 malformed=0 ...
    [binance_usdm] md=live user=down order=down books=1/1 md_msgs=112 resyncs=0 malformed=0 ...
    ```

2. Prepare the Binance Demo account ([Run on a testnet or Binance Demo](../operations/run-on-testnet.md)). One Demo Trading key covers both Binance venues.

    | Where | What |
    |---|---|
    | API key | Spot and Futures enabled |
    | USDⓈ-M wallet | USDT, one-way position mode |
    | Spot wallet | BTC and USDT |

3. Put the keys in the environment:

    ```bash
    export FASTMM_BINANCE_API_KEY=... FASTMM_BINANCE_API_SECRET=...
    ```

4. Run it for a fixed time and watch it from a second terminal:

    ```bash
    ./build/release/bin/fastmm-live --config configs/xmm-binance-demo.toml --duration 30m
    ./build/release/bin/fastmm-top --name xmm-binance-demo   # second terminal
    ```

5. Read the quote fills and the hedges from the store ([Query what you traded](../operations/query-trading-records.md)):

    ```bash
    ./build/release/bin/fastmm-pnl fills --engine xmm-binance-demo
    ```

To share the account with other strategies, run it behind [`fastmm-gateway`](../operations/run-behind-a-gateway.md). Start the gateway with both venues, then run `fastmm-live --config configs/xmm-binance-demo.toml --gateway runs/<gateway name>.gw`.

## On Binance Demo

`configs/xmm-binance-demo.toml` ran for 2 h 28 min behind `fastmm-gateway`, with one fault every 20 minutes: `kill -9` of `xmm` after a spot fill, a 20 s `SIGSTOP` of the gateway, `kill -9` of the gateway, `kill -9` of a second strategy.

| Result | |
|---|---:|
| Spot orders / trades | 3011 / 33 |
| Hedge IOCs sent / filled | 25 / 25 |
| Largest unhedged position | 0.00209 BTC, for about 75 ms |
| Unhedged at the end | 0.0000453 BTC |
| Trades booked twice, client order ids reused, orders left open | 0 |

Positions and balances matched the venue's records. The `xmm` process that replaced the killed one booked the fill and sent no second hedge.

## Set it up

1. Configure both venues under `[venues.*]` and both instruments under `[[instruments]]`. One session trades both (`threading = "split"`, the default).
2. Name the instruments by their position in `[[instruments]]`, counting from 0. They are read at start.

    | Parameter | Instrument |
    |---|---|
    | `quote_instrument` | quoted as a maker |
    | `hedge_instrument` | takes the hedges, and gives the fair value |
    | `fallback_instrument` | takes the hedges when the hedge instrument cannot; -1 for none |

3. Set each venue's fees under `[venues.<name>.fees]`, or per instrument in `[[instruments]]`. The quotes price in the quote venue's maker fee, negative for a rebate, and the hedge venue's taker fee.
4. Keep `[engine] ack_timeout_ms = 0`, or well above the venues' round trip. An order the ack timeout cancels is an unknown outcome and holds hedging for `uncertain_hold_ms`.

Both instruments must be linear: spot, linear perpetuals or futures. An inverse instrument leaves the strategy idle with an error in the log. Contract sizes may differ, because positions are compared in base units (`qty * contract_multiplier`): Binance USDⓈ-M in BTC can be hedged with OKX swaps of 0.01 BTC. `fastmm-live --list-strategies` lists the parameters.

## Pricing

```text
ref   = hedge mid (use_microprice: the touch microprice)
basis = EWMA of (quote mid - ref), half-life basis_halflife_s; 0 turns it off
        mark_basis: (quote mark - quote index) - (hedge mark - hedge index)
carry = ref * (hedge funding - quote funding) over funding_horizon_s; 0 turns it off
fair  = ref + basis + carry
half  = fair * (edge_bps + quote maker fee + hedge taker fee + slippage_bps)
bid   = fair - half, rounded down; ask = fair + half, rounded up
size  = one level of quote_qty, base units
```

Quotes never cross the quote venue's touch. A fair value move under `requote_threshold_ticks` does not requote.

### Perpetual legs

The mark, index and funding come from the venues ([`ctx.mark`, `ctx.funding`](../../reference/strategy-api.md#perpetuals)). A leg that is not a perpetual counts 0 in both terms.

`carry` prices the funding the hedged pair earns or pays. A filled bid leaves a long quote and a short hedge. Over `funding_horizon_s` the short receives the hedge's funding and the long pays the quote's, each `ctx.funding(id).over(horizon)`; positive means longs pay. A filled ask holds the opposite pair and pays the same amount, so both sides shift by the same `carry`.

For example, with spot quoted on Binance and hedged on a perpetual paying 0.01 % per 8 h, a 1 h horizon moves fair up by 0.00125 % of the hedge mid.

`mark_basis` replaces the EWMA of the mids with the venues' own premia, assuming both indices price the same underlying. It follows the funding-driven premium without the EWMA's lag and ignores the noise of two books.

A mark, index or funding rate that a term needs and that is stale or missing pulls the quotes, as a stale book does. The limits are `[accounting] stale_mark_ms` and `stale_funding_ms`. Each `PerpState` of either instrument requotes while one of the two terms is on.

## Hedging

The hedge rules are the executor's ([What it sends](hedge-executor.md#what-it-sends)). In `xmm` terms:

| Rule | |
|---|---|
| exposure | `unhedged = quote position + hedge position + fallback position`, base units |
| when a hedge goes out | `\|unhedged\|` rounds down to at least one lot, `min_qty` and `min_notional` of the hedge instrument |
| hedge order | one IOC limit, `hedge_tolerance_bps` through the touch; `fallback_tolerance_bps` on the fallback |
| smaller remainder | waits for the next fill, within `max_unhedged` |
| in flight | one hedge at a time, including one sent and not yet acknowledged; a partial fill is followed by a hedge for the rest |
| sizing | from positions, not fill counts: a restart, a replayed or duplicated execution, or a late fill lead to the same hedge |
| reconciling | no hedge while a venue reconciles, nor after a start until all venues have replayed their executions |

A partial maker fill under the hedge venue's minimum notional stays unhedged; on Binance USDⓈ-M that minimum is 50 USDT. Waiting for reconciliation after a start keeps a quote fill replayed before its hedge from being hedged twice.

On Bybit, a hedge can be reported ended before its executions arrive, because the `order` and `execution` topics are not ordered. It is booked from the venue's cumulative quantity; the executions then correct its price and fee without adding quantity.

## Fallback and de-risk

With `fallback_instrument` set, the hedge goes to the fallback while the hedge instrument is unusable ([Failover](hedge-executor.md#failover)). It goes back as soon as the hedge instrument can take it.

| Hedge instrument is unusable when | Parameter |
|---|---|
| its venue has an order channel down or its kill switch on | |
| its book is invalid or older than `stale_ms` | `stale_ms` |
| the feed-lag gate holds its venue | `[risk] max_feed_lag_ms` |
| its balance or margin cannot cover the hedge | |
| it failed `max_hedge_failures` times within `failure_window_ms` | sits out `failover_bench_ms` |

The fair value stays on the hedge instrument's book, so its market data must stay fresh for the quotes to stay up.

With `derisk_after_ms` set, a residual that no hedge instrument has taken for that long is reduced on the quote instrument ([De-risk](hedge-executor.md#de-risk)). The orders are reduce-only IOCs of at most `derisk_step_qty`, `derisk_tolerance_bps` through the quote venue's touch, `derisk_interval_ms` apart. The quotes are off meanwhile.

## Guards

| Condition | Effect |
|---|---|
| Either book invalid or older than `stale_ms` | quotes pulled |
| `mark_basis` or `funding_horizon_s` on, and a mark, index or funding rate it needs stale or missing | quotes pulled |
| No hedge instrument can take a hedge: each has an order channel down, its venue killed, its book invalid or stale, its venue gated, or is benched | quotes pulled, no hedges |
| Hedge instrument's venue held by the feed-lag gate (`[risk] max_feed_lag_ms`) | quotes pulled |
| A fill would take the unhedged position past `max_unhedged` | that side not quoted |
| The quote venue's balance cannot cover a fill of a side (`ctx.balance_room`: a spot bid's quote asset, an ask's base, a derivative's margin) | that side not quoted |
| No hedge venue's balance or margin can cover the hedge | hedge held and logged once per episode (`Stats::hedges_held`), not a failure; only the side that reduces `\|unhedged\|` quoted; the next balance report or fill looks again |
| A hedge fills nothing (expired, rejected, refused by risk) | next hedge after `hedge_retry_ms` |
| `max_hedge_failures` such hedges within `failure_window_ms` on the last hedge instrument not sitting out | halted: quotes pulled, no hedges, error logged |
| A hedge ends without the venue saying how (ack timeout, a reconciliation with quantity unaccounted for, a generic venue reject such as a REST timeout) | next hedge after `uncertain_hold_ms` |

To clear a halt, give `restart` a new value; hedging restarts from the positions. Pausing and resuming quoting does not clear it, because reconciliations pause and resume on their own.

```bash
./build/release/bin/fastmm-ctl --name xmm-binance-demo param restart=1   # 2, 3, ... for later halts
```

`[risk] max_position` applies per instrument, and `max_unhedged` bounds the net of the legs. For the net across strategies and venues, set `[risk.underlying]` or, behind a gateway, `[gateway.underlying.<BASE>] max_net`.

## Test it offline

A backtest can hold both venues, each with its own latency, replace and self-trade settings ([`[backtest.venues.<name>]`](../../reference/configuration.md#backtestvenues)), fed by one recorded feed per venue ([Backtesting](../../explanation/backtesting.md)).

| Test | Covers |
|---|---|
| [`tests/integration/xmm_live_test.cpp`](../../../tests/integration/xmm_live_test.cpp) | `xmm` live against two simulated venues |
| [`tests/integration/xmm_restart_test.cpp`](../../../tests/integration/xmm_restart_test.cpp) | `fastmm-live`, and a strategy behind `fastmm-gateway`, killed with the hedge out, and with a quote filled while down |
| [`tests/integration/xmm_failover_test.cpp`](../../../tests/integration/xmm_failover_test.cpp) | a hedge venue that refuses all orders; `fastmm-live` killed while the fallback holds the hedge's reply, then restarted |
