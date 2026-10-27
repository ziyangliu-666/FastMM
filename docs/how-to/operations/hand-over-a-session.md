# Hand over a running session

Replace a running `fastmm-live` with a new build or configuration on the same `[engine] name`, with never two processes trading the account. The old session cancels its orders and exits; the new one starts from its position, loss budget, client order ids and strategy state. Quoting pauses for one restart.

## 1. Turn on the instance lock

Start the running session with the lock on:

```toml
[engine]
name = "mm"
instance_lock = true          # one process of this engine name at a time
# lock_file = "/run/fastmm/mm.lock"   # default <journal_dir>/<name>.lock
```

The lock is an exclusive `flock(2)`. A session takes it before it touches the kill file, the store, the epoch file or a venue, and releases it after its shutdown has cancelled, flushed and closed them. A second start on the same name exits 8 and touches nothing:

```text
fastmm-live: process 41377 holds the instance lock runs/mm.lock and trades this engine's account. Take it over with --takeover, wait for it with --standby, or stop it.
```

The kernel drops the lock when the process dies, so a crash, `kill -9` or OOM kill never leaves it held. The file stays, holding the last holder's pid. `flock` is local to one host: two hosts sharing a `journal_dir` over NFS are not kept apart.

A session also refuses to start when another session without the lock answers on its control socket.

## 2. Start the new process

Take over at once:

```bash
fastmm-live --config mm-v2.toml --takeover
```

Or start a waiting process and choose the moment yourself:

```bash
fastmm-live --config mm-v2.toml --standby &
fastmm-ctl --name mm handoff
```

The new configuration may change anything except `[engine] name`, `journal_dir`, `lock_file` and the control socket path, which is how the two processes find each other. `--standby` and `--takeover` take the lock whatever `instance_lock` says. Python takes the same options: `python -m fastmm run ... --takeover` or `fastmm.run_live(..., takeover=True)`.

A `--standby` without `--takeover` waits for the lock however long it takes. Run it under a supervisor that does not also restart the running session.

## What happens

| Step | New process | Running process |
|---|---|---|
| 1 | Loads the configuration and the strategy, the venues' reference data and the account fees (read-only REST); finds the lock held | trades |
| 2 | Warm standby: takes the next session epoch, opens its journal, connects to the venues' market data only and starts its engine and strategy; every order is refused (`NotReconciled`) | trades |
| 3 | Once every book is synced (at most 30 s), `--takeover` sends `handoff` to the control socket; `--standby` waits for whoever stops the running session | answers `ok handing over` |
| 4 | keeps reading market data | `fastmm-live: shutting down (control socket: handoff)`: control socket closed, kill switch, cancel-all on every venue, engine and network threads stopped, journal trailer, `[strategy] state_file` written, store session closed, kill file written |
| 5 | | `instance lock <path> released`, exit 0 |
| 6 | Takes the lock (it polls every 10 ms): `standby: instance lock <path> taken over`. Reads the kill file, the strategy state and the store's positions, opens the store, the status file and the control socket, and opens each venue's private channels: `standby: took over after <n> ms warm` | |
| 7 | Replays the venue's executions since the store's last fill, reconciles, quotes: `every venue has reconciled its orders and executions since the start; orders are sent from now on` | |

The new process logs each stage:

```text
standby: instance lock <path> taken over
standby: took over after <n> ms warm
every venue has reconciled its orders and executions since the start; orders are sent from now on
```

Against the local simulator, a warm standby's first order comes about 35 ms after the `handoff`; a cold start's comes 100 to 200 ms after the old process exits. On a real venue the warm pause is the private-channel connect and the reconciliation.

A cold standby stops after loading, before it connects at all. After the lock it does everything a warm one does, plus the market-data connect and the book snapshots.

No order rests at the venue between the cancel-all and the new quotes. An old order the cancel-all missed carries the old session epoch, so the new session's start-up sweep cancels it (`cancelling unknown live order <id>`).

## Warm standby

A standby waits warm when all of these hold:

- it holds no lock at start;
- it connects to the venues itself, not through `--gateway`;
- it runs `threading = "split"`;
- every venue it trades can hold its private channels back and replay executions before it quotes. Binance Spot can ([Venue connectors](../../reference/venues.md)).

