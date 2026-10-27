# Kill switch and shutdown

The kill switch stops a session from sending new orders: it pulls every quote and cancels every working order, on one venue or on all of them.

Every trip is also a row in the [store](../../reference/storage.md) with its reason and the PnL at the time: `fastmm-pnl recover --engine <name>` or `SELECT * FROM kill_events`.

## The kill switch

While the global switch is set, every new order and replace is refused (`KillSwitch`); cancels always go. A venue's switch refuses that venue's orders (`VenueKilled`) and leaves the others trading.

| Bit | In the log | Scope |
|---|---|---|
| 0 | `0x1` | every venue |
| 1 + venue id | `0x2` for venue 0, `0x4` for venue 1, ... | one venue; ids 30 and up share `0x80000000` |

Venue ids follow the order of the `[venues.<name>]` tables, from 0. The engine keeps the first reason each bit was set for, and `fastmm-top` shows the flags and reasons ([Monitoring a live session](monitor-with-fastmm-top.md)).

A kill leaves the inventory in place. To work the position off, run `fastmm-ctl flatten`: its reduce-only orders pass the global switch while quoting stays off ([Operating a running session](operate-a-running-session.md#flatten)). A venue's switch still refuses them.

`SIGHUP` or `fastmm-ctl unkill` clears the switch of a running session with `on_kill = "stay"` and resumes quoting. It also clears a latched `max_loss` trip; the loss already spent stays in the budget.

## What trips it

### Stops you ask for

These run the [shutdown sequence](#shutdown-sequence) directly and do not go through `on_kill`.

| Cause | Log line | Exit code |
|---|---|---|
| `fastmm-ctl stop` | `fastmm-live: shutting down (control socket: stop)` | 0 |
| `fastmm-ctl handoff`, or `fastmm-live --takeover` ([Hand over a running session](hand-over-a-session.md)) | `fastmm-live: shutting down (control socket: handoff)`, later `instance lock <path> released` | 0 |
| Ctrl-C (SIGINT) or SIGTERM | `fastmm-live: shutting down (signal)` | 0 |
| `--duration` elapsed | `fastmm-live: shutting down (duration elapsed)` | 0 |
| `fastmm-ctl kill` | `kill switch requested (flags=0x1); pulling quotes and cancelling all` | keeps running, quoting off; `fastmm-ctl unkill` clears it |

Each stop is followed by `stopping: pulling quotes and cancelling all`.

### Failures that stop the process

These also run the shutdown sequence directly, at ERROR.

| Cause | Log line | Exit code |
|---|---|---|
| a venue's order-event ring overflowed | `order ring overflow on <venue>: tripping the kill switch` | 5 |
| the journal file cannot be written (full disk, I/O error) | `journal write failed (<error>): tripping the kill switch and shutting down` | 5 |
| behind `fastmm-gateway`, the gateway went away | `the gateway closed the attachment: no market data, no orders` | 5 |
| the engine thread stalled for `[engine] stall_ms` | `fastmm-live: the engine thread has not come round its loop for <n> ms ...` | 5 ([Engine stall](#engine-stall)) |
| a Python slow method raised | `fastmm-live: slow tier failed (Exception): a slow method raised`, traceback on stderr | 7 |
| a slow method ran past its `timeout` | `fastmm-live: slow tier failed (Timeout): ...` | 7 |
| the slow methods' fills ring was full | `fastmm-live: slow tier failed (FillsOverflow): ...` | 7 |
| the slow thread ended | `fastmm-live: slow tier failed (ThreadExited): ...` | 7 |

### Global kills

These trip the global switch, at ERROR, and then [`on_kill`](#after-a-kill-the-engine-trips-itself) decides what happens.

| Reason | Cause |
|---|---|
| `MaxLoss` | net PnL, plus the carry from earlier sessions, fell to `-[risk] max_loss`; the trip is [latched on disk](#the-latched-loss-budget) |
| `OrderIdsExhausted` | the session's 32-bit order sequence ran out (4.3 billion orders); a restart takes a fresh epoch |
| `TransportFull` | the outbound ring to a venue was full (`outbound transport full: <n> message(s) dropped`) |
| `JournalOverflow` | the journal ring was full |
| `StoreFailure` | the store failed a write or dropped records, with `[storage] kill_on_failure = true` ([Storage](../../reference/storage.md#what-a-backend-guarantees)); trips again after `unkill` on a new failure |
| `StrategyError` | a Python hot hook raised, called `ctx.fail` or set a non-finite level; no hook runs again |
| `GatewayMaxLoss` | the gateway's account `[gateway] max_loss` ([Account risk](run-behind-a-gateway.md#account-risk)); the gateway's kill file latches it, not the strategy's |
| `GatewayOperator` | an operator's `kill` on the gateway ([Control](run-behind-a-gateway.md#control)) |
| `AllVenuesKilled` | every venue that has instruments is killed |

The log line is `kill switch engaged (<reason>, flags=0x1); pulling quotes and cancelling all`.

### Venue kills

These stop one venue; the others keep trading ([Venue kill switch](#venue-kill-switch)).

| Reason | Cause | First log line |
|---|---|---|
| `VenueFatal` | bad key, signature or permission | `<venue>: asking the engine to kill this venue (VenueFatal)` |
| `VenueHardStop` | REST stopped, such as a Binance HTTP 418 IP ban | `<venue>: HTTP 418 IP ban: REST stopped until restart` |
| `DeadMansSwitchLost` | a venue-side dead man's switch was not refreshed for a whole window; the venue has already cancelled, so do not requote until someone has looked ([Run in production](running-in-production.md#a-lapsed-switch-is-a-kill)) | `<venue>: countdownCancelAll not refreshed within <n> ms; the venue has cancelled this account's orders` |
| `FeedLost` | a `nasdaq_itch` feed cannot rebuild its books: the recovery buffer overflowed during two snapshots in a row, or a gap without `glimpse_url` ([Venue connectors](../../reference/venues/nasdaq-itch.md#startup-and-recovery)) | `<venue>: market data lost (<cause>): the venue stops` |

The log reads `venue <id> kill switch engaged (<reason>, flags=0x2)`, then `<n> of <m> venue(s) still trading`.

### What does not trip it

- Market data lost or stale: the engine pulls that venue's quotes and requotes once the book is valid again.
- Order channel lost: with `cancel_on_order_channel_loss = true`, the default, the connector cancels over REST, reconnects with backoff and reconciles. Repeated authentication failure ends in `VenueFatal`.
- A venue rejecting one order: counted per reason ([Troubleshooting](troubleshooting.md#orders-and-reconciliation)).

## After a kill the engine trips itself

`[engine] on_kill` decides what `fastmm-live` does after a [global kill](#global-kills).

| `on_kill` | Behaviour |
|---|---|
| `"exit"` (default) | within 50 ms logs `fastmm-live: shutting down (kill switch: <reason>; [engine] on_kill = "exit")` and runs the [shutdown sequence](#shutdown-sequence); exit code 6 |
| `"stay"` | keeps running with quoting off; only `fastmm-ctl flatten` sends orders; logs `kill switch engaged ... on_kill = "stay"` every 10 s; SIGINT or SIGTERM then shuts down with exit code 0 |

Use `"stay"` only when someone watches the session. Alert on exit codes 5, 6 and 7. A failed cancel-all makes any exit code 5 ([Reading the last lines](#reading-the-last-lines)).

After a `max_loss` kill, check the PnL and the market, then clear the [latched budget](#the-latched-loss-budget); until then every start exits 6.

## The latched loss budget

`[risk] max_loss` is a budget for the deployment, not for one process. `fastmm-live` keeps it in `[engine] kill_file`, default `<journal_dir>/<name>.kill`, rewritten whenever the numbers change:

```text
fastmm-kill 1
latched 1
reason 2
realized_raw -4200000000
fees_raw 31000000
sessions 7
updated_ns 1758600000000000000
```

| Field | Meaning |
|---|---|
| `realized_raw`, `fees_raw` | sums over every session since the file was cleared, in 1e-8 of the settlement currency; the engine adds `realized - fees` to the running session's PnL |
| `latched` | set by a `max_loss` trip; the next start prints the reason, the carry and the session count, and exits 6 before contacting any venue |
| `sessions` | sessions counted |

Unrealized PnL is not carried: an open position is measured again from the venue once the session has reconciled. No other kill reason is latched. `fastmm-top` shows `LATCHED` next to the state and `carried=` and `budget_used=` on the `pnl` line.

To clear a trip and re-arm the whole budget:

```bash
./build/release/bin/fastmm-live --config <file> --clear-kill
```

Removing the file does not clear a trip the store recorded: if the store's newest session ended on a `max_loss` trip, the start still exits 6 until `--clear-kill`. Otherwise it starts and logs that `max_loss` is measured from zero again.

### A daily budget

With `[risk] max_loss_period = "utc_day"`, `max_loss` is a budget for one UTC day. At 00:00 UTC net PnL, unrealized included, is measured from zero again and the kill file gains a `period_start_ns` line. A start on a later day begins at zero. A trip stays latched across midnight and still needs `--clear-kill`.

## Venue kill switch

A connector error that makes one venue unusable trips only that venue's bit.

| Kind | Examples |
|---|---|
| `Fatal` | Binance `-1022`, `-2014`, `-2015`, a failed `session.logon`; Bybit failed private or trade authentication and its key and permission `retCode`s; Deribit failed `public/auth`, including after a failed token refresh |
| `HardStop` | Binance HTTP 418 IP ban; the venue's cancel-all fails while the ban lasts |

The connector logs `fatal venue error (<code> <msg>); order entry disabled` and refuses further orders. Then the engine:

1. sets the venue's bit and records `VenueFatal` or `VenueHardStop`;
2. pulls that venue's quotes and cancels its orders, which the venue may refuse with an unusable key;
3. refuses new orders for that venue and ignores `set_quotes` for its instruments; a strategy can ask `ctx.venue_killed(venue)`;
4. keeps trading on every other venue;
5. trips the global switch with `AllVenuesKilled` once every venue with instruments is killed.

At shutdown the venue's REST cancel-all still runs. After a key error it usually fails, so the exit code is 5 ([When cancel_all failed](#when-cancel_all-failed)).

## Shutdown sequence

Python strategies stop the same way ([Run a Python strategy live](../strategies/python-live.md)).

1. The control thread sees the cause: a signal or `stop` at once, anything else within 50 ms. It removes the control socket, writes the kill file, sets the status to `stopping` and logs `fastmm-live: shutting down (<reason>)`.
2. Unless the engine already tripped itself, the control thread asks it to pull every quote and cancel every working order. If the control ring is full, the log says `control ring full: kill switch message dropped` and step 3 still runs.
3. The control thread calls each venue's REST cancel-all over its own connection, so it works even if a network thread is stuck. Each request times out after 5000 ms (`http_timeout_ms`), one per instrument. It is skipped in `--dry-run`, and behind a gateway, which cancels when the attachment closes.
4. The engine runs until the venue has confirmed every cancel, a round trip. After 1 s it stops waiting and logs `<n> order(s) still open 1000 ms after the cancel-all`.
5. Each network thread sends what is queued, waits up to 100 ms and disconnects. The journal is closed with a trailer.
6. The summary is logged: counters, rejects per reason, PnL, one `[<venue>] final:` line per venue, the kill flags if any, then `shutdown took <n> ms (cancel_all ok)` or `(cancel_all FAILED)`.
7. The status file is marked `stopped` and the store records the close.
8. The instance lock is released, so a `--standby` or `--takeover` process starts ([Hand over a running session](hand-over-a-session.md)).
9. `fastmm-live: exit code <n>` is logged last.

| Venue | Cancel-all request |
|---|---|
| Binance Spot | `DELETE /api/v3/openOrders`; `-2011` (nothing open) counts as success |
| Binance USDⓈ-M | `DELETE /fapi/v1/allOpenOrders` |
| Bybit | `/v5/order/cancel-all` |
| Deribit | `private/cancel_all_by_instrument` |

A one-venue, one-symbol Binance Demo session shuts down in 0.5 to 0.7 s. The upper bound is about 5 s per REST request that times out, plus 300 ms.

## Engine stall

An engine thread stuck inside one step cannot pull its quotes, while the network threads keep the venue connections and dead man's switches alive. The control thread therefore watches a heartbeat the engine advances every turn, at least once a millisecond when idle. When it has not moved for `[engine] stall_ms` (default 10000), the control thread:

1. removes the control socket and logs the stall at ERROR;
2. cancels all on every venue over REST, as in step 3 above; behind a gateway, the gateway cancels when the process exits;
3. writes the kill file and the status `stopped`, logs `fastmm-live: engine stalled; cancel_all ok, exit code 5`, and exits 5.

The journal has no trailer and the store misses its last records, as after a crash; the next start recovers ([Run in production](running-in-production.md#restarts)).

| Setting | Effect |
|---|---|
| `stall_ms = 0` | watch off |
| hooks that legitimately take seconds | raise `stall_ms` |
| `threading = "single"` | a step includes the network loop; keep `stall_ms` above `http_timeout_ms` (5000) |

Time the whole process was stopped, such as a paused VM or SIGSTOP, is not counted, nor is warm-up before the first turn.

## Reading the last lines

```text
fastmm-live: shutdown took 697 ms (cancel_all ok)
fastmm-live: exit code 0
```

| Text | Meaning |
|---|---|
| `<n> ms` | from step 1 to step 6 of the shutdown |
| `cancel_all ok` | every venue's REST cancel-all succeeded, or was skipped in a dry run; it does not prove no order is left ([Go-live checklist](go-live-checklist.md#stopping)) |
| `cancel_all FAILED` | at least one venue's cancel-all failed; the exit code is 5 whatever stopped the session ([Exit codes](../../reference/cli.md#exit-codes)) |

## When cancel_all failed

1. Cancel the open orders on the venue's website or app.
2. Find the cause in the ERROR lines before the shutdown line:

    ```text
    <venue>: kill-switch cancel-all for <symbol> failed: <status> <error>                         # Binance
    <venue>: kill-switch cancel-all for <symbol> failed: status=<s> retCode=<code> <msg>          # Bybit
    <venue>: kill-switch cancel_all_by_instrument <symbol> failed: status=<s> code=<code> <msg>   # Deribit
    <venue>: kill-switch cancel-all failed: <error>                                               # the connection failed
    ```

    | Status | Usual cause |
    |---|---|
    | 0 with an error text | network outage |
    | `401`, `403` | revoked key or missing permission; `VenueFatal` has usually tripped earlier |
    | `418` on Binance, `403` on Bybit | IP ban; `VenueHardStop` |
    | other | the venue is down |

3. Confirm the venue shows no open orders before you start anything. Deribit with `cancel_on_disconnect = true`, Binance USDⓈ-M with `dead_mans_switch_ms` and Bybit with DCP armed also cancel once the connection is gone ([Run in production](running-in-production.md#venue-side-cancel-switches)).
4. A restart sweeps what is left: each connector queries open orders on its first connect and the engine cancels those it does not know (`cancelling unknown live order <id>`), manual orders included. With `[engine] cancel_unknown = "own"` it cancels only those with a FastMM client order id ([Configuration](../../reference/configuration.md#engine)). Until then they rest and can fill; the restarted session books those fills.
