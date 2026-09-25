# Run strategies behind a gateway

`fastmm-gateway` holds the venue connections. Strategy processes, `fastmm-live --gateway`, attach to it, several at once, each trading instruments no other attached strategy trades. A strategy can be stopped, crash or be replaced without the venue sessions dropping and without touching the others. When a strategy process goes away for any reason, `kill -9` included, the gateway cancels that strategy's orders at once.

```console
$ fastmm-gateway --config configs/sim-local.toml
$ fastmm-live --config configs/sim-local.toml --gateway runs/sim-local.gw
```

The gateway uses `[engine]` (`name`, `journal_dir`, `epoch_file`, `spin_mode`, `net_cpus`, `net_backend`, the ring sizes), `[venues.*]`, `[[instruments]]` (every instrument any strategy trades) and `[gateway]`; it needs the API keys. A strategy uses everything else and needs no keys. Its `[[instruments]]` name what it trades there (venue and symbol); its venues, their ids and the instrument table (tick, lot, limits from the venue's reference data) come from the gateway. Give each strategy its own `[engine] name`, so each has its own store, journal and kill file.

## Attach

The socket is `<journal_dir>/<engine name>.gw` of the gateway's configuration, `AF_UNIX` `SOCK_SEQPACKET`, mode 0600 (`--socket` moves it). The gateway refuses an attach naming an instrument it does not have, or one a live attachment trades (`instrument BTCUSDT on venue 'sim' is traded by <engine> (pid, attachment, epoch)`), at most 16 at a time. Otherwise it:

1. gives the strategy a session epoch from the gateway's `epoch_file` (the high 16 bits of every client order id, so ids are unique across strategies and across gateway restarts; the strategy's own `epoch_file` is not used),
2. creates three rings per venue under `/dev/shm` (`fastmm-gw-<name>-<pid>-<attachment>-<venue>.md`, `.ord`, `.out`) and adds them to the venue's routing,
3. asks every book for a fresh snapshot (nasdaq_itch cannot, so its books wait for the next resync),
4. reconciles: the account's executions since the strategy's last stored fill, then the open orders,
5. answers with the epoch, the instrument table, the ring paths and, as descriptors, the wake pages and its reactors' eventfds (see [Latency](#latency)).

The strategy restores its previous position from its own store, as `fastmm-live` does on a restart ([What survives a restart](running-in-production.md#1-what-survives-a-restart)); the execution replay in step 4 books what happened since. Instruments of other strategies stay in its table, disabled: their market data arrives, their quotes are pulled.

## Routing

On each venue's network thread:

- Market data goes to every attachment. A strategy that falls behind loses market data alone: its ring drops (the gateway logs the count), and once it has room again it gets a `Resyncing` state (its books clear, its quotes on that venue are pulled) and the books are snapshotted again.
- An order event goes to the strategy whose epoch its client order id carries. A fill of an epoch no attachment holds (a dead session's order, or an execution naming no order) goes to the strategy that trades the instrument, and so does an account-level position record; with none, the gateway logs it.
- A reconciliation goes to the strategies that asked for it (their attach, their engine's reconcile request), or to all when the connector started it. Each gets its own rows, under a `Begin` whose sent watermark is its own last order the venue had taken. A row of an epoch no attachment holds is a dead session's order, and the gateway cancels it.
- While the execution replay a strategy's attach started runs, replayed fills go to that strategy only: the others booked theirs already, some of it longer ago than their engine's dedupe window.

## Account guards

Every order passes the gateway's network thread on its way to the connector. Before it goes on, `[gateway]` ([Configuration](../../reference/configuration.md#gateway)) checks, per venue:

- the instrument is one the sending strategy claimed,
- `orders_per_sec` and `burst`: new orders and replaces of every strategy together (cancels always go),
- `max_open_notional`: the notional of every order working at the venue, both sides, every strategy, including this one (a replace counts the difference).

A refused order goes back to the strategy that sent it as an `OrderReject` with `GatewayNotOwner`, `GatewayRateLimit` or `GatewayOpenNotional` ([Reject reasons](../../reference/errors.md#gateway)); its quote manager backs that side off as after any venue reject. These are guards on the account, not a second risk engine: positions, loss and the per-strategy limits stay with each strategy's `[risk]`. The gateway does not know the account's position (it starts with whatever the venue holds), so it limits what it can see, the orders working.

## Detach

Closing the connection is the detach, and the kernel closes it when the process dies. The gateway takes the strategy out of the routing, cancels every order of its epoch it knows to be working (one cancel each; the connectors' only other primitive is a venue-wide cancel-all, which would take every other strategy's quotes too), asks the venue for its open orders so that one it did not know is cancelled as a dead session's row, frees the instruments and removes the rings. An acknowledgement that arrives later for an order of the dead epoch is cancelled too. The other strategies keep trading.

A strategy that stops cleanly cancels its own quotes through the gateway first and logs `no venue cancel_all here`. A strategy whose gateway goes away stops with exit code 5. The gateway itself, on SIGINT or SIGTERM, detaches everyone and cancels all open orders on every venue.

## Latency

With `spin_mode = "adaptive"` an idle side blocks, and the other wakes it as threads wake each other inside `fastmm-live`: the engine sleeps on a futex in a page it shares with the gateway, and the gateway's network threads sleep in their reactors, whose flags (in a second page, shared by every attachment) and eventfds the strategy receives on attach. A wake-up costs a system call only while the other side sleeps. Each event and each order is copied once more: the connector's events from its own ring into each attachment's, the orders from the attachment's ring into the venue's.

Tick-to-trade against the simulator with one strategy attached (`scripts/bench-gateway.sh`, 45 s x 3 runs, WSL2, unpinned; the routing for several strategies left it unchanged), p50 in µs:

| `spin_mode` | engine, in-process | engine, gateway | wire, in-process | wire, gateway |
| --- | --- | --- | --- | --- |
| `adaptive` | 34.8 | 36.9 | 66.4 | 70.3 |
| `busy` | 7.2-7.7 | 7.9-9.2 | 41.0 | 43.0 |

## Not yet

- Two strategies on one instrument: the owner is per instrument, so a fill of an order no one holds and an account-level position have one strategy to go to.
- Positions and loss across strategies in the gateway: each strategy's `[risk]` limits its own.
- `[engine] threading = "single"`: the venue runs in the engine's thread, so it cannot attach.
- The strategy's status file shows the venue names but not their connection state; the gateway logs it every second.
