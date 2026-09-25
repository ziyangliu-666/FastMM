# Run strategies behind a gateway

`fastmm-gateway` holds the venue connections. Strategy processes, `fastmm-live --gateway`, attach to it, several at once, each trading instruments no other attached strategy trades. A strategy can be stopped, crash or be replaced without the venue sessions dropping and without touching the others. When a strategy process goes away for any reason, `kill -9` included, the gateway cancels that strategy's orders at once.

```console
$ fastmm-gateway --config configs/sim-local.toml
$ fastmm-live --config configs/sim-local.toml --gateway runs/sim-local.gw
```

The gateway uses `[engine]` (`name`, `journal_dir`, `epoch_file`, `kill_file`, `spin_mode`, `net_cpus`, `net_backend`, the ring sizes), `[venues.*]`, `[[instruments]]` (every instrument any strategy trades) and `[gateway]`; it needs the API keys. A strategy uses everything else and needs no keys. Its `[[instruments]]` name what it trades there (venue and symbol); its venues, their ids and the instrument table (tick, lot, limits from the venue's reference data) come from the gateway. Give each strategy its own `[engine] name`, so each has its own store, journal and kill file.

## Attach

The socket is `<journal_dir>/<engine name>.gw` of the gateway's configuration, `AF_UNIX` `SOCK_SEQPACKET`, mode 0600 (`--socket` moves it). The gateway refuses an attach naming an instrument it does not have, or one a live attachment trades (`instrument BTCUSDT on venue 'sim' is traded by <engine> (pid, attachment, epoch)`), at most 16 at a time, and every attach while the account's kill switch is latched ([Account risk](#account-risk)). Otherwise it:

1. gives the strategy a session epoch from the gateway's `epoch_file` (the high 16 bits of every client order id, so ids are unique across strategies and across gateway restarts; the strategy's own `epoch_file` is not used),
2. creates three rings per venue under `/dev/shm` (`fastmm-gw-<name>-<pid>-<attachment>-<venue>.md`, `.ord`, `.out`) and adds them to the venue's routing,
3. asks every book for a fresh snapshot (nasdaq_itch cannot, so its books wait for the next resync),
4. reconciles: the account's executions since the strategy's last stored fill, then the open orders,
5. answers with the epoch, the instrument table, the ring paths and, as descriptors, the wake pages and its reactors' eventfds (see [Latency](#latency)).

The strategy restores its previous position from its own store, as `fastmm-live` does on a restart ([What survives a restart](running-in-production.md#1-what-survives-a-restart)); the execution replay in step 4 books what happened since. The attach request carries the positions it restored, which start the gateway's account. Instruments of other strategies stay in its table, disabled: their market data arrives, their quotes are pulled.

## Routing

On each venue's network thread:

- Market data goes to every attachment. A strategy that falls behind loses market data alone: its ring drops (the gateway logs the count), and once it has room again it gets a `Resyncing` state (its books clear, its quotes on that venue are pulled) and the books are snapshotted again.
- An order event goes to the strategy whose epoch its client order id carries. A fill of an epoch no attachment holds (a dead session's order, or an execution naming no order) goes to the strategy that trades the instrument, and so does an account-level position record; with none, the gateway logs it.
- A reconciliation goes to the strategies that asked for it (their attach, their engine's reconcile request), or to all when the connector started it. Each gets its own rows, under a `Begin` whose sent watermark is its own last order the venue had taken. A row of an epoch no attachment holds is a dead session's order, and the gateway cancels it.
- A replayed fill is routed like a streamed one. The replay one strategy's attach starts names the others' executions too; each books only those its engine has not seen (it deduplicates by execution id), which includes a fill its private stream missed. One naming no live order reaches the instrument's owner only if it is not older than the owner's own replay start and not among the trade ids its store listed: older ones are in its store already (the gateway logs and counts them).

## Account guards

Every order passes the gateway's network thread on its way to the connector. Before it goes on, `[gateway]` ([Configuration](../../reference/configuration.md#gateway)) checks, in this order:

- the instrument is one the sending strategy claimed,
- the account's kill switch ([Account risk](#account-risk)),
- `max_open_notional`: the notional of every order working at the venue, both sides, every strategy, including this one (a replace counts the difference),
- `max_gross_notional` and `max_net_notional`: the account's positions at the marks, over every venue, plus this order, as `[risk]` checks one strategy's; an order that reduces its instrument's position always passes, and so does one that brings a net already over the cap towards zero,
- `orders_per_sec` and `burst`: new orders and replaces of every strategy together, per venue (cancels always go).

A refused order goes back to the strategy that sent it as an `OrderReject` with `GatewayNotOwner`, `GatewayAccountKilled`, `GatewayOpenNotional`, `GatewayGrossNotional`, `GatewayNetNotional` or `GatewayRateLimit` ([Reject reasons](../../reference/errors.md#gateway)); its quote manager backs that side off as after any venue reject. An exposure refusal is per order; the other strategies trade on. Each strategy's own `[risk]` still applies to it.

## Account risk

The gateway keeps the account's positions: every execution that passes through it, streamed or replayed, whichever strategy it goes to (or none), booked once (by venue execution id, instrument and side, as a strategy's OMS books it; a base-asset commission changes the quantity as it does there), marked at the mid of the gateway's own copy of each book (applied after the strategies have been woken, so it is not on their way). Each venue's network thread books its own instruments; the totals are shared between the threads.

- **Where it starts.** A fresh gateway knows nothing of the account. The first strategy that attaches owning an instrument sends the position it restored from its store, and the account's position of that instrument starts there (flat when it restored nothing, or its venue cannot replay executions and the strategy starts flat too). A replayed execution older than that strategy's replay start, or among the trade ids its store listed, is in the position already and is not booked. From then on the account books everything itself.
- **Detach.** The position is the account's, not the process's: it stays when its strategy detaches, fills of its orders still in flight are booked into it, and the next strategy that owns the instrument finds it (the gateway logs what that strategy's store said).
- **`max_loss`.** The account's net PnL (realized plus unrealized minus fees, over every strategy, plus what earlier runs carried) at or below `-max_loss` trips the account's kill switch. The realized PnL and fees are carried in the gateway's kill file (`[engine] kill_file`, default `<journal_dir>/<name>.kill`, the format of [a strategy's](kill-switch-and-shutdown.md#the-latched-loss-budget)); unrealized PnL is measured again from the positions the strategies bring. Give the gateway an `[engine] name` of its own: with `max_loss` set it refuses a strategy with its name, whose kill file would be the same.
- **The trip.** Every network thread refuses new orders and replaces (`GatewayAccountKilled`), sends each attached strategy `TripVenueKill` (`GatewayMaxLoss`) for its venue, so its engine pulls its quotes, cancels its orders and, with every venue killed, trips its own kill switch (`on_kill`); the gateway cancels every order it knows, asks each venue for its open orders and cancels every row, cancels an acknowledgement that arrives later, and calls each venue's `cancel_all`. It then refuses every attach. The trip is latched in the kill file: a restart exits 6 until `fastmm-gateway --clear-kill` or the file is removed, which arms the whole budget again.

Once a second the gateway logs the account when it changed, and each position that changed:

```text
gateway: account net_pnl=-0.42600010 realized=0 unrealized=0.17352000 fees=0.59952010 carried=0 gross_exposure=240.00008000 net_exposure=240.00008000 kill=armed max_loss=3
gateway: account position sim:BTCUSDT 0.004
```

The limits are one number, so a gateway with any of them set refuses a table whose instruments settle in different currencies, as `fastmm-live` does for `[risk] max_loss`.

## Detach

Closing the connection is the detach, and the kernel closes it when the process dies. The gateway takes the strategy out of the routing, cancels every order of its epoch it knows to be working (one cancel each; the connectors' only other primitive is a venue-wide cancel-all, which would take every other strategy's quotes too), asks the venue for its open orders so that one it did not know is cancelled as a dead session's row, frees the instruments and removes the rings. An acknowledgement that arrives later for an order of the dead epoch is cancelled too. The other strategies keep trading.

A strategy that stops cleanly cancels its own quotes through the gateway first and logs `no venue cancel_all here`. A strategy whose gateway goes away stops with exit code 5. The gateway itself, on SIGINT or SIGTERM, detaches everyone and cancels all open orders on every venue.

## Latency

With `spin_mode = "adaptive"` an idle side blocks, and the other wakes it as threads wake each other inside `fastmm-live`: the engine sleeps on a futex in a page it shares with the gateway, and the gateway's network threads sleep in their reactors, whose flags (in a second page, shared by every attachment) and eventfds the strategy receives on attach. A wake-up costs a system call only while the other side sleeps. Each event and each order is copied once more: the connector's events from its own ring into each attachment's, the orders from the attachment's ring into the venue's.

Tick-to-trade against the simulator with one strategy attached (`scripts/bench-gateway.sh`, 45 s x 3 runs, WSL2, unpinned; the routing for several strategies and the account's checks and marks, `--account-limits`, left it unchanged), p50 in µs:

| `spin_mode` | engine, in-process | engine, gateway | wire, in-process | wire, gateway |
| --- | --- | --- | --- | --- |
| `adaptive` | 34.8 | 36.9 | 66.4 | 70.3 |
| `busy` | 7.2-7.7 | 7.9-9.2 | 41.0 | 43.0 |

## Not yet

- Two strategies on one instrument: the owner is per instrument, so a fill of an order no one holds and an account-level position have one strategy to go to.
- The gateway has no status file or control socket: its account is in its log.
- `[engine] threading = "single"`: the venue runs in the engine's thread, so it cannot attach.
- The strategy's status file shows the venue names but not their connection state; the gateway logs it every second.
