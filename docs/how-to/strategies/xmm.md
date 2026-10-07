# Quote on one venue, hedge on another (xmm)

`xmm` is a built-in strategy: it quotes one instrument as a maker and hedges every fill with a taker IOC on a second instrument, usually the same underlying on another venue, optionally falling back to a third. The hedging is a [`HedgeExecutor`](hedge-executor.md). Source: `include/fastmm/strategies/xmm.hpp`.

## Run it

Two configs ship:

| Config | Quotes | Hedges |
|---|---|---|
| [`configs/xmm-binance-demo.toml`](../../../configs/xmm-binance-demo.toml) | Binance Spot Demo Mode, BTCUSDT | Binance USDⓈ-M Demo Trading, BTCUSDT perpetual |
| [`configs/xmm-demo.toml`](../../../configs/xmm-demo.toml) | Binance USDⓈ-M Demo Trading, BTCUSDT perpetual | Bybit testnet, BTCUSDT linear perpetual |

1. Build: `cmake --preset release && cmake --build --preset release -j`.
2. Check the market data of both venues with a dry run. It needs no keys and sends no order:

    ```bash
    ./build/release/bin/fastmm-live --config configs/xmm-binance-demo.toml \
        --dry-run --duration 60s
    ```

    Each venue prints a status line every 10 s; both should show `md=live` and `books=1/1` (the order channels stay down in a dry run):

    ```text
    [binance] md=live user=down order=down books=1/1 md_msgs=526 resyncs=0 malformed=0 ...
    [binance_usdm] md=live user=down order=down books=1/1 md_msgs=112 resyncs=0 malformed=0 ...
    ```

3. Put the keys in the environment. Binance Demo Trading keys cover both Binance venues; enable Spot and Futures on the key, put USDT in the USDⓈ-M wallet, set it to one-way position mode, and hold BTC and USDT in the spot wallet ([Run on a testnet or Binance Demo](../operations/run-on-testnet.md)):

    ```bash
    export FASTMM_BINANCE_API_KEY=... FASTMM_BINANCE_API_SECRET=...
    ```

4. Run it for a fixed time and watch it from a second terminal:

    ```bash
    ./build/release/bin/fastmm-live --config configs/xmm-binance-demo.toml --duration 30m
    ./build/release/bin/fastmm-top --name xmm-binance-demo   # second terminal
    ```

5. Read the quote fills and the hedges from the store: `./build/release/bin/fastmm-pnl fills --engine xmm-binance-demo` ([Query what you traded](../operations/query-trading-records.md)).

Behind [`fastmm-gateway`](../operations/run-behind-a-gateway.md), `xmm` shares the account with other strategies: start the gateway with both venues, then `fastmm-live --config configs/xmm-binance-demo.toml --gateway runs/<gateway name>.gw`.

## On Binance Demo

`configs/xmm-binance-demo.toml` ran for 2 h 28 min behind `fastmm-gateway` on 2026-09-29, beside a `basic_mm` on USDⓈ-M ETHUSDT. A fault came every 20 minutes: `kill -9` of `xmm` right after a spot fill, a 20 s `SIGSTOP` of the gateway, `kill -9` of the gateway with 90 s down, and `kill -9` of the other strategy.

| | |
|---|---:|
| Spot orders / trades | 3011 / 33 |
| Hedge IOCs sent / filled | 25 / 25 |
| Largest unhedged position | 0.00209 BTC, for about 75 ms |
| Unhedged at the end | 0.0000453 BTC |

A script checked the stores against the venue's own order, trade and position records: every trade booked once, no client order id used twice, no order open at the end, positions and balances equal to the venue's. Every hedge left in the same engine step as the fill that caused it, except one that waited 74 ms for the hedge before it to end. The `xmm` process killed right after a spot fill had sent its hedge 76 ms before; the one that replaced it booked the fill and sent nothing.

## Set it up

1. Configure both venues under `[venues.*]` and both instruments under `[[instruments]]`. One session trades both (`threading = "split"`, the default).
2. Name the instruments by their position in `[[instruments]]` (from 0): `quote_instrument`, `hedge_instrument` and, for a second hedge venue, `fallback_instrument` (-1: none). They are read at start.
3. Set each venue's fees under `[venues.<name>.fees]` (or per instrument in `[[instruments]]`). The quotes price in the quote venue's maker fee (negative for a rebate) and the hedge venue's taker fee.
4. Keep `[engine] ack_timeout_ms = 0`, or well above the venues' round trip: an order the ack timeout cancels is an unknown outcome and holds hedging for `uncertain_hold_ms`.

Both instruments must be linear (spot, linear perpetuals or futures). Contract sizes may differ: positions are compared in base units, `qty * contract_multiplier`, so Binance USDⓈ-M (BTC) can be hedged with OKX swaps (0.01 BTC contracts). An inverse instrument leaves the strategy idle with an error in the log. Parameters: `fastmm-live --list-strategies`.

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