Otherwise it waits cold. A pool member opens nothing before the lock.

Until it takes the lock, a warm standby:

| Does | Does not |
|---|---|
| read market data, sync books and run the strategy's hooks, so the strategy warms up | send, cancel or query anything of the account: no order entry, user stream, execution replay or open-order sweep |
| refuse its own quotes quietly (`set_quotes` returns false, an order fails with `NotReconciled`), counting no reject | write the kill file, strategy state file, store, status file or control socket |
| write its own journal | |
| use the next session epoch, so its client order ids are its own | |
| read the kill file once, and exit 6 on a latched `max_loss` trip | |

At takeover the engine receives the PnL from the kill file and the strategy state from `[strategy] state_file`. `restore()` runs then, after the warm-up, so the strategy keeps what it learnt from market data. A strategy whose `restore()` replaces everything loses only the warm-up. A replay of the journal applies the carried PnL and restores nothing.

A standby that stops before it took over cancels nothing: the orders at the venue are the running session's.

## When a step fails

Until the running session accepts the handoff, every failure leaves it trading. After that, every failure ends with no process trading and no order resting.

| Failure | Result |
|---|---|
| The new process cannot load its configuration, strategy or reference data | It exits 3 or 4; the running session never hears of it. |
| The running session refuses `handoff`, has no control socket or does not answer within 5 s | `--takeover` exits 8; the running session trades on. |
| The old session's cancel-all fails | It exits 5 and releases the lock; the new session's start-up sweep cancels what is left. |
| The old session's shutdown hangs | The [shutdown watchdog](kill-switch-and-shutdown.md#shutdown-sequence) ends it 60 s after the stop; the new session starts and sweeps. |
| The lock is not free `[engine] handoff_timeout_ms` (75000 ms) after the handoff | `--takeover` exits 8. The old session is already stopping, so start again. |
| The new process fails after taking the lock | It exits with that code and releases the lock. Start the old or the new build again. |
| The running session crashes while a `--standby` waits | The standby takes the lock and starts as [after a crash](running-in-production.md#restarts). |
| `SIGINT` or `SIGTERM` to a waiting `--standby` | It exits 0 and cancels nothing. |
| The kill file holds a latched `max_loss` trip at takeover | It exits 6 before opening its private channels. |

## What carries over

| State | How |
|---|---|
| Position | the store and the venue's execution replay; the old session writes its last fill before releasing the lock |
| Kill switch and `[risk] max_loss` budget | `[engine] kill_file`, written last by the old session |
| Client order ids | the next epoch from `[engine] epoch_file`; the epoch is the high 16 bits of every id, so the two sessions' ids never meet |
| Strategy state | `state()` and `restore()` with `[strategy] state_file` ([Strategy API](../../reference/strategy-api.md#state-across-sessions)), in C++ and Python |
| `fastmm-ctl param`, `limits` and scoped pulls | not carried: put them in the new configuration |

The handoff cancels and requotes, so the old orders' queue positions are lost.

## Behind a gateway

A strategy attached to `fastmm-gateway` hands over the same way:

```bash
fastmm-live --config mm-v2.toml --gateway runs/gw.gw --takeover
```

The old process detaches and the gateway cancels its orders. The new one attaches once it holds the lock, so the gateway never sees two attachments with the same name. Books come from the gateway and its venue connections stay up, so the pause is shorter than a direct restart's.

A standby behind a gateway waits cold: the reference data and books are the gateway's, and an early attachment would claim the running session's instruments.

## Venue sessions across the handoff

| Venue feature | Across a handoff |
|---|---|
| Cancel-on-disconnect per connection (Deribit) | nothing to do |
| Account-wide dead man's switch (Binance USDⓈ-M `countdownCancelAll`) | the old process stops it, the new one arms it; the lock orders the two |
| User stream, Binance USDⓈ-M | the new process reuses or renews the account's listenKey, which FastMM never deletes |
| User stream, Binance Spot | one subscription per WebSocket API connection |
| Request weight | both processes use it while the new one loads reference data, a few requests |
| Account settings set at load (Binance USDⓈ-M `leverage`, `one_way_mode`) | changed while the old session still trades; change them only when its position allows |
