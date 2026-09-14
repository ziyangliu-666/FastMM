# Journals, replay and PnL

Every `fastmm-live` session can be recorded to a journal. This page shows how to read a journal,
how to replay it, and how to check a session's PnL against the venue account.

## Journal files

With `[engine] journal = true` (the default), `fastmm-live` writes
`<journal_dir>/<engine name>-<session id>.fmj`; `--journal <path>` chooses the file and
`--no-journal` turns it off. The log names the file at startup (`journal: <path>`).

A journal holds the session header (session id, start time, clock calibration, config hash, RNG
seed, strategy name), the instrument table, and then every event the engine consumed, in order,
plus a copy of every order message the engine sent (marked `out`). Blocks carry CRC32C checksums,
and a clean shutdown writes a trailer block. The format is described in
[ADR 0010](../../adr/0010-fmj-journal-format.md).

Size: our one-symbol Binance Demo sessions wrote 23 MB to 32 MB per hour.

### The session epoch

`[engine] epoch_file` (default `runs/session_epoch`) stores a counter that goes up by one for every
session. Client order ids are the epoch in the upper 32 bits and a sequence number in the lower 32
bits, so ids stay unique across restarts. Keep the file between sessions that trade on the same
account; the startup log shows the value (`epoch=22`).

## Read a journal

`tools/journal_dump.py` needs only Python 3:

```bash
python3 tools/journal_dump.py runs/demo-1/session.fmj --type OrderFill --first 3
```

It prints the header, the instrument table, the first matching events and a count of every event
type. A fill looks like this:

```text
#86      OrderFill         in  inst 0 venue 0 flags - exch_ts 1789356325085000000 recv_ts 1789356324721949775 venue_seq 0
          cl_ord_id=94489280513(epoch 22, seq 1) exec_id=300867841 px=77762.68 qty=0.0003 cum=0.0003 leaves=0 fee=0.0000003 fee_asset=base side=Buy liq=Maker
```

- `fee` is in units of `fee_asset`: `quote` (for example USDT), `base` (for example BTC) or `other`
  (for example BNB, which the engine cannot value and leaves out of fees and positions).
- `liq` is `Maker`, `Taker` or `Unknown`.
- `trailer MISSING` at the end means the process did not shut down cleanly; the events before the
  damaged block are still readable.
- `--no-crc` skips checksum verification, which is slow in pure Python on large journals.

## Replay

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic --out - --journal-out runs/bt/session.fmj
./build/release/bin/fastmm-replay --journal runs/bt/session.fmj --config configs/backtest-example.toml --verify
```

`fastmm-replay` runs the recorded inbound events through the same engine and strategy with a
simulated clock and compares every outbound order message, and their SHA-256, with the copies in
the journal. Pass the config the session ran with; a config with `${VAR}` references needs those
variables set, even though replay sends nothing. A journal without outbound copies (market data
only, such as `tests/fixtures/journals/sample_1000.fmj`) is backtested first, and with `--verify`
the run's outbound hash must match the `<journal>.sha256` sidecar or `--expect <sha256>`:

```bash
./build/release/bin/fastmm-replay --journal tests/fixtures/journals/sample_1000.fmj --verify
```

Exit codes: 0 match (or no verification requested), 1 mismatch, 2 bad command line, 3 unreadable
config or journal, or unknown strategy.

A mismatch means the engine did not make the same decisions from the same inputs. Check first that
the binary, strategy parameters and config are the ones of the recording (the header's config hash
identifies the config); if they are, it is a determinism bug, and the journal is the reproduction.

**Live journals do not replay to a match today.** A 20 s `fastmm-live` session against
`fastmm-sim-exchange` (`./scripts/run-sim.sh --duration 20s`) replayed with
`--config configs/sim-local.toml --verify` reported `replay MISMATCH` at the first outbound
message (311 recorded, 624 replayed). Use live journals for inspection and PnL; prove determinism
on backtest journals as above.

## Check PnL

There are three views of a session's PnL, and they should agree:

1. **The engine's**, from the summary line `fastmm-live: realized_pnl=<r> unrealized_pnl=<u>
   fees=<f> ...` (also the final `fastmm-top` frame). Net PnL is `r + u - f`, marked at the engine's
   last mid.
2. **The journal's**, computed from the fills by `tools/pnl_report.py`.
3. **The account's**, from balance snapshots taken before and after the session.

`tools/pnl_report.py` prints fills, maker share, volume, fees and inventory per hour, the journal's
trading PnL and, when given, the engine's summary and the account reconciliation:

```bash
python3 tools/pnl_report.py runs/demo-1/session.fmj --engine-log runs/demo-1/engine.log \
  --start runs/demo-1/equity_start.json --end runs/demo-1/equity_end.json
