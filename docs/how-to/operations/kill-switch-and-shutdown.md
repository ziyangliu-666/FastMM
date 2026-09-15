# Kill switch and shutdown

## The kill switch

The kill switch is one atomic 32-bit flag word in the risk engine (`include/fastmm/core/risk.hpp`). Any thread can set it. While the global bit is set, the pre-trade check refuses every new order and replace (`RejectReason::KillSwitch`); cancels are always allowed. When it trips, the engine turns quoting off, pulls every quote and sends a cancel for every working order over the venues' order channels.

| Bit | Value in the log | Meaning |
|---|---|---|
| 0 | `0x1` | Global: no new orders on any venue |
| 1 + venue id | `0x2` (venue 0), `0x4` (venue 1), ... | One venue only (`RejectReason::VenueKilled`); venues from id 30 up share bit 31 (`0x80000000`) |

Venue ids follow the order of the `[venues.<name>]` tables in the config, starting at 0. The engine keeps the first reason each bit was set for (`KillReason`: `Requested`, `MaxLoss`, `TransportFull`, `JournalOverflow`, `AllVenuesKilled`, `VenueFatal`, `VenueHardStop`, `OrderRingOverflow`, `StrategyError`); `fastmm-top` shows the flags and reasons ([Monitoring a live session](monitor-with-fastmm-top.md)). `fastmm-live` has no command to reset the kill switch: restart the process.

## What trips it

