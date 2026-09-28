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
INFO  first_mm: started, quoting off
INFO  first_mm: venue 0 channel 0 is live
INFO  first_mm: venue 0 channel 1 is live
INFO  first_mm: venue 0 channel 1 is live
INFO  first_mm: quoting on
INFO  first_mm: fills=11 late_fills=0 disconnects=0 net_pnl=0.16228803
INFO  first_mm: fills=17 late_fills=0 disconnects=0 net_pnl=0.22196902
WARN  first_mm: venue 0 channel 0 is Disconnected; quotes pulled
INFO  first_mm: venue 0 channel 0 is live
INFO  first_mm: fills=22 late_fills=0 disconnects=1 net_pnl=0.29124105
...
INFO  first_mm: quoting off
INFO  fastmm-live: events=4687 book_updates=402 orders=56 cancels=3 replaces=674 fills=55 risk_rejects=0 venue_rejects=0
INFO  fastmm-live: realized_pnl=1.05586083 unrealized_pnl=0.01321016 fees=0.32585832 tick_to_trade p50=36863 ns p99=61439 ns
INFO  fastmm-live: shutdown took 316 ms (cancel_all ok)
```

(Timestamps and thread ids removed.) Channel 0 is market data; channel 1 is order entry and the user-data stream, which report separately. After the disconnect the connector reconnected within 250 ms, took a fresh snapshot, and the next `on_book` requoted. PnL and fees are in USDT.

On WSL2 and in virtual machines the terminal may also show `TSC recalibration stepped the engine clock` warnings: the host's wall clock jumped ([Troubleshooting](../../how-to/operations/troubleshooting.md)).

The session journal also renders as a page: `python3 tools/report.py runs/tutorial/sim.fmj` writes `runs/tutorial/report.html` with the equity, the inventory and the counts of the session ([Run report](../../reference/run-report.md)).

## Replay the live session

<!-- snippet: scripts/docs/tutorial.sh#sim-replay -->
```bash
"$BIN"/tutorial-replay --journal runs/tutorial/sim.fmj --verify
```

```text
journal  runs/tutorial/sim.fmj: format v3, 5473 messages (1810 market data, 733 outbound), rng_seed 42, strategy 'first_mm'
session  epoch 1, quoting enabled, cancel-replace venues 0x1, engine clock recorded
config   embedded in the journal (hash 03cb0711746f2516)
replay   strategy=first_mm events=4695
recorded outbound 733 msgs sha256 e265b576737c9773cba10dd22e69e3829cb062a0e0e913b63261901ab10e37d4
replayed outbound 733 msgs sha256 e265b576737c9773cba10dd22e69e3829cb062a0e0e913b63261901ab10e37d4
replay MATCH
```

The journal stores the order in which the engine consumed events and its clock at each one, so the replay reproduces the live session, disconnect included ([Determinism](../../explanation/determinism.md)). A live session's counts and hashes differ from run to run.

Next: [9. Trade on Binance Demo](09-binance-demo.md)