```

The snapshots are JSON objects that you produce from the venue's account and ticker endpoints
right before and right after the session (FastMM does not ship a tool for this, because it needs
keys and venue-specific endpoints). The balances are the free plus locked amounts:

```json
{"utc": "2026-09-14T03:25:21Z", "btc": 0.004995, "usdt": 4614.81995, "mid": 77762.685, "open_orders": 0}
```

The balance keys are `base` and `quote`, or the lower-case asset names given by `--base-asset` and
`--quote-asset` (default `BTC` and `USDT`). `equity` (or `equity_usdt`) is optional and computed as
`quote + base * mid` when missing. `python3 tools/pnl_report.py --help` lists every option, and
`python3 tools/pnl_report.py --self-test` checks the tool on a synthetic journal.

The account's equity change is split into what the starting inventory did on its own and what
trading did:

```text
equity change = starting base balance * (end mid - start mid) + trading
```

How to read the differences:

- **Balance change "unexplained"** (account balance change minus the journal's inventory or cash
  change) should be zero. A non-zero value means fills that are not in the journal (a second
  engine or manual trades on the account), deposits or withdrawals, or commission paid in another
  asset.
- **Journal trading PnL vs the account** should agree to within rounding.
- **Engine net vs the account** differs by the inventory marked at the engine's last mid instead of
  the snapshot mid; with a small inventory the difference is cents.

Commission charged in the base asset is already inside the inventory: a buy receives `qty - fee`
and a sell delivers `qty + fee`. Subtracting it from the PnL a second time understates the result
(an early version of the report did this and showed -48.74 USDT for the session below instead of
-32.94 USDT).

## A real example: Binance Demo

Two one-hour `basic_mm` sessions on BTCUSDT in Binance Demo Mode, with the same risk limits and
quote size (0.0003 BTC):

| Session | `half_spread_bps` | Fills in the hour | Account equity change | Of which trading |
|---|---:|---:|---:|---:|
| Quoting 15 bps from the mid | 15 | 0 | +2.11 USDT | 0.00 USDT |
| Quoting at the touch | 0 | 1640 (all maker) | -33.36 USDT | -32.94 USDT |

The first session never traded: its whole equity change was the revaluation of the starting 0.004995
BTC. The second session's report:

- Notional traded: 31,622.62 USDT; fees: 31.62 USDT, exactly 10.00 bps (the 0.1% maker commission).
  Commission was charged in BTC on buys (0.00020358 BTC in total) and in USDT on sells (15.82 USDT).
- Inventory change -0.00044358 BTC and cash change +1.5160 USDT, both after fees, matching the
  account's balance changes exactly.
- Account: equity change -33.36 USDT = starting inventory revaluation -0.42 USDT + trading
  -32.94 USDT.
- Engine: realised -1.32 USDT, unrealised +0.00 USDT, fees 31.62 USDT, net -32.94 USDT. The
  difference to the account's trading PnL was 0.003 USDT.
- The strategy had 260 post-only rejects (`PostOnlyWouldCross`), the normal cost of quoting at the
  touch.

Quoting at the touch captured almost no spread (-1.32 USDT before fees), and the 10 bps maker fee
turned that into a 32.94 USDT loss. A market maker on this fee tier needs a rebate or a much better
edge than the spread it quotes.

## See also

- [Monitoring a live session](../../monitoring.md)
- [Kill switch and shutdown](kill-switch-and-shutdown.md)
- [Go-live checklist](go-live-checklist.md)
