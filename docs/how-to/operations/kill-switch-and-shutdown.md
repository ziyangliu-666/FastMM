# Kill switch and shutdown

Every trip, global or per venue, is also a row in the [store](../../reference/storage.md) with the reason and the PnL at the time: `fastmm-pnl recover --engine <name>` and `SELECT * FROM kill_events`.

## The kill switch

The kill switch is one atomic 32-bit flag word in the risk engine (`include/fastmm/core/risk.hpp`). Any thread can set it. While the global bit is set, the pre-trade check refuses every new order and replace (`RejectReason::KillSwitch`); cancels are always allowed. When it trips, the engine turns quoting off, pulls every quote and sends a cancel for every working order over the venues' order channels.

| Bit | Value in the log | Meaning |
|---|---|---|
| 0 | `0x1` | Global: no new orders on any venue |
| 1 + venue id | `0x2` (venue 0), `0x4` (venue 1), ... | One venue only (`RejectReason::VenueKilled`); venues from id 30 up share bit 31 (`0x80000000`) |

Venue ids follow the order of the `[venues.<name>]` tables in the config, starting at 0. The engine keeps the first reason each bit was set for (`KillReason`: `Requested`, `MaxLoss`, `TransportFull`, `JournalOverflow`, `AllVenuesKilled`, `VenueFatal`, `VenueHardStop`, `OrderRingOverflow`, `StrategyError`, `FeedLost`, `OrderIdsExhausted`, `DeadMansSwitchLost`, `GatewayMaxLoss`); `fastmm-top` shows the flags and reasons ([Monitoring a live session](monitor-with-fastmm-top.md)).

`SIGHUP`, or `fastmm-ctl unkill`, clears the kill switch of a running session and resumes quoting (`[engine] on_kill = "stay"`). It also clears a latched `max_loss` trip; the loss already spent stays in the budget.

A kill leaves the inventory on: it pulls quotes and cancels orders, nothing more. Working the position off is `fastmm-ctl flatten`, and it has to run **before** the kill — while the kill switch is engaged the pre-trade check refuses the flatten's orders too ([Operating a running session](operate-a-running-session.md#flatten)).

## The latched loss budget

`[risk] max_loss` is a budget for the deployment, not for one process. `fastmm-live` keeps it in `[engine] kill_file` (default `<journal_dir>/<name>.kill`), written through a temporary file and a rename whenever the numbers change:

```text
fastmm-kill 1
latched 1
reason 2
realized_raw -4200000000
fees_raw 31000000
sessions 7
updated_ns 1758600000000000000
```

- `realized_raw` and `fees_raw` are the sum over every session since the file was last cleared, in 1e-8 units of the settlement currency. The engine adds `realized - fees` to the running session's PnL before comparing it with `max_loss`.
- Unrealized PnL is not carried. An open position is remeasured from the venue's view once the session has reconciled.
- A `max_loss` trip sets `latched`. The next start prints the reason, the carried PnL and the session count, and exits with code 6 before contacting any venue.
- `fastmm-live --clear-kill`, or removing the file, arms the whole `max_loss` again and forgets the losses so far.
- No other kill reason is latched.

`fastmm-top` shows `LATCHED` next to the state and `carried=` / `budget_used=` on the `pnl` line.

## What trips it