The mark, index and funding come from the venues ([`ctx.mark`, `ctx.funding`](../../reference/strategy-api.md#perpetuals)); a leg that is not a perpetual counts 0 in both terms.

- `carry`: a bid that fills is hedged by a sell, so the pair holds a short hedge and a long quote. Over the expected holding time `funding_horizon_s` the short receives the hedge's funding and the long pays the quote's, each `ctx.funding(id).over(horizon)` (the rate per interval scaled to the horizon; positive: longs pay). An ask that fills holds the opposite pair and pays the same amount, so both sides shift by the same `carry`. Example: spot quoted on Binance, hedged on a perpetual paying 0.01 % per 8 h, horizon 1 h: fair moves up by 0.00125 % of the hedge mid.
- `mark_basis`: the venues' own premia replace the EWMA of the mids, on the assumption that both indices price the same underlying. It follows the funding-driven premium without the EWMA's lag and ignores the noise of two books.

A mark, index or funding rate a term needs that is stale (`[accounting] stale_mark_ms`, `stale_funding_ms`) or has not arrived pulls the quotes, as a stale book does. Every `PerpState` of either instrument requotes while one of the two is on.

## Hedging

- `unhedged = quote position + hedge position + fallback position`, in base units.
- When `|unhedged|` rounds down to at least one lot of the hedge instrument (and its `min_qty` and `min_notional`), one IOC limit goes out, `hedge_tolerance_bps` through the touch (`fallback_tolerance_bps` on the fallback). A smaller remainder waits for the next fill: a partial maker fill under the hedge venue's minimum notional (Binance USDⓈ-M: 50 USDT) stays unhedged, within `max_unhedged`.
- While any order is open on a hedge instrument, including one sent but not acknowledged, no other hedge is sent. When it ends, the positions are read again: a partial fill is followed by a hedge for the rest.
- Hedges follow positions, not fill counts. A restart, a replayed or duplicated execution and a fill booked late by reconciliation all lead to the same hedge.
- No hedge goes out while a venue reconciles (`ctx.reconciling()`), nor after a start until every venue has replayed its executions and reconciled, so a quote fill replayed before the hedge that covered it is not hedged twice. `tests/integration/xmm_restart_test.cpp` kills `fastmm-live` (and a strategy behind `fastmm-gateway`) with the hedge out and unanswered, and with a quote filled while it is down.
- A hedge reported ended before its executions arrive (Bybit's `order` and `execution` topics are not ordered) is booked from the venue's cumulative quantity when the end arrives; the executions then correct its price and fee without adding quantity.

The rules are the executor's ([What it sends](hedge-executor.md#what-it-sends)).

## Fallback and de-risk

With `fallback_instrument` set, the hedge goes to the fallback while the hedge instrument's venue has an order channel down or its kill switch on, its book is invalid or older than `stale_ms`, the feed-lag gate holds it, its balance or margin cannot cover the hedge, or it failed `max_hedge_failures` times within `failure_window_ms` (it then sits out `failover_bench_ms`). The next hedge goes back to the hedge instrument as soon as it can take it ([Failover](hedge-executor.md#failover)). The fair value stays on the hedge instrument's book, so its market data has to stay fresh for the quotes to stay up. `tests/integration/xmm_failover_test.cpp` runs a hedge venue that refuses every order, kills `fastmm-live` while the fallback holds the hedge's reply, and restarts it.

With `derisk_after_ms` set, a residual that no hedge instrument has taken for that long is reduced on the quote instrument: reduce-only IOC orders of at most `derisk_step_qty`, `derisk_tolerance_bps` through the quote venue's touch, `derisk_interval_ms` apart ([De-risk](hedge-executor.md#de-risk)). The quotes are off meanwhile.

## Guards

| Condition | Effect |
|---|---|
| Either book invalid or older than `stale_ms` | quotes pulled |
| `mark_basis` or `funding_horizon_s` on, and a mark, index or funding rate it needs stale or missing | quotes pulled |
| No hedge instrument can take a hedge: each has an order channel down, its venue killed, its book invalid or stale, its venue gated, or is benched | quotes pulled, no hedges |
| Hedge instrument's venue held by the feed-lag gate (`[risk] max_feed_lag_ms`) | quotes pulled |
| A fill would take the unhedged position past `max_unhedged` | that side not quoted |
| The quote venue's balance cannot cover a fill of a side (`ctx.balance_room`: a spot bid's quote asset, an ask's base, a derivative's margin) | that side not quoted |
| No hedge venue's balance or margin can cover the hedge | hedge held, logged and counted once per episode (`Stats::hedges_held`), no failure; only the side that reduces `\|unhedged\|` quoted, as at `max_unhedged`; the next balance report or fill looks again |
| A hedge fills nothing (expired, rejected, refused by risk) | next hedge after `hedge_retry_ms` |
| `max_hedge_failures` such hedges within `failure_window_ms` on the last hedge instrument not sitting out | halted: quotes pulled, no hedges, error logged |
| A hedge ends without the venue saying how (ack timeout, a reconciliation with quantity unaccounted for, a generic venue reject such as a REST timeout) | next hedge after `uncertain_hold_ms` |

To clear a halt, give `restart` a new value (`fastmm-ctl param restart=1`, then 2, ...); hedging restarts from the positions. Pausing and resuming quoting does not clear it, because reconciliations do that on their own.

`[risk] max_position` applies per instrument, and `max_unhedged` bounds the net of the legs. For the net across strategies and venues, set `[risk.underlying]` or, behind a gateway, `[gateway.underlying.<BASE>] max_net`.

## Test it offline

A backtest can hold both venues, each with its own latency, replace and self-trade settings ([`[backtest.venues.<name>]`](../../reference/configuration.md#backtestvenues)), fed by one recorded feed per venue ([Backtesting](../../explanation/backtesting.md)). `tests/integration/xmm_live_test.cpp` runs `xmm` live against two simulated venues.
