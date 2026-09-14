# Kill switch and shutdown

This page explains what stops a `fastmm-live` session from trading, what the process does after a
kill switch trips, what happens between Ctrl-C and the process exiting, and what to do when the
final line says `cancel_all FAILED`.

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

Venue ids follow the order of the `[venues.<name>]` tables in the config, starting at 0. The engine
keeps the first reason each bit was set for (`KillReason`: `Requested`, `MaxLoss`,
`TransportFull`, `JournalOverflow`, `AllVenuesKilled`, `VenueFatal`, `VenueHardStop`). The flags
and reasons appear in the log line of the trip and in `fastmm-top`: the state line shows
`KILLED (<reason>)` for the global switch or `VENUE KILLED` when only venues are killed, and the
venue table's `kill` column shows each killed venue's reason. `fastmm-live` has no command to reset
the kill switch: restart the process.

## What trips it

| Cause | Scope | Log line | Level | What happens next |
|---|---|---|---|---|
| Ctrl-C (SIGINT) or SIGTERM | Global, requested | `fastmm-live: shutting down (signal)`, then `kill switch requested (flags=0x1); pulling quotes and cancelling all` | WARN | [Shutdown sequence](#shutdown-sequence); exit code 0 |
| `--duration` elapsed | Global, requested | `fastmm-live: shutting down (duration elapsed)`, then the same `kill switch requested` line | WARN | Shutdown sequence; exit code 0 |
| A venue's order-event ring overflowed | Global, requested | `order ring overflow on <venue>: tripping the kill switch`, then `fastmm-live: shutting down (order ring overflow)` | ERROR | Shutdown sequence; exit code 5 |
| `[risk] max_loss`: net PnL (realised plus unrealised minus fees) fell to `-max_loss` or below | Global | `kill switch engaged (MaxLoss, flags=0x1); pulling quotes and cancelling all` | ERROR | [`on_kill`](#after-a-kill-the-engine-trips-itself): shutdown sequence and exit code 6, or keep running |
| The outbound ring to a venue was full | Global | `outbound transport full: <n> message(s) dropped; tripping kill switch`, then `kill switch engaged (TransportFull, ...)` | ERROR | As for `max_loss` |
| The journal ring was full | Global | `kill switch engaged (JournalOverflow, ...)` | ERROR | As for `max_loss` |
| A fatal venue error (see [below](#venue-kill-switch)) | That venue | `<venue>: asking the engine to kill this venue (VenueFatal)`, `venue <id> kill switch engaged (VenueFatal, flags=0x2); ...`, `[<venue>] venue kill switch engaged (VenueFatal): ...; <n> of <m> venue(s) still trading` | ERROR | Only that venue stops; the others keep trading |
| Every venue that has instruments is killed | Global | `kill switch engaged (AllVenuesKilled, ...)` | ERROR | As for `max_loss` |

These events do **not** trip the kill switch:

- **Market-data loss or a stale feed:** the engine pulls the quotes on the instruments of that
  venue and requotes when the book is valid again.
- **Order-channel loss:** with `cancel_on_order_channel_loss = true` (the default) the connector
  cancels everything over REST, reconnects with backoff (without giving up) and reconciles open
  orders. A connection that keeps failing its authentication ends in a fatal venue error, below.
- **A venue rejecting one order** (filters, balance, rate limit): counted per reason; see
  [Troubleshooting](troubleshooting.md).

## After a kill the engine trips itself

`[engine] on_kill` decides what `fastmm-live` does after a global kill it did not ask for: every
row above except the requested ones (signal, `--duration`, order ring overflow, which already shut
down).

| `on_kill` | Behaviour |
|---|---|
| `"exit"` (default) | The control thread notices the kill within 50 ms, logs `fastmm-live: shutting down (kill switch: <reason>; [engine] on_kill = "exit")` and runs the [shutdown sequence](#shutdown-sequence): the engine has already pulled quotes and queued cancels, every venue runs its REST cancel-all and waits for the replies (or the request timeout), the summary is logged, the journal is closed with its trailer and the status file is left as `stopped` with `KILLED (<reason>)`. Exit code **6** when every cancel-all succeeded, 5 when one failed |
| `"stay"` | The process keeps running with quoting off and new orders refused, so you can inspect it. Every 10 s it logs `fastmm-live: kill switch engaged (<reason>, flags=<hex>) and [engine] on_kill = "stay": quoting is off and no new orders are sent; stop the process (SIGINT/SIGTERM) to cancel all and exit` at ERROR. Ctrl-C then runs the full shutdown sequence (exit code 0 if cancel-all succeeds) |

Why `exit` is the default: an unattended session that has stopped trading still looks alive to a
process supervisor, and a `stay` session only shows the problem in its log and in `fastmm-top`. A
process that exits with its own code (6) can be alerted on and is not restarted blindly by a
supervisor that treats non-zero exits as failures. A `max_loss` kill is not a condition a restart
fixes: check the PnL and the market before starting again. Use `stay` when someone watches the
session and wants to inspect it before it cancels and exits.

## Venue kill switch

A connector error that makes one venue unusable trips only that venue's bit:

- the venue's error map returns **Fatal**: bad API key, bad signature, missing permission (Binance
  `-1022`, `-2014`, `-2015`, a failed `session.logon`; Bybit a failed private or trade
  authentication and its key and permission `retCode`s; Deribit a failed `public/auth`, including
  the re-authentication after a failed token refresh);
- the venue's error map returns **HardStop**: REST stopped (Binance HTTP 418 IP ban). The
  kill-switch cancel-all of that venue fails while the ban lasts.

The connector logs the error (`fatal venue error (<code> <msg>); order entry disabled` or the
HardStop line), refuses further orders itself and sends the engine a `TripVenueKill` command
through its order-event ring, once per session. The engine then:

1. sets the venue's bit and records the reason (`VenueFatal` or `VenueHardStop`);
2. pulls the quotes of that venue's instruments and cancels its working orders (the connector may
   refuse the cancels when its key is unusable; the shutdown's REST cancel-all still tries);
3. refuses new orders and replaces for that venue in the pre-trade check with
   `RejectReason::VenueKilled`, and ignores `set_quotes` for its instruments (strategies can ask
   `ctx.venue_killed(venue)`);
4. keeps trading on every other venue;
5. when every venue that has instruments is killed, trips the global switch with
   `AllVenuesKilled`, which `on_kill` handles like any other kill.

The command is an engine input: it is journaled with the order events and a replay applies it at
the same point. At shutdown the venue's REST cancel-all still runs; after a fatal key error it
usually fails, so the last line says `cancel_all FAILED` and the exit code is 5: cancel on the
venue's website.

## Shutdown sequence

From `run_live()` in `src/live/session.cpp`:

1. The control thread notices the signal, the elapsed duration, the ring overflow or a kill the
   engine tripped itself (it checks every 50 ms), publishes the state `stopping` to the status file
   and logs `fastmm-live: shutting down (<reason>)`.
2. Unless the engine tripped the kill switch itself (it has already pulled quotes and cancelled),
   it posts a kill-switch command to the engine. The engine logs `kill switch requested`, pulls
   every quote and queues cancels for every working order. If the control ring is full, the log
   says `control ring full: kill switch message dropped`; step 3 still runs.
3. **Independently of the engine**, the control thread calls `cancel_all()` on every venue, one
   after another, over a new blocking REST connection (so it works even when the venue's network
   thread is stuck), and waits for each reply. It is skipped in `--dry-run`. Each request has a
   5000 ms timeout (`http_timeout_ms`), and there is one request per subscribed instrument:
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
   venue, the clock statistics, after a kill that was not requested or any venue kill
   `fastmm-live: kill switch flags=<hex> reason=<reason> kills=<n> venue_kills=<n>` (ERROR), then
   `fastmm-live: shutdown took <n> ms (cancel_all ok)` or `(cancel_all FAILED)` and, last,
   `fastmm-live: exit code <n>`.
7. The status file is marked `stopped` and left in place, so `fastmm-top` shows the final numbers
   and the kill reason.

In our Binance Demo sessions (one venue, one symbol) shutdown took 555 ms and 697 ms. The upper
bound is roughly 5 s per REST request that times out, plus 300 ms.

## Reading the last lines

```text
fastmm-live: shutdown took 697 ms (cancel_all ok)
fastmm-live: exit code 0
```

- `<n> ms` runs from step 1 to step 6.
- `cancel_all ok`: every venue's REST cancel-all succeeded (or was skipped in a dry run). The engine's
  own cancels were also sent, but `ok` does not prove that no order is left; check the venue.
- `cancel_all FAILED`: at least one venue's cancel-all failed. `fastmm-live` exits with code 5,
  whatever stopped the session.

## Exit codes

A failed cancel-all takes precedence: code 5 whenever orders may still be resting.

| Code | Meaning |
|---|---|
| 0 | Stopped by `--duration` or SIGINT/SIGTERM (also after a kill with `on_kill = "stay"`), `cancel_all ok` |
| 2 | Bad command line, or a venue has no API keys (and no `--dry-run`) |
| 3 | Bad config (including an invalid `on_kill`), unknown strategy or parameters |
| 4 | A venue's reference data failed to load |
| 5 | Runtime failure: `cancel_all FAILED`, the journal cannot be opened, an order ring overflowed, an uncaught error |
| 6 | The engine tripped the kill switch itself (`max_loss`, a full ring, every venue killed) with `on_kill = "exit"`, and `cancel_all ok` |

`fastmm-live --help` prints the same list; a test keeps the two in step.

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
   permission (`401`, `403`; the venue kill switch has usually tripped earlier with `VenueFatal`),
   an IP ban (`418` on Binance, `403` on Bybit; `VenueHardStop`) or the venue being down.
3. **Confirm zero open orders** on the venue before you start anything again. On Deribit,
   `cancel_on_disconnect = true` also makes the venue cancel the orders of the order connection
   when it closes; still check.
4. Do not rely on a restart to clean up. A new session reconciles open orders when it connects and
   cancels live orders its OMS does not know (`cancelling unknown live order <id>`), but only for
   orders the connector recognises as its own.

## Practise it

Before a session that matters, run a short keyed session with open quotes, press Ctrl-C, check
`cancel_all ok` and confirm on the venue that nothing is left ([Run on a testnet](run-on-testnet.md),
[Go-live checklist](go-live-checklist.md)). Decide on `on_kill` and make sure whatever starts
`fastmm-live` reports exit code 6 to someone.