| Cause | Scope | Log line | Level | What happens next |
|---|---|---|---|---|
| `fastmm-ctl kill` | Global, requested | `kill switch requested (flags=0x1); pulling quotes and cancelling all` | WARN | The session keeps running with quoting off; `fastmm-ctl unkill` clears it |
| `fastmm-ctl stop` | Global, requested | `fastmm-live: shutting down (control socket: stop)`, then `kill switch requested` | WARN | [Shutdown sequence](#shutdown-sequence); exit code 0 |
| Ctrl-C (SIGINT) or SIGTERM | Global, requested | `fastmm-live: shutting down (signal)`, then `kill switch requested (flags=0x1); pulling quotes and cancelling all` | WARN | [Shutdown sequence](#shutdown-sequence); exit code 0 |
| `--duration` elapsed | Global, requested | `fastmm-live: shutting down (duration elapsed)`, then the same `kill switch requested` line | WARN | Shutdown sequence; exit code 0 |
| A venue's order-event ring overflowed | Global, requested | `order ring overflow on <venue>: tripping the kill switch`, then `fastmm-live: shutting down (order ring overflow)` | ERROR | Shutdown sequence; exit code 5 |
| `[risk] max_loss`: net PnL (realised plus unrealised minus fees, plus the carry from earlier sessions) fell to `-max_loss` or below | Global | `kill switch engaged (MaxLoss, flags=0x1); pulling quotes and cancelling all` | ERROR | [`on_kill`](#after-a-kill-the-engine-trips-itself), and the trip is [latched on disk](#the-latched-loss-budget) |
| The session's 32-bit client order id sequence ran out (4.3 billion orders) | Global | `kill switch engaged (OrderIdsExhausted, ...)` | ERROR | `on_kill`; restart the process, which takes a fresh session epoch |
| The outbound ring to a venue was full | Global | `outbound transport full: <n> message(s) dropped; tripping kill switch`, then `kill switch engaged (TransportFull, ...)` | ERROR | `on_kill` |
| The journal ring was full | Global | `kill switch engaged (JournalOverflow, ...)` | ERROR | `on_kill` |
| A fatal venue error (see [below](#venue-kill-switch)) | That venue | `<venue>: asking the engine to kill this venue (VenueFatal)`, `venue <id> kill switch engaged (VenueFatal, flags=0x2); ...`, `[<venue>] venue kill switch engaged (VenueFatal): ...; <n> of <m> venue(s) still trading` | ERROR | Only that venue stops; the others keep trading |
| A venue-side dead man's switch could not be refreshed for a whole window ([Running in production](running-in-production.md#a-dead-mans-switch-that-lapses-is-a-kill-not-a-retry)) | That venue | `<venue>: countdownCancelAll not refreshed within <n> ms; the venue has cancelled this account's orders`, then `venue <id> kill switch engaged (DeadMansSwitchLost, ...)` | ERROR | Only that venue stops. The venue has already cancelled its orders; it must not requote until someone has looked |
| A `nasdaq_itch` feed cannot rebuild its books: the recovery buffer overflowed during two snapshots in a row, or a gap without `glimpse_url` ([Venue connectors](../../reference/venues.md#startup-and-recovery)) | That venue | `<venue>: market data lost (<cause>): the venue stops`, then `venue <id> kill switch engaged (FeedLost, ...)` | ERROR | Only that venue stops |
| Behind `fastmm-gateway`, the account's `[gateway] max_loss` over every strategy ([Run behind a gateway](run-behind-a-gateway.md#account-risk)) | Every venue, so global | `venue <id> kill switch engaged (GatewayMaxLoss, ...)`, then `kill switch engaged (AllVenuesKilled, ...)` | ERROR | `on_kill`; the strategy's own kill file is not latched, the gateway's is |
| Every venue that has instruments is killed | Global | `kill switch engaged (AllVenuesKilled, ...)` | ERROR | `on_kill` |
| A hot hook of a Python strategy raised, called `ctx.fail` or set a float level that is not finite; no hook runs again | Global | `kill switch engaged (StrategyError, ...)`; after the session, `fastmm: py:<Class>.<hook> ... kill switch tripped: StrategyError` on stderr | ERROR | `on_kill` |
| A slow method of a Python strategy raised | Global, requested | `fastmm-live: slow tier failed (Exception): a slow method raised`, then `fastmm-live: shutting down (slow tier failed)`; the traceback on stderr | ERROR | Shutdown sequence; exit code 7 |
| A slow method ran past its `timeout` | Global, requested | `fastmm-live: slow tier failed (Timeout): a slow method ran past its timeout`, then `fastmm-live: shutting down (slow tier failed)` | ERROR | Shutdown sequence; exit code 7 |
| The engine found the slow methods' fills ring full | Global, requested | `fastmm-live: slow tier failed (FillsOverflow): the fills ring was full`, then `fastmm-live: shutting down (slow tier failed)` | ERROR | Shutdown sequence; exit code 7 |
| The slow thread ended while the session ran | Global, requested | `fastmm-live: slow tier failed (ThreadExited): the slow thread ended`, then `fastmm-live: shutting down (slow tier failed)` | ERROR | Shutdown sequence; exit code 7 |

The exit codes in the table assume `cancel_all ok`; a failed cancel-all makes the code 5 ([Reading the last lines](#reading-the-last-lines)).

These events do not trip the kill switch:

- Market-data loss or a stale feed: the engine pulls the quotes on the instruments of that venue and requotes when the book is valid again.
- Order-channel loss: with `cancel_on_order_channel_loss = true` (the default) the connector cancels everything over REST, reconnects with backoff (without giving up) and reconciles open orders. A connection that keeps failing its authentication ends in a fatal venue error, below.
- A venue rejecting one order (filters, balance, rate limit): counted per reason ([Troubleshooting](troubleshooting.md#orders-and-reconciliation)).

## After a kill the engine trips itself

`[engine] on_kill` decides what `fastmm-live` does after a global kill it did not request: every row above except signal, `--duration`, order ring overflow and the slow-tier rows, which shut down in any case.

| `on_kill` | Behaviour |
|---|---|
| `"exit"` (default) | Within 50 ms the control thread logs `fastmm-live: shutting down (kill switch: <reason>; [engine] on_kill = "exit")` at ERROR and runs the [shutdown sequence](#shutdown-sequence). Exit code 6, or 5 if a cancel-all failed |
| `"stay"` | The process keeps running with quoting off and new orders refused. At once and then every 10 s it logs `fastmm-live: kill switch engaged (<reason>, flags=<hex>) and [engine] on_kill = "stay": quoting is off and no new orders are sent; stop the process (SIGINT/SIGTERM) to cancel all and exit` at ERROR. SIGINT or SIGTERM then runs the shutdown sequence: exit code 0, or 5 if a cancel-all failed |

Use `"stay"` only when someone watches the session. Alert on exit codes 5, 6 and 7. After a `max_loss` kill, check the PnL and the market, then clear the [latched budget](#the-latched-loss-budget): a plain restart exits with code 6 again.

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

From `run_live()` in `src/live/session.cpp`, which also runs Python strategies ([Run a Python strategy live](../strategies/python-live.md)):

1. The control thread notices the signal, a `stop` on the [control socket](operate-a-running-session.md), the elapsed duration, the ring overflow, a slow-tier failure or a kill the engine tripped itself (it checks every 50 ms), publishes the state `stopping` to the status file and logs `fastmm-live: shutting down (<reason>)`.
2. Unless the engine tripped the kill switch itself (it has already pulled quotes and cancelled), it posts a kill-switch command to the engine. The engine logs `kill switch requested`, pulls every quote and queues cancels for every working order. If the control ring is full, the log says `control ring full: kill switch message dropped`; step 3 still runs.
3. Independently of the engine, the control thread calls `cancel_all()` on every venue, one after another, over a new blocking REST connection (so it works even when the venue's network thread is stuck), and waits for each reply. It is skipped in `--dry-run`. Each request has a 5000 ms timeout (`http_timeout_ms`), and there is one request per subscribed instrument:
   - Binance: `DELETE /api/v3/openOrders` per symbol; error `-2011` (nothing open) counts as success.
   - Bybit: `/v5/order/cancel-all` per symbol.
   - Deribit: `private/cancel_all_by_instrument` per instrument.
4. It waits 200 ms so that the engine's queued cancels reach the wire, then stops the engine thread.
5. The control socket is closed and removed, so no further command can reach a session that is shutting down. Each network thread sends what is still queued, runs its reactor for up to 100 ms more and disconnects. The journal is flushed and closed with a trailer block.
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
3. Confirm that the venue shows no open orders before you start anything again. On Deribit, `cancel_on_disconnect = true` also makes the venue cancel the orders of the order connection when it closes, and on Binance USDⓈ-M the `dead_mans_switch_ms` countdown does the same once it runs out ([Running in production](running-in-production.md#binance-spot-has-no-dead-mans-switch-and-bybits-is-not-self-serve)).
4. A restart sweeps what is left: every connector queries open orders on its first connect and the engine cancels the ones it does not know (`cancelling unknown live order <id>`), but only orders the connector recognises as FastMM's. Until the next start they keep resting and can fill; the restarted session books those fills from the venue's trade history ([Running this in production](running-in-production.md#2-orders-the-engine-cannot-see)).
