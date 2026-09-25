# Journals, replay and PnL

## Journal files

With `[engine] journal = true` (the default), `fastmm-live` writes `<journal_dir>/<engine name>-<session id>.fmj`; `--journal <path>` chooses the file and `--no-journal` turns it off. The log names the file at startup (`journal: <path>`).

A journal holds the session header, the instrument table, the effective configuration (without API keys and secrets), each consumed event with the engine clock at which it was processed, and a copy of each order message the engine sent (marked `out`). Blocks carry CRC32C checksums, and a clean shutdown writes a trailer block. The current format is version 3 ([Journal format](../../reference/journal-format.md), [ADR 0010](../../adr/0010-fmj-journal-format.md)); the tools also read versions 1 and 2.

Size: one-symbol Binance Demo sessions wrote 23 MB to 32 MB per hour. `[engine] journal_max_bytes` rolls the file over into numbered parts and `[engine] journal_retention_days` deletes old ones at start-up; `[engine] journal_sync` chooses how far a write is pushed before the writer moves on, and a journal that cannot be written trips the kill switch ([Journal format](../../reference/journal-format.md#durability)).

Next to the journal, `[storage]` writes the same session as rows: fills, orders, positions, PnL by day and kill events, queryable without a replay ([Query what you traded](query-trading-records.md)). The journal stays the authority; the store is the convenient view.

### The session epoch

`[engine] epoch_file` (default `runs/session_epoch`) stores a counter that goes up by one for every session. Client order ids are the epoch in the upper 32 bits and a sequence number in the lower 32 bits, so ids stay unique across restarts. Keep the file between sessions that trade on the same account; the startup log shows the value (`epoch=22`).

## Read a journal

`fastmm report runs/demo-1/session.fmj` writes the session as one HTML page — equity, inventory, fees, order and reject counts — beside the journal ([Run report](../../reference/run-report.md)). For the events themselves, `tools/journal_dump.py` needs only Python 3:

```bash
python3 tools/journal_dump.py runs/demo-1/session.fmj --type OrderFill --first 3
```

It prints the header, the instrument table, the first matching events and a count per event type. A fill:

```text
#86      OrderFill         in  inst 0 venue 0 flags - exch_ts 1789356325085000000 recv_ts 1789356324721949775 venue_seq 0
          cl_ord_id=94489280513(epoch 22, seq 1) exec_id=300867841 px=77762.68 qty=0.0003 cum=0.0003 leaves=0 fee=0.0000003 fee_asset=base side=Buy liq=Maker
```

- `fee` is in units of `fee_asset`: `quote` (for example USDT), `base` (for example BTC) or `other` (for example BNB, which the engine cannot value and leaves out of fees and positions).
- `liq` is `Maker`, `Taker` or `Unknown`.
- `trailer MISSING` at the end means the process did not shut down cleanly; the events before the damaged block are still readable.
- `--no-crc` skips checksum verification, which is slow in pure Python on large journals.

## Replay

A journal recorded by `fastmm-live` or by `fastmm-backtest --journal-out` replays on its own:

```bash
./build/release/bin/fastmm-replay --journal runs/sim-local-1789370000000000000.fmj --verify
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic --out - --journal-out runs/bt/session.fmj
./build/release/bin/fastmm-replay --journal runs/bt/session.fmj --verify
```

`fastmm-replay` runs the recorded inbound events through the same engine and strategy with a simulated clock set to the engine clock of the recording, and compares every outbound order message, and their SHA-256, with the copies in the journal. It takes the configuration embedded in the journal, and the session epoch, dry run, RNG seed and each venue's cancel-replace from the header, so it needs neither the config file nor API keys. A 20 s `fastmm-live` session against `fastmm-sim-exchange`:

```text
journal  /tmp/e2e/session.fmj: format v3, 2644 messages (850 market data, 320 outbound), rng_seed 42, strategy 'basic_mm'
session  epoch 23, quoting enabled, cancel-replace venues 0x1, engine clock recorded
config   embedded in the journal (hash fb8ab9d4318a634e)
replay   strategy=basic_mm events=2300
recorded outbound 320 msgs sha256 a9463030a4344f26d386ee4bf5ec1f53060895e9b925f6bda62b69b76ef9cd6a
replayed outbound 320 msgs sha256 a9463030a4344f26d386ee4bf5ec1f53060895e9b925f6bda62b69b76ef9cd6a
replay MATCH
```

`--config <file>` replays with that file instead. Its effective configuration (secrets and formatting do not count) is hashed and compared with the journal's config hash; if they differ, the run is a what-if replay and prints the warning below. `--strategy <name>` is a what-if run too.

A mismatch prints the first differing message as recorded and as replayed. The same session with `half_spread_bps = 6.0` instead of `5.0`:

```text
fastmm-replay: warning: sim-local-whatif.toml is not the configuration this session was recorded with (effective config hash cc9c8e5451e9b57f, journal fb8ab9d4318a634e); this is a what-if replay and is not expected to match. Omit --config to replay with the recorded configuration.
...
first mismatching outbound message: #0
  expected: OutNewOrder venue=0 inst=0 cl_ord_id=98784247809 (epoch 23, seq 1) Buy PostOnly GTC px=59970 qty=0.001 recv_ts=1789376211743451228
  actual:   OutNewOrder venue=0 inst=0 cl_ord_id=98784247809 (epoch 23, seq 1) Buy PostOnly GTC px=59964 qty=0.001 recv_ts=1789376211743451228
replay MISMATCH
```

Only the first mismatch means anything: replay feeds the recorded acknowledgements whatever it sent, so everything after it diverges too.

Journals written before format version 2 carry no configuration, session settings or engine clock. Replay them with `--config <the config the session ran with>`; a live one of those does not replay to a match. A journal without outbound copies (market data only, such as `tests/fixtures/journals/sample_1000.fmj`) is backtested first, with `configs/backtest-example.toml` unless `--config` is given, and with `--verify` the run's outbound hash must match the `<journal>.sha256` sidecar or `--expect <sha256>`:

```bash
./build/release/bin/fastmm-replay --journal tests/fixtures/journals/sample_1000.fmj --verify
```

Exit codes: [Command lines](../../reference/cli.md#fastmm-replay).

A mismatch with the embedded configuration and the same binary is a determinism bug, and the journal reproduces it ([Determinism](../../explanation/determinism.md)). A different binary may not match.

## Check the fill model against live fills

`fastmm-data fill-check` measures how well `[backtest] fill_model = "l2_queue"` predicts the passive fills of a live session. It does not re-run the strategy. It takes the orders the session had resting, each from its ack until its cancel ack, last fill or expiry, puts each one behind the quantity displayed at its price when the ack arrived, and feeds the queue model the journal's book deltas and trades the way a backtest does. A replace follows the new order id. Orders that crossed the book at the ack, market, IOC and FOK orders are left out.

```bash
./build/release/bin/fastmm-data fill-check runs/demo-1/session.fmj --csv runs/demo-1/fill-check.csv
```

The one-hour Binance Demo session quoting at the touch ([Example](#example-binance-demo)):

```text
orders   2602 sent, 2347 resting after the ack; left out: 255 rejected, 0 not acked, 0 ended before the ack, 0 market/IOC/FOK, 0 crossing the book at the ack
market   32598 book and trade messages
live     1509 filled, qty 0.4074

conservatism  filled   both live only model only neither model/live    model qty  |dt| p50
0.00             221    216      1293          5     833      0.143   0.05812644     0.9ms
0.50             201    197      1312          4     834      0.130   0.05311345     0.9ms
1.00             197    193      1316          4     834      0.128      0.05217     0.9ms
```

Each row is one `queue_conservatism` value (`--conservatism 0,0.25,0.5` to choose others). `both`, `live only`, `model only` and `neither` count orders by whether they filled live and whether the model filled them. `model/live` is the ratio of filled quantities. `|dt| p50` is the median gap between the first model fill and the first live fill, for orders that filled both ways. Pick the conservatism whose `model/live` is closest to 1 and whose `live only` and `model only` are smallest.

The model predicted 13% of the quantity this session filled, whatever the conservatism. Binance Demo fills passive orders ahead of the quantity it displays: the session's first buy, 0.0003 BTC at 77762.68, acked behind 1.65 BTC, filled after 0.009 BTC had traded at that price, with 1.43 BTC still displayed there. A Demo session cannot calibrate the model; use a session on the real market.

Times are receive times (`recv_ts`). The quantity displayed at the ack may already include the order itself if the venue published it first. `--csv` writes one row per order: price, size, the quantity ahead at the ack, resting time, how it ended, and the live and model fills with their times.

## Check PnL

A session's PnL has four views:

1. The engine's, from the summary line `fastmm-live: realized_pnl=<r> unrealized_pnl=<u> fees=<f> ...` (also the final `fastmm-top` frame). Net PnL is `r + u - f`, marked at the engine's last mid.
2. The journal's, computed from the fills by `tools/pnl_report.py`.
3. The store's, from `fastmm-pnl` or `fastmm.open_store()`: the same fills, indexed by day and instrument across sessions ([Query what you traded](query-trading-records.md)).
4. The account's, from balance snapshots taken before and after the session.

`tools/pnl_report.py` prints fills, maker share, volume, fees and inventory per hour, post-fill markouts, the journal's trading PnL and, when given, the engine's summary and the account reconciliation:

```bash
python3 tools/pnl_report.py runs/demo-1/session.fmj --engine-log runs/demo-1/engine.log \
  --start runs/demo-1/equity_start.json --end runs/demo-1/equity_end.json
```

The snapshots are JSON objects that you produce from the venue's account and ticker endpoints right before and right after the session (FastMM ships no tool for this). The balances are the free plus locked amounts:

```json
{"utc": "2026-09-14T03:25:21Z", "btc": 0.004995, "usdt": 4614.81995, "mid": 77762.685, "open_orders": 0}
```

The balance keys are `base` and `quote`, or the lower-case asset names given by `--base-asset` and `--quote-asset` (default `BTC` and `USDT`). `equity` (or `equity_usdt`) is optional and computed as `quote + base * mid` when missing. `python3 tools/pnl_report.py --self-test` checks the tool on a synthetic journal.

The markout table marks each fill against the mid `--markout-horizons` seconds later (default `1,10,60`), taken from the journal's `BookTicker` events; a journal without top-of-book updates has no mid to mark against and the report says so. A fill whose horizon falls after the last quote in the journal is excluded, not marked at the last known mid. The spread capture next to it is over the same fills, so the difference is the adverse selection ([Backtesting](../../explanation/backtesting.md#markouts)).

```text
equity change = starting base balance * (end mid - start mid) + trading
```

The session reconciles when:

- the unexplained balance change (account balance change minus the journal's inventory or cash change) is zero. A non-zero value means fills that are not in the journal (a second engine or manual trades on the account), deposits or withdrawals, or commission paid in another asset;
- the journal's trading PnL equals the account's trading PnL to within rounding;
- the engine's net PnL differs from the account's trading PnL only by the engine's inventory marked at its last mid instead of the end snapshot's mid.

Commission charged in the base asset is already inside the inventory: a buy receives `qty - fee` and a sell delivers `qty + fee`. Do not subtract it from the PnL a second time.

## Example: Binance Demo

Two one-hour `basic_mm` sessions on BTCUSDT in Binance Demo Mode, with the same risk limits and quote size (0.0003 BTC):

| Session | `half_spread_bps` | Fills in the hour | Account equity change | Of which trading |
|---|---:|---:|---:|---:|
| Quoting 15 bps from the mid | 15 | 0 | +2.11 USDT | 0.00 USDT |
| Quoting at the touch | 0 | 1640 (all maker) | -33.36 USDT | -32.94 USDT |

The second session's report:

- Notional traded: 31,622.62 USDT; fees: 31.62 USDT, 10.00 bps (the 0.1% maker commission). Commission was charged in BTC on buys (0.00020358 BTC in total) and in USDT on sells (15.82 USDT).
- Inventory change -0.00044358 BTC and cash change +1.5160 USDT, both after fees, equal to the account's balance changes.
- Account: equity change -33.36 USDT = starting inventory revaluation -0.42 USDT + trading -32.94 USDT.
- Engine: realised -1.32 USDT, unrealised +0.00 USDT, fees 31.62 USDT, net -32.94 USDT, 0.003 USDT from the account's trading PnL.
- Post-only rejects (`PostOnlyWouldCross`): 260.
