# Kill switch and shutdown

This page explains what stops a `fastmm-live` session from trading, what happens between Ctrl-C
and the process exiting, and what to do when the final line says `cancel_all FAILED`.

## The kill switch

The kill switch is one atomic 32-bit flag word in the risk engine
(`include/fastmm/core/risk.hpp`). Any thread can set it. While the global bit is set, the pre-trade
check refuses every new order and replace (`RejectReason::KillSwitch`); **cancels are always
allowed**. When it trips, the engine turns quoting off, pulls every quote and sends a cancel for
every working order over the venues' order channels.

| Bit | Value in the log | Meaning |
|---|---|---|
| 0 | `0x1` | Global: no new orders on any venue |
| 1 + venue id | `0x2` (venue 0), `0x4` (venue 1), ... | One venue only (`RejectReason::VenueKilled`); venues from id 30 up share bit 31 (`0x80000000`) |

Nothing in `fastmm-live` sets a per-venue bit today, so in practice you see `flags=0x1`. The
flags appear in the log line of the trip and in `fastmm-top` next to the number of trips.
`fastmm-live` has no command to reset the kill switch: restart the process.

## What trips it

| Cause | Log line | Level | What happens next |
|---|---|---|---|
| Ctrl-C (SIGINT) or SIGTERM | `fastmm-live: shutting down (signal)`, then `kill switch requested (flags=0x1); pulling quotes and cancelling all` | WARN | [Shutdown sequence](#shutdown-sequence); exit code 0 |
| `--duration` elapsed | `fastmm-live: shutting down (duration elapsed)`, then the same `kill switch requested` line | WARN | Shutdown sequence; exit code 0 |
| A venue's order-event ring overflowed | `order ring overflow on <venue>: tripping the kill switch`, then `fastmm-live: shutting down (order ring overflow)` | ERROR | Shutdown sequence; exit code 5 |
| `[risk] max_loss`: net PnL (realised plus unrealised minus fees) fell to `-max_loss` or below | `kill switch engaged (flags=0x1); pulling quotes and cancelling all` | ERROR | Quotes pulled and orders cancelled; **the process keeps running** with quoting off |
| The outbound ring to a venue was full | `outbound transport full: <n> message(s) dropped; tripping kill switch`, then `kill switch engaged` | ERROR | As for `max_loss`: the process keeps running |
| The journal ring was full | `kill switch engaged` | ERROR | As for `max_loss`: the process keeps running |

The last three leave the process running so that you can inspect it with `fastmm-top`; stop it with
Ctrl-C, which still runs the full shutdown sequence below.

These events do **not** trip the kill switch:

- **A fatal venue error** (bad key, bad signature, missing permission): the venue logs
  `fatal venue error (<code> <msg>); order entry disabled` and refuses every further order on that
  venue. The engine keeps running and cancels are still sent.
- **Market-data loss or a stale feed:** the engine pulls the quotes on the instruments of that
  venue and requotes when the book is valid again.
- **Order-channel loss:** with `cancel_on_order_channel_loss = true` (the default) the connector
  cancels everything over REST, reconnects and reconciles open orders.

## Shutdown sequence

From `run_live()` in `apps/fastmm-live/live_backend.cpp`:

1. The control thread notices the signal, the elapsed duration or the ring overflow (it checks every
   50 ms), publishes the state `stopping` to the status file and logs
   `fastmm-live: shutting down (<reason>)`.
2. It posts a kill-switch command to the engine. The engine logs `kill switch requested`, pulls
   every quote and queues cancels for every working order. If the control ring is full, the log
   says `control ring full: kill switch message dropped`; step 3 still runs.
3. **Independently of the engine**, the control thread calls `cancel_all()` on every venue, one
   after another, over a new blocking REST connection (so it works even when the venue's network
   thread is stuck). It is skipped in `--dry-run`. Each request has a 5000 ms timeout
   (`http_timeout_ms`), and there is one request per subscribed instrument:
   - Binance: `DELETE /api/v3/openOrders` per symbol; error `-2011` (nothing open) counts as success.
   - Bybit: `/v5/order/cancel-all` per symbol.
   - Deribit: `private/cancel_all_by_instrument` per instrument.
4. It waits 200 ms so that the engine's queued cancels reach the wire, then stops the engine thread.
5. Each network thread sends what is still queued, runs its reactor for up to 100 ms more and
   disconnects. The journal is flushed and closed with a trailer block.
6. The summary lines are logged: engine counters (`fastmm-live: events=... risk_rejects=<n>
   venue_rejects=<n>`), the rejects per reason for each kind that had any
   (`fastmm-live: risk_rejects by reason: MaxPosition 12, RateLimit 5`), PnL
   (`fastmm-live: realized_pnl=... unrealized_pnl=... fees=...`), one `[<venue>] final:` line per
   venue, the clock statistics and, last, `fastmm-live: shutdown took <n> ms (cancel_all ok)` or
   `(cancel_all FAILED)`.
7. The status file is marked `stopped` and left in place, so `fastmm-top` shows the final numbers.

In our Binance Demo sessions (one venue, one symbol) shutdown took 555 ms and 697 ms. The upper
bound is roughly 5 s per REST request that times out, plus 300 ms.

## Reading the last line

```text
fastmm-live: shutdown took 697 ms (cancel_all ok)
```

- `<n> ms` runs from step 1 to step 6.
- `cancel_all ok`: every venue's REST cancel-all succeeded (or was skipped in a dry run). The engine's
  own cancels were also sent, but `ok` does not prove that no order is left; check the venue.
- `cancel_all FAILED`: at least one venue's cancel-all failed. `fastmm-live` exits with code 5.

The exit codes of `fastmm-live` are 0 (ok), 2 (bad command line or missing API keys), 3 (bad
config or strategy), 4 (venue reference data failed) and 5 (runtime failure: cancel-all failed, the
journal could not be opened, ring overflow).

## When cancel_all failed

1. **Cancel by hand now.** Open the venue's website (or app), go to open orders and cancel them all.
   Know where this button is before you start a session.
2. **Find the cause** in the ERROR lines just before the shutdown line:
   - `<venue>: kill-switch cancel-all for <symbol> failed: <status> <error>` (Binance)
   - `<venue>: kill-switch cancel-all for <symbol> failed: status=<s> retCode=<code> <msg>` (Bybit)
   - `<venue>: kill-switch cancel_all_by_instrument <symbol> failed: status=<s> code=<code> <msg>`
     (Deribit)
   - `<venue>: kill-switch cancel-all failed: <error>` (the REST connection itself failed)

   Typical causes are a network outage (status 0 with an error text), a revoked key or a missing
   permission (`401`, `403`), an IP ban (`418` on Binance, `403` on Bybit) or the venue being down.
3. **Confirm zero open orders** on the venue before you start anything again. On Deribit,
   `cancel_on_disconnect = true` also makes the venue cancel the orders of the order connection
   when it closes; still check.
4. Do not rely on a restart to clean up. A new session reconciles open orders when it connects and
   cancels live orders its OMS does not know (`cancelling unknown live order <id>`), but only for
   orders the connector recognises as its own.

## Practise it

Before a session that matters, run a short keyed session with open quotes, press Ctrl-C, check
`cancel_all ok` and confirm on the venue that nothing is left ([Run on a testnet](run-on-testnet.md),
[Go-live checklist](go-live-checklist.md)).
