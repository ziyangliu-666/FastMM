# Journals, replay and PnL

## Journal files

With `[engine] journal = true` (the default), `fastmm-live` writes `<journal_dir>/<engine name>-<session id>.fmj`; `--journal <path>` chooses the file and `--no-journal` turns it off. The log names the file at startup (`journal: <path>`).

A journal holds the session header, the instrument table, the effective configuration (without API keys and secrets), each consumed event with the engine clock at which it was processed, and a copy of each order message the engine sent (marked `out`). Blocks carry CRC32C checksums, and a clean shutdown writes a trailer block. The current format is version 2 ([Journal format](../../reference/journal-format.md), [ADR 0010](../../adr/0010-fmj-journal-format.md)); the tools also read version 1.

Size: one-symbol Binance Demo sessions wrote 23 MB to 32 MB per hour.

### The session epoch

`[engine] epoch_file` (default `runs/session_epoch`) stores a counter that goes up by one for every session. Client order ids are the epoch in the upper 32 bits and a sequence number in the lower 32 bits, so ids stay unique across restarts. Keep the file between sessions that trade on the same account; the startup log shows the value (`epoch=22`).

## Read a journal

`tools/journal_dump.py` needs only Python 3:

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

## Check PnL

A session's PnL has three views:

1. The engine's, from the summary line `fastmm-live: realized_pnl=<r> unrealized_pnl=<u> fees=<f> ...` (also the final `fastmm-top` frame). Net PnL is `r + u - f`, marked at the engine's last mid.
2. The journal's, computed from the fills by `tools/pnl_report.py`.
3. The account's, from balance snapshots taken before and after the session.

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
