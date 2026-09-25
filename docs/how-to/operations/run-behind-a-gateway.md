# Run a strategy behind a gateway

`fastmm-gateway` holds the venue connections. A strategy process, `fastmm-live --gateway`, attaches to it, trades through shared-memory rings and can be stopped, crash or be replaced without the venue sessions dropping. When the strategy process goes away for any reason, `kill -9` included, the gateway cancels every open order at once.

```console
$ fastmm-gateway --config configs/sim-local.toml
$ fastmm-live --config configs/sim-local.toml --gateway runs/sim-local.gw
```

Both read the same configuration. The gateway uses `[engine]` (`name`, `journal_dir`, `spin_mode`, `net_cpus`, `net_backend`, the ring sizes), `[venues.*]` and `[[instruments]]`; it needs the API keys. The strategy uses everything else and needs no keys: its venues, their ids and the instrument table (tick, lot, limits from the venue's reference data) come from the gateway.

## Attach

The socket is `<journal_dir>/<engine name>.gw`, `AF_UNIX` `SOCK_SEQPACKET`, mode 0600 (`--socket` moves it). On attach the gateway:

1. creates three rings per venue under `/dev/shm` (`fastmm-gw-<name>-<pid>-<attachment>-<venue>.md`, `.ord`, `.out`),
2. points the venue's market data and order events at them,
3. asks every book for a fresh snapshot (nasdaq_itch cannot, so its books wait for the next resync),
4. reconciles: the account's executions since the strategy's last stored fill, then the open orders,
5. answers with the instrument table, the ring paths and, as descriptors, a wake page and its reactors' eventfds (see [Latency](#latency)).

The strategy restores its previous position from its own store, as `fastmm-live` does on a restart ([What survives a restart](running-in-production.md#1-what-survives-a-restart)); the execution replay in step 4 books what happened since. One strategy at a time: a second attach is refused while one is attached.

## Detach

Closing the connection is the detach, and the kernel closes it when the process dies. The gateway then points the sinks at a ring it discards (the counts are in its log), cancels all open orders on every venue, removes the rings and waits for the next strategy.

A strategy that stops cleanly cancels its own quotes through the gateway first and logs `no venue cancel_all here`. A strategy whose gateway goes away stops with exit code 5.

## Latency

With `spin_mode = "adaptive"` an idle side blocks, and the other wakes it as threads wake each other inside `fastmm-live`: the engine sleeps on a futex in a page it shares with the gateway, and the gateway's network threads sleep in their reactors, whose eventfds the strategy receives on attach. A wake-up costs a system call only while the other side sleeps. Each order is copied once more, from the shared ring into the venue's own ring.

Tick-to-trade against the simulator (`scripts/bench-gateway.sh`, 45 s x 3 runs, WSL2, unpinned), p50 in µs:

| `spin_mode` | engine, in-process | engine, gateway | wire, in-process | wire, gateway |
| --- | --- | --- | --- | --- |
| `adaptive` | 34.8 | 36.9 | 66.4 | 70.3 |
| `busy` | 7.2-7.7 | 7.9-9.2 | 41.0 | 43.0 |

## Not yet

- Several strategies on one gateway, account-level risk in the gateway.
- `[engine] threading = "single"`: the venue runs in the engine's thread, so it cannot attach.
- The strategy's status file shows the venue names but not their connection state; the gateway logs it every second.
