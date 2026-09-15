# 8. Trade on the simulated exchange

`fastmm-sim-exchange` is a local exchange that speaks the Binance Spot API.

## Start the exchange

`configs/tutorial-sim.toml` configures both processes: the exchange reads `[[instruments]]` and `[sim]`, the engine reads the rest. Its fault section:

```toml
[sim.faults]
drop_md_after_s = 15         # close every market-data WebSocket once, 15 s after start
```

Start the exchange in a second terminal, or in the background as here:

<!-- snippet: scripts/docs/tutorial.sh#sim-exchange -->
```bash
"$BIN"/fastmm-sim-exchange --config configs/tutorial-sim.toml --duration 90s > runs/tutorial/sim-exchange.log 2>&1 &
```

It listens on 127.0.0.1:9080 (plain) and 9443 (TLS). The [Simulated exchange](../../reference/sim-exchange.md) reference lists the faults.

## Trade for 40 seconds

<!-- snippet: scripts/docs/tutorial.sh#sim-live -->
```bash
export FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret
"$BIN"/tutorial-live --config configs/tutorial-sim.toml --duration 40s \
  --journal runs/tutorial/sim.fmj --log runs/tutorial/sim-live.log
```

- The key and secret are the simulator's; the configuration reads them from these variables ([Configuration](../../reference/configuration.md#general-rules)).
- `--duration 40s` stops the session like Ctrl-C ([Kill switch and shutdown](../../how-to/operations/kill-switch-and-shutdown.md)).
- `--journal` records the session; `--log` writes the log to a file and still prints warnings.

While it runs, `./build/release/bin/fastmm-top --name tutorial-sim` in another terminal shows orders, fills, PnL and latency ([Monitor a session](../../how-to/operations/monitor-with-fastmm-top.md)).

## Read the log

<!-- snippet: scripts/docs/tutorial.sh#sim-log -->
```bash
grep -E "first_mm: |fastmm-live: (events|realized_pnl|shutdown took)" runs/tutorial/sim-live.log
```

```text
INFO  first_mm: started, quoting enabled
INFO  first_mm: venue 0 channel 0 is live
INFO  first_mm: venue 0 channel 1 is live
INFO  first_mm: fills=10 late_fills=0 disconnects=0 net_pnl=0.10924483
INFO  first_mm: fills=15 late_fills=0 disconnects=0 net_pnl=0.20807468
WARN  first_mm: venue 0 channel 0 is Disconnected; quotes pulled
INFO  first_mm: venue 0 channel 0 is live
INFO  first_mm: fills=32 late_fills=0 disconnects=1 net_pnl=0.40847872
...
INFO  fastmm-live: events=4693 book_updates=402 orders=69 cancels=5 replaces=638 fills=64 risk_rejects=0 venue_rejects=0
INFO  fastmm-live: realized_pnl=1.24543742 unrealized_pnl=0.0472925 fees=0.3839993 tick_to_trade p50=94207 ns p99=172031 ns
INFO  fastmm-live: shutdown took 320 ms (cancel_all ok)
```

(Timestamps and thread ids removed.) Channel 0 is market data, channel 1 order entry. After the disconnect the connector reconnected within 250 ms, took a fresh snapshot, and the next `on_book` requoted. PnL and fees are in USDT.

On WSL2 and in virtual machines the terminal may also show `TSC recalibration stepped the engine clock` warnings: the host's wall clock jumped ([Troubleshooting](../../how-to/operations/troubleshooting.md)).

## Replay the live session

<!-- snippet: scripts/docs/tutorial.sh#sim-replay -->
```bash
"$BIN"/tutorial-replay --journal runs/tutorial/sim.fmj --verify
```

```text
journal  runs/tutorial/sim.fmj: format v3, 5458 messages (1926 market data, 712 outbound), rng_seed 42, strategy 'first_mm'
session  epoch 1, quoting enabled, cancel-replace venues 0x1, engine clock recorded
config   embedded in the journal (hash 543d5a2b3f8d5c29)
replay   strategy=first_mm events=4701
recorded outbound 712 msgs sha256 1116a9bdbd8604cfe3881af988501b44624a2ca9989c88dbcf009d2b11a08485
replayed outbound 712 msgs sha256 1116a9bdbd8604cfe3881af988501b44624a2ca9989c88dbcf009d2b11a08485
replay MATCH
```

The journal stores the order in which the engine consumed events and its clock at each one, so the replay reproduces the live session, disconnect included ([Determinism](../../explanation/determinism.md)).

Next: [9. Trade on Binance Demo](09-binance-demo.md)
