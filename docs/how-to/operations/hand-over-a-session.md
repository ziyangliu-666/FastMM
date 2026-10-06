# Hand over a running session

Replace a running `fastmm-live` with a new process (a new build, a new configuration) on the same `[engine] name`, so that at no moment do two processes trade that engine's account. The running session cancels its orders and exits; the new one starts from what it left: position, loss budget, client order id sequence and strategy state. Quoting pauses for the length of one restart, not longer.

## Turn on the instance lock

```toml
[engine]
name = "mm"
instance_lock = true          # one process of this engine name at a time
# lock_file = "/run/fastmm/mm.lock"   # default <journal_dir>/<name>.lock
```

The lock is an exclusive `flock(2)` on the lock file. A session takes it before it reads the kill file, the store or the epoch file and before any venue hears from it, and releases it after its shutdown has cancelled, flushed and closed all of them. A second start on the same name exits with code 8 and touches nothing:

```text
fastmm-live: process 41377 holds the instance lock runs/mm.lock and trades this engine's account. Take it over with --takeover, wait for it with --standby, or stop it.
```

The kernel drops the lock with the process, so a crash, `kill -9` or an OOM kill never leaves it held. The file stays and holds the holder's pid. `flock` is local to one host: two hosts sharing a `journal_dir` over NFS are not kept apart.

A session also refuses to start when another one answers on its control socket without holding the lock (a session started before `instance_lock` was set).

## Hand over

With the running session started with `instance_lock = true`:

```bash
fastmm-live --config mm-v2.toml --takeover
```

or, to have a process waiting and decide the moment yourself:

```bash
fastmm-live --config mm-v2.toml --standby &
fastmm-ctl --name mm handoff
```

The new configuration may change anything but `[engine] name`, `journal_dir`, `lock_file` and the control socket path: those are how the two processes find each other. `--standby` and `--takeover` take the lock whatever `instance_lock` says. Python strategies take the same options (`python -m fastmm run ... --takeover`, `fastmm.run_live(..., takeover=True)`).