| Cause | Scope | Log line | Level | What happens next |
|---|---|---|---|---|
| Ctrl-C (SIGINT) or SIGTERM | Global, requested | `fastmm-live: shutting down (signal)`, then `kill switch requested (flags=0x1); pulling quotes and cancelling all` | WARN | [Shutdown sequence](#shutdown-sequence); exit code 0 |
| `--duration` elapsed | Global, requested | `fastmm-live: shutting down (duration elapsed)`, then the same `kill switch requested` line | WARN | Shutdown sequence; exit code 0 |
| A venue's order-event ring overflowed | Global, requested | `order ring overflow on <venue>: tripping the kill switch`, then `fastmm-live: shutting down (order ring overflow)` | ERROR | Shutdown sequence; exit code 5 |
| `[risk] max_loss`: net PnL (realised plus unrealised minus fees) fell to `-max_loss` or below | Global | `kill switch engaged (MaxLoss, flags=0x1); pulling quotes and cancelling all` | ERROR | [`on_kill`](#after-a-kill-the-engine-trips-itself) |
| The outbound ring to a venue was full | Global | `outbound transport full: <n> message(s) dropped; tripping kill switch`, then `kill switch engaged (TransportFull, ...)` | ERROR | `on_kill` |
| The journal ring was full | Global | `kill switch engaged (JournalOverflow, ...)` | ERROR | `on_kill` |
| A fatal venue error (see [below](#venue-kill-switch)) | That venue | `<venue>: asking the engine to kill this venue (VenueFatal)`, `venue <id> kill switch engaged (VenueFatal, flags=0x2); ...`, `[<venue>] venue kill switch engaged (VenueFatal): ...; <n> of <m> venue(s) still trading` | ERROR | Only that venue stops; the others keep trading |
| Every venue that has instruments is killed | Global | `kill switch engaged (AllVenuesKilled, ...)` | ERROR | `on_kill` |

The exit codes in the table assume `cancel_all ok`; a failed cancel-all makes the code 5 ([Reading the last lines](#reading-the-last-lines)).

These events do not trip the kill switch:

- Market-data loss or a stale feed: the engine pulls the quotes on the instruments of that venue and requotes when the book is valid again.
- Order-channel loss: with `cancel_on_order_channel_loss = true` (the default) the connector cancels everything over REST, reconnects with backoff (without giving up) and reconciles open orders. A connection that keeps failing its authentication ends in a fatal venue error, below.
- A venue rejecting one order (filters, balance, rate limit): counted per reason ([Troubleshooting](troubleshooting.md#orders-and-reconciliation)).

## After a kill the engine trips itself

`[engine] on_kill` decides what `fastmm-live` does after a global kill it did not request: every row above except signal, `--duration` and order ring overflow, which shut down in any case.

| `on_kill` | Behaviour |
|---|---|
| `"exit"` (default) | Within 50 ms the control thread logs `fastmm-live: shutting down (kill switch: <reason>; [engine] on_kill = "exit")` at ERROR and runs the [shutdown sequence](#shutdown-sequence). Exit code 6, or 5 if a cancel-all failed |
| `"stay"` | The process keeps running with quoting off and new orders refused. At once and then every 10 s it logs `fastmm-live: kill switch engaged (<reason>, flags=<hex>) and [engine] on_kill = "stay": quoting is off and no new orders are sent; stop the process (SIGINT/SIGTERM) to cancel all and exit` at ERROR. SIGINT or SIGTERM then runs the shutdown sequence: exit code 0, or 5 if a cancel-all failed |

Use `"stay"` only when someone watches the session. Alert on exit codes 5 and 6. After a `max_loss` kill, check the PnL and the market before restarting.

## Venue kill switch

A connector error that makes one venue unusable trips only that venue's bit:

- the venue's error map returns `Fatal`: bad API key, bad signature, missing permission (Binance `-1022`, `-2014`, `-2015`, a failed `session.logon`; Bybit a failed private or trade authentication and its key and permission `retCode`s; Deribit a failed `public/auth`, including the re-authentication after a failed token refresh);
- the venue's error map returns `HardStop`: REST stopped (Binance HTTP 418 IP ban). The kill-switch cancel-all of that venue fails while the ban lasts.

The connector logs the error (`fatal venue error (<code> <msg>); order entry disabled` or the HardStop line), refuses further orders itself and sends the engine a `TripVenueKill` command through its order-event ring, once per session. The engine then:

1. sets the venue's bit and records the reason (`VenueFatal` or `VenueHardStop`);
2. pulls the quotes of that venue's instruments and cancels its working orders (the connector may refuse the cancels when its key is unusable; the shutdown's REST cancel-all still tries);
3. refuses new orders and replaces for that venue in the pre-trade check with `RejectReason::VenueKilled`, and ignores `set_quotes` for its instruments (strategies can ask `ctx.venue_killed(venue)`);
4. keeps trading on every other venue;
5. when every venue that has instruments is killed, trips the global switch with `AllVenuesKilled`, which `on_kill` handles.

At shutdown the venue's REST cancel-all still runs. After a fatal key error it usually fails: `cancel_all FAILED`, exit code 5 ([When cancel_all failed](#when-cancel_all-failed)).

## Shutdown sequence

From `run_live()` in `src/live/session.cpp`:

1. The control thread notices the signal, the elapsed duration, the ring overflow or a kill the engine tripped itself (it checks every 50 ms), publishes the state `stopping` to the status file and logs `fastmm-live: shutting down (<reason>)`.
2. Unless the engine tripped the kill switch itself (it has already pulled quotes and cancelled), it posts a kill-switch command to the engine. The engine logs `kill switch requested`, pulls every quote and queues cancels for every working order. If the control ring is full, the log says `control ring full: kill switch message dropped`; step 3 still runs.
3. Independently of the engine, the control thread calls `cancel_all()` on every venue, one after another, over a new blocking REST connection (so it works even when the venue's network thread is stuck), and waits for each reply. It is skipped in `--dry-run`. Each request has a 5000 ms timeout (`http_timeout_ms`), and there is one request per subscribed instrument:
   - Binance: `DELETE /api/v3/openOrders` per symbol; error `-2011` (nothing open) counts as success.
   - Bybit: `/v5/order/cancel-all` per symbol.
   - Deribit: `private/cancel_all_by_instrument` per instrument.
4. It waits 200 ms so that the engine's queued cancels reach the wire, then stops the engine thread.
5. Each network thread sends what is still queued, runs its reactor for up to 100 ms more and disconnects. The journal is flushed and closed with a trailer block.
6. The summary lines are logged: engine counters (`fastmm-live: events=... risk_rejects=<n> venue_rejects=<n>`), the rejects per reason for each kind that had any (`fastmm-live: risk_rejects by reason: MaxPosition 12, RateLimit 5`), PnL (`fastmm-live: realized_pnl=... unrealized_pnl=... fees=...`), one `[<venue>] final:` line per venue, the clock statistics, after a kill that was not requested or any venue kill `fastmm-live: kill switch flags=<hex> reason=<reason> kills=<n> venue_kills=<n>` (ERROR), then `fastmm-live: shutdown took <n> ms (cancel_all ok)` or `(cancel_all FAILED)`.
7. The status file is marked `stopped` and left in place.
8. `fastmm-live: exit code <n>` is logged last.

In two Binance Demo sessions (one venue, one symbol) shutdown took 555 ms and 697 ms. The upper bound is roughly 5 s per REST request that times out, plus 300 ms.

## Reading the last lines

```text
fastmm-live: shutdown took 697 ms (cancel_all ok)
fastmm-live: exit code 0
```

- `<n> ms` runs from step 1 to step 6.
- `cancel_all ok`: every venue's REST cancel-all succeeded, or was skipped in a dry run. It does not prove that no order is left ([Go-live checklist](go-live-checklist.md#stopping)).
- `cancel_all FAILED`: at least one venue's cancel-all failed, and the exit code is 5 whatever stopped the session ([Exit codes](../../reference/cli.md#exit-codes)).

## When cancel_all failed

1. Cancel the open orders on the venue's website or app.
2. Find the cause in the ERROR lines before the shutdown line:
   - `<venue>: kill-switch cancel-all for <symbol> failed: <status> <error>` (Binance)
   - `<venue>: kill-switch cancel-all for <symbol> failed: status=<s> retCode=<code> <msg>` (Bybit)
   - `<venue>: kill-switch cancel_all_by_instrument <symbol> failed: status=<s> code=<code> <msg>` (Deribit)
   - `<venue>: kill-switch cancel-all failed: <error>` (the REST connection itself failed)

   Typical causes are a network outage (status 0 with an error text), a revoked key or a missing permission (`401`, `403`; the venue kill switch has usually tripped earlier with `VenueFatal`), an IP ban (`418` on Binance, `403` on Bybit; `VenueHardStop`) or the venue being down.
3. Confirm that the venue shows no open orders before you start anything again. On Deribit, `cancel_on_disconnect = true` also makes the venue cancel the orders of the order connection when it closes.
4. Do not rely on a restart to clean up. A new session reconciles open orders when it connects and cancels live orders its OMS does not know (`cancelling unknown live order <id>`), but only orders the connector recognises as its own.
