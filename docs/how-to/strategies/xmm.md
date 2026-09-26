# Quote on one venue, hedge on another (xmm)

`xmm` is a built-in strategy: it quotes one instrument as a maker and hedges every fill with a taker IOC on a second instrument, usually the same underlying on another venue. Source: `include/fastmm/strategies/xmm.hpp`. Example: [`configs/xmm-demo.toml`](../../../configs/xmm-demo.toml) (Binance USDⓈ-M demo quotes, Bybit testnet hedges).

## Set it up

1. Configure both venues under `[venues.*]` and both instruments under `[[instruments]]`. One session trades both (`threading = "split"`, the default).
2. Name the two instruments by their position in `[[instruments]]` (from 0): `quote_instrument` and `hedge_instrument`. They are read at start.
3. Put each venue's fees into the parameters: `quote_fee_bps` (maker, negative for a rebate) and `hedge_fee_bps` (taker). The strategy does not read `[venues.*.fees]`.
4. Keep `[engine] ack_timeout_ms = 0`, or well above the venues' round trip: an order the ack timeout cancels is an unknown outcome and holds hedging for `uncertain_hold_ms`.

Both instruments must be linear (spot, linear perpetuals or futures). Contract sizes may differ: positions are compared in base units, `qty * contract_multiplier`, so Binance USDⓈ-M (BTC) can be hedged with OKX swaps (0.01 BTC contracts). An inverse instrument leaves the strategy idle with an error in the log.

## Pricing

```text
ref   = hedge mid (use_microprice: the touch microprice)
basis = EWMA of (quote mid - ref), half-life basis_halflife_s; 0 turns it off
fair  = ref + basis
half  = fair * (edge_bps + quote_fee_bps + hedge_fee_bps + slippage_bps)
bid   = fair - half, rounded down; ask = fair + half, rounded up; one level of quote_qty (base units)
```

Quotes never cross the quote venue's touch. A fair value move under `requote_threshold_ticks` does not requote.

## Hedging

- `unhedged = quote position + hedge position`, in base units.
- When `|unhedged|` rounds down to at least one hedge lot (and the hedge instrument's `min_qty`), one IOC limit goes out on the hedge instrument, `hedge_tolerance_bps` through the touch.
- While any order is open on the hedge instrument, including one sent but not acknowledged, no other hedge is sent. When it ends, the positions are read again: a partial fill is followed by a hedge for the rest.
- Fills are never counted. A restart, a replayed or duplicated execution and a fill booked late by reconciliation all lead to the same hedge.

## Guards

| Condition | Effect |
|---|---|
| Either book invalid or older than `stale_ms` | quotes pulled |
| Hedge venue market data or order channel down | quotes pulled, no hedges |
| A fill would take `\|unhedged\|` past `max_unhedged` | that side not quoted |
| A hedge fills nothing (expired, rejected, refused by risk) | next hedge after `hedge_retry_ms` |
| `max_hedge_failures` such hedges within `failure_window_ms` | halted: quotes pulled, no hedges, error logged |
| A hedge ends without the venue saying how (ack timeout, reconciliation with quantity unaccounted for, a generic venue reject such as a REST timeout) | next hedge after `uncertain_hold_ms` |

To clear a halt, give `restart` a new value (`fastmm-ctl param restart=1`, then 2, ...); hedging restarts from the positions. Pausing and resuming quoting does not clear it, because reconciliations do that on their own.

## Limits

- The backtester runs one venue per run, so `xmm` is tested with unit tests, the strategy harness and a live session against two simulated venues (`tests/integration/xmm_live_test.cpp`), not in a backtest.
- `[risk] max_position` applies per instrument; nothing limits the net position across the two venues except `max_unhedged`.
- Parameters: `fastmm-live --list-strategies`.