What happens, in order, when the new process can wait warm ([below](#warm-standby)):

| Step | New process | Running process |
|---|---|---|
| 1 | Loads the configuration and the strategy, the venues' reference data and the account fees (read-only REST); finds the lock held | trades |
| 2 | Warm standby: takes the next session epoch, opens its journal, connects to the venues' market data only and starts its engine and strategy; every order is refused (`NotReconciled`) | trades |
| 3 | Once every book is synced (at most 30 s), `--takeover` sends `handoff` to the control socket; `--standby` waits for whoever stops the running session | answers `ok handing over` |
| 4 | keeps reading market data | `fastmm-live: shutting down (control socket: handoff)`: control socket closed, kill switch, cancel-all on every venue, engine and network threads stopped, journal trailer, `[strategy] state_file` written, store session closed, kill file written |
| 5 | | `instance lock <path> released`, exit 0 |
| 6 | Takes the lock (it polls every 10 ms): `standby: instance lock <path> taken over`. Reads the kill file, the strategy state and the store's positions, opens the store, the status file and the control socket, and opens each venue's private channels: `standby: took over after <n> ms warm` | |
| 7 | Replays the venue's executions since the store's last fill, reconciles, quotes: `every venue has reconciled its orders and executions since the start; orders are sent from now on` | |

Against the local simulator the warm standby's first order follows the old process's exit by a few tens of milliseconds, a cold start's by a few hundred. On a real venue the warm pause is the private channels' connect and the reconciliation; the books are already there.

Where a standby cannot wait warm it waits at the end of step 1, before it connects at all, and does all of step 6 and 7 plus the market-data connect and the book snapshots after the lock.

Nothing rests at the venue between steps 4 and 7. An order of the old session that its cancel-all missed has an id of the old session epoch, which the new session's start-up sweep does not recognise and cancels (`cancelling unknown live order <id>`).

## Warm standby

A standby waits warm when it holds no lock at start, connects to the venues itself (not `--gateway`), runs `threading = "split"`, and every venue it trades on can hold its private channels back and replays its executions before it quotes. Binance Spot can ([Venue connectors](../../reference/venues.md)); with any other connector in the configuration the standby waits cold. A pool member opens nothing before the lock.

Until it takes the lock over, the standby:

- reads market data, syncs its books and runs its strategy's hooks on them: the strategy warms up. Its quotes and orders are refused quietly, as before any session's first reconciliation (`set_quotes` returns false, a direct order fails with `NotReconciled`), and count as no reject;
- sends, cancels and queries nothing of the account: no order entry, no user stream, no execution replay, no open-order sweep, which would find the running session's orders and cancel them as unknown;
- writes its own journal and nothing else: not the kill file, the strategy state file, the store, the status file or the control socket, which are the running session's;
- has taken the next session epoch at start, so its client order ids are its own from its first order on;
- has read the kill file once, to refuse a latched `max_loss` trip (exit 6) before it connects.

Taking over is one control message to its engine (`ControlCommand::TakeOver`, journaled): the PnL the kill file now carries, and the strategy's state from `[strategy] state_file`, which `restore()` gets then, after the warm-up, rather than before the first event. The strategy keeps what it learnt from the market data and takes what the session before it saved; a strategy whose `restore()` replaces everything it holds loses the warm-up but nothing else. A replay of the journal applies the carry and, as for any session, restores nothing.

If the standby stops before it took over (a signal, the handoff refused or timed out), it cancels nothing at the venue: the orders there are the running session's.

## When a step fails

Until the running session has accepted the handoff, every failure leaves it trading. After that, every failure ends with no process trading and no order resting.

| Failure | Result |
|---|---|
| The new process cannot load its configuration, strategy or the reference data | It exits 3 or 4 at step 1; the running session never hears of it |
| The running session refuses `handoff` (it holds no lock), or has no control socket, or does not answer within 5 s | `--takeover` exits 8; the running session trades on |
| Its cancel-all fails | It exits 5 and still releases the lock; the new session's start-up sweep cancels what is left |
| Its shutdown hangs | The shutdown watchdog ends it 60 s after the stop ([Kill switch and shutdown](kill-switch-and-shutdown.md#shutdown-sequence)); the kernel drops the lock; the new session starts and sweeps |
| The lock is not free `[engine] handoff_timeout_ms` (75000) after the handoff was accepted | `--takeover` exits 8. The running session is already stopping: nobody trades. Start again |
| The new process fails after taking the lock (a venue unreachable, the store) | It exits with that code and releases the lock: nobody trades, nothing rests. Start the old or the new build again |
| The running session crashes while a `--standby` waits | The standby takes the lock at once and starts like a restart after a crash ([Run in production](running-in-production.md#restarts)) |
| SIGINT/SIGTERM to a waiting `--standby` | It exits 0; it never traded and cancels nothing |
| The kill file holds a latched `max_loss` trip when the standby takes the lock | It exits 6 without opening its private channels |

A `--standby` without `--takeover` waits until the lock is free, however long; run it under a supervisor that does not also restart the running session.

## What carries over

| State | How |
|---|---|
| Position | the store and the venue's execution replay, as on any restart; the old session wrote its last fill before releasing the lock |
| Kill switch and `[risk] max_loss` budget | `[engine] kill_file`, written last by the old session |
| Client order ids | the next session epoch from `[engine] epoch_file`: the epoch is the high 16 bits of every id, so the two sessions' ids never meet |
| Strategy state | `state()` and `restore()` with `[strategy] state_file` ([Strategy API](../../reference/strategy-api.md#state-across-sessions)): the old session writes the file after its strategy has stopped, the new one reads it after taking the lock (a warm standby restores it then, after its warm-up). Python strategies define the same two methods |
| Parameters set with `fastmm-ctl param`, limits set with `limits`, scoped pulls | not carried: put them in the new configuration |

## Behind a gateway

A strategy attached to `fastmm-gateway` hands over the same way: `fastmm-live --config mm-v2.toml --gateway runs/gw.gw --takeover`. The old process detaches and the gateway cancels its orders; the new one attaches once it holds the lock, so the gateway never sees two attachments with the same engine name. Its books come from the gateway's copy and the venue connections stay up, so the pause is shorter than a direct restart's. The lock file must be on the gateway's host, which it is: an attachment shares memory with the gateway.

A standby behind a gateway waits cold and loads nothing before the lock: the reference data and the books are the gateway's, and an attachment would claim the running session's instruments.

## Venue sessions across the handoff

- Dead man's switches: one per connection (Deribit's cancel-on-disconnect) needs nothing. An account-wide one (Binance USDⓈ-M `countdownCancelAll`, per symbol) is stopped by the old process when it disconnects and armed by the new one when it connects; the lock puts the stop before the arm.
- User streams: Binance USDⓈ-M returns the account's active listenKey to whoever asks and FastMM never deletes it, so the new process reuses or renews it. Binance Spot subscribes the user stream per WebSocket API connection.
- Request weight: during step 1 both processes use the account's and the IP's request weight; the reference-data load is a few requests.
- A connector that changes account settings while loading reference data (Binance USDⓈ-M `leverage` and `one_way_mode`) does so in step 1, while the old session still trades. Change them in a handoff only when the old session's position allows it.

## Design: taking the orders over

A warm standby still cancels and requotes: the queue position of the old session's orders is lost, and the venue sees no quotes for the length of steps 4 to 7. Not implemented: handing the orders over instead.

The old session would freeze (stop quoting and sending, keep its orders), wait until no order of its own is in flight, and pass its state to the successor over the control socket: open orders (client order id, venue order id, instrument, side, price, leaves), positions, last execution id per venue, its epoch. The successor's OMS adopts the orders of that epoch where it now cancels unknown ones, as NautilusTrader's `external_order_claims` imports external orders of the instruments a strategy claims, and its quote manager maps them to quote slots or cancels them on the next requote. Venues cancel and replace by the original client order id from any connection of the account, so the adopted ids keep working.

The lock would pass with the state: the old process sends the locked descriptor over the socket (`SCM_RIGHTS`); a `flock` belongs to the open file, so the successor holds it the moment it receives it, and there is no instant with no holder or two. If the successor does not confirm within the timeout, the old process unfreezes and keeps trading with its own orders.
