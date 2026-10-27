# Journals, replay and PnL

Every live session writes a journal; from it you can replay the session, value its fills, check the backtest's fill model and reconcile PnL with the account.

| Task | Command |
|---|---|
| read the events | `python3 tools/journal_dump.py <journal>` |
| see the session as a page | `fastmm report <journal>` |
| reproduce the session | `fastmm-replay --journal <journal> --verify` |
| value the fills | `fastmm-data analyze <journal>` |
| check the fill model | `fastmm-data fill-check <journal>` |
| fit the backtest to live sessions | `fastmm-data calibrate <journals> --backtest` |
| line a backtest up with the session | `fastmm-data events <journal>` |
| reconcile PnL with the account | `python3 tools/pnl_report.py <journal>` |

## Journal files

`fastmm-live` writes `<journal_dir>/<engine name>-<session id>.fmj` and names it at start-up (`journal: <path>`). `--journal <path>` chooses the file and `--no-journal` turns it off.

A journal holds:

- the session header, the instrument table and the effective configuration, without keys and secrets;
- each event the engine consumed, with the engine clock when it was processed;
- a copy of each order message the engine sent, marked `out`.

Blocks carry CRC32C checksums, and a clean shutdown writes a trailer ([Journal format](../../reference/journal-format.md)).

| Key in `[engine]` | Effect |
|---|---|
| `journal` | on by default |
| `journal_max_bytes` | rolls the file over into numbered parts |
| `journal_retention_days` | deletes old journals at start-up |
| `journal_sync` | how far a write is pushed before the writer moves on ([Durability](../../reference/journal-format.md#durability)) |

A one-symbol Binance Demo session writes 23 to 32 MB per hour. A journal that cannot be written trips the kill switch.

`[storage]` writes the same session as rows for queries without a replay ([Query what you traded](query-trading-records.md)). Where the two disagree, the journal is right.

### The session epoch

`[engine] epoch_file` (default `runs/session_epoch`) holds a counter that goes up by one each session. Client order ids carry the epoch in their upper 32 bits, so they stay unique across restarts. Keep the file between sessions on the same account; the start-up log shows the value (`epoch=22`).

## Read a journal

`fastmm report <journal>` writes the session as one HTML page beside the journal: equity, inventory, fees, order and reject counts ([Run report](../../reference/run-report.md)). For the events, use `tools/journal_dump.py`, which needs only Python 3:

```bash
python3 tools/journal_dump.py runs/demo-1/session.fmj --type OrderFill --first 3
```

It prints the header, the instrument table, the first matching events and a count per type. A fill:

```text
#86      OrderFill         in  inst 0 venue 0 flags - exch_ts 1789356325085000000 recv_ts 1789356324721949775 venue_seq 0
          cl_ord_id=94489280513(epoch 22, seq 1) exec_id=300867841 px=77762.68 qty=0.0003 cum=0.0003 leaves=0 fee=0.0000003 fee_asset=base side=Buy liq=Maker
```

| Field or line | Meaning |
|---|---|
| `fee_asset` | `quote` (USDT), `base` (BTC) or `other` (BNB, which the engine cannot value and leaves out) |
| `liq` | `Maker`, `Taker` or `Unknown` |
| `trailer MISSING` | the process did not shut down cleanly; events before the damaged block are readable |

`--no-crc` skips checksum verification, which is slow in Python on large journals.

## Replay

1. Replay a journal from `fastmm-live`, or one a backtest wrote with `--journal-out`:

    ```bash
    ./build/release/bin/fastmm-replay --journal runs/sim-local-1789370000000000000.fmj --verify
    ```

    ```text
    journal  /tmp/e2e/session.fmj: format v5, 2644 messages (850 market data, 320 outbound), rng_seed 42, strategy 'basic_mm'
    session  epoch 23, quoting enabled, cancel-replace venues 0x1, engine clock recorded
    config   embedded in the journal (hash fb8ab9d4318a634e)
    replay   strategy=basic_mm events=2300
    recorded outbound 320 msgs sha256 a9463030a4344f26d386ee4bf5ec1f53060895e9b925f6bda62b69b76ef9cd6a
    replayed outbound 320 msgs sha256 a9463030a4344f26d386ee4bf5ec1f53060895e9b925f6bda62b69b76ef9cd6a
    replay MATCH
    ```

2. For a backtest, write the journal first:

    ```bash
    ./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic \
        --out - --journal-out runs/bt/session.fmj
    ./build/release/bin/fastmm-replay --journal runs/bt/session.fmj --verify
    ```

The replay runs the recorded inbound events through the same engine and strategy on the recorded engine clock. It compares every outbound message, and their SHA-256, with the copies in the journal. The configuration, epoch, dry run, RNG seed and cancel-replace settings come from the journal, so it needs no config file and no keys.

With the embedded configuration and the same binary, a mismatch is a determinism bug, and the journal reproduces it ([Determinism](../../explanation/determinism.md)). A different binary need not match.

### What-if replays

`--config <file>` or `--strategy <name>` replays with something else. A configuration whose hash differs from the journal's prints a warning; secrets and formatting do not count. The same session with `half_spread_bps = 6.0` instead of `5.0`:

```text
fastmm-replay: warning: sim-local-whatif.toml is not the configuration this session was recorded with (effective config hash cc9c8e5451e9b57f, journal fb8ab9d4318a634e); this is a what-if replay and is not expected to match. Omit --config to replay with the recorded configuration.
...
first mismatching outbound message: #0
  expected: OutNewOrder venue=0 inst=0 cl_ord_id=98784247809 (epoch 23, seq 1) Buy PostOnly GTC px=59970 qty=0.001 recv_ts=1789376211743451228
  actual:   OutNewOrder venue=0 inst=0 cl_ord_id=98784247809 (epoch 23, seq 1) Buy PostOnly GTC px=59964 qty=0.001 recv_ts=1789376211743451228
replay MISMATCH
```

Only the first mismatch counts: the replay feeds the recorded acknowledgements whatever it sent, so everything after it diverges.

### Other journals

| Journal | How it replays |
|---|---|
| no trailer | refused unless `--allow-incomplete`; then the outbound comparison proves nothing |
| format before version 2 | no embedded configuration; pass `--config`, and a live session will not match |
| market data only, such as `tests/fixtures/journals/sample_1000.fmj` | backtested with `configs/backtest-example.toml` or `--config`; `--verify` checks the hash against `<journal>.sha256` or `--expect <sha256>` |

### Check before replaying

`--check` says whether a replay can run, and with what, without running it:

```bash
./build/release/bin/fastmm-replay --journal tests/fixtures/journals/sample_1000.fmj --check
```

```text
journal        ok   market-data journal, format v1, 1000 messages
complete       ok   closed by its writer
config         ok   configs/backtest-example.toml (default for a market-data journal)
strategy       ok   basic_mm
params         ok   8 set
expected_hash  ok   tests/fixtures/journals/sample_1000.sha256
ready to replay
```

A `warn` line runs but proves less: a what-if configuration, no engine clock, or an unclosed journal. A `fail` line stops the replay with exit code 3 ([Command lines](../../reference/cli.md#fastmm-replay)). `--json` prints `{"ready": ..., "checks": [{"check", "status", "detail"}, ...]}` for a script.

## What the fills were worth

`fastmm-data analyze` marks each fill of a live session against the venue mid the session recorded. It reads the journal alone, with no store, configuration or replay.

```bash
./build/release/bin/fastmm-data analyze runs/mm1-1789370000000000000.fmj
./build/release/bin/fastmm-data analyze runs/mm1-1789370000000000000.fmj --json --horizons-ms 1000,10000,60000
```

```text
session  strategy 'basic_mm', 1 s of venue time
orders   2 sent, 1 cancels, 0 replaces, 0 refused by the venue
fills    1 (1 maker, 100.0%), 1 repeated report(s) counted once

instrument       fills   maker         bought           sold         notional
BTCUSDT              1       1              1              0           100.00

horizon  fills of         marked  capture_bps  markout_bps  adverse_bps  excluded
1s       all                  1         1.00        -1.00         2.00         0
10s      all                  0         0.00         0.00         0.00         1
```

| Column | Meaning, in bps of traded notional |
|---|---|
| `capture_bps` | the fill price against the mid at the fill |
| `markout_bps` | what the fill was worth at the mid after the horizon |
| `adverse_bps` | the difference |
| `excluded` | fills with no mid at the fill or after the horizon, never marked at a stale price |

An execution reported twice, as after a reconnect, counts once. A session without a BookTicker stream has counts and no markouts. The definitions match a backtest's markouts, so the two compare directly ([Backtesting](../../explanation/backtesting.md)).

## Check the fill model against live fills

`fastmm-data fill-check` measures how well `[backtest] fill_model = "l2_queue"` predicts a live session's passive fills. It replays the session's resting orders through the queue model, each from its ack to its end, behind the quantity displayed at its price.

```bash
./build/release/bin/fastmm-data fill-check runs/demo-1/session.fmj \
    --csv runs/demo-1/fill-check.csv
```

The one-hour Binance Demo session quoting at the touch ([Example](#example-binance-demo)):

```text
orders   2602 sent, 2347 resting after the ack; left out: 255 rejected, 0 not acked, 0 ended before the ack, 0 market/IOC/FOK, 0 crossing the book at the ack
market   32598 book and trade messages, ...
times    venue (order times in ms); from receive time: 0 order events, 1 market messages; ties in the ack / end millisecond: 2 / 0; own orders in depth: yes
live     1509 filled, qty 0.4074

conservatism  filled   both live only model only neither model/live    model qty  |dt| p50
0.00             546    542       967          4     834      0.393    0.1602879     0.0ms
0.50             526    523       986          3     835      0.379      0.15439     0.0ms
1.00             522    519       990          3     835      0.376      0.15319     0.0ms
```

Each row is one `queue_conservatism`; `--conservatism 0,0.25,0.5` picks others. Choose the row whose `model/live` is closest to 1 and whose `live only` and `model only` are smallest.

| Column | Meaning |
|---|---|
| `both`, `live only`, `model only`, `neither` | orders by whether they filled live and in the model |
| `model/live` | ratio of filled quantities |
| `\|dt\| p50` | median gap between the first model fill and the first live fill |

Here the model predicted 38 to 39% of the filled quantity at any conservatism. Binance Demo fills passive orders ahead of the displayed queue, so calibrate on a session on the real market.

`fastmm-data calibrate` runs this over several sessions and fits the conservatism and the latencies ([Calibrate the backtest](#calibrate-the-backtest-against-live-sessions)).

### How orders are matched

- Times are venue times: a venue sends its execution report before the public trade that filled the order. The `times` line counts events that had only a receive time.
- Binance order times are whole milliseconds. A trade in the ack's millisecond counts for the order, and the `ties` count shows how many.
- The depth feed shows your own orders; each level is stripped of them before the model sees it, as `journal:<file>,strip_own=1` does ([Market-data sources](../../reference/data-sources.md#journal)).
- A replace follows the new order id. Orders that crossed the book at the ack, and market, IOC and FOK orders, are left out.

The full matching rules, including the book ticker, trade and tie rules, are in [How fill-check matches orders](../../reference/cli.md#how-fill-check-matches-orders).

`--market other.fmj` takes the market data from another journal of the same period, such as a recorder with more depth. `--csv` writes one row per order: price, size, queue ahead at the ack, resting time, how it ended, and the live and model fills.

### Which live fills the model misses

After the table, `fill-check` prints a diagnosis at the last conservatism listed. Each first fill gets the mid move over `--pre-ms` (default 100) before it and markouts at `--horizons-ms` (default `100,1000,10000`), in bps, positive when the mid moved the order's way:

```text
  fills                     n    100ms       1s      10s  pre p10  pre p50  pre p90  through  missing  crossed   cancel
  both, live fill         ...
  both, model fill        ...
  live only               ...
  model only              ...
```

Compare `live only` with `both, live fill` to see whether the missed fills are worse ones.

| Column | Meaning |
|---|---|
| `through` | live: a trade printed through the order's price in the fill's millisecond; model: the fill came from such a print |
| `missing` | no recorded trade at or through the order's price in the live fill's millisecond |
| `crossed` | the opposite touch had reached the order's price by the fill |
| `cancel` | a cancel or replace had gone out before the live fill: it lost the race |

Then each feature splits the orders into buckets: queue ahead, ticks behind the touch, touch reached, trade through, quantity printed, resting time, cancelled. A bucket whose `live/model` stands far from the others shows what the model gets wrong.

## Calibrate the backtest against live sessions

`fastmm-data calibrate` fits the `l2_queue` fill model and the simulated latency to journals recorded by `fastmm-live`, and prints the `[backtest]` keys to use:

```bash
./build/release/bin/fastmm-data calibrate sessA.fmj sessB.fmj sessV.fmj --backtest
```

For each session it replays the orders that rested through the queue model, once per `queue_conservatism` (`--conservatism`, default `0,0.25,0.5,0.75,1`), and counts orders by whether they filled live and in the model ([fill check](#check-the-fill-model-against-live-fills)):

```text
  conservatism     both   live  model   none    hit   miss  false  error model/live
  0.00              151      2      0   1573  0.987  0.013  0.000  0.013      0.987
  ...
  1.00              151      2      0   1573  0.987  0.013  0.000  0.013      0.987
  no ticker         144      9      1   1572  0.941  0.059  0.001  0.065      0.940
  no tape           150      3      0   1573  0.980  0.020  0.000  0.020      0.981
  neither           127     26      1   1572  0.830  0.170  0.001  0.175      0.795
```

| Column or row | Meaning |
|---|---|
| `hit`, `miss` | share of live fills the model also fills, and the rest |
| `false` | share of orders not filled live that the model fills |
| `error` | (live only + model only) over the orders filled either way; the fit minimises it, then `model/live` nearest 1, then prefers the larger conservatism |
| `no ticker`, `no tape`, `neither` | the fitted value without the book ticker, without the trades since the depth update, or both: what each input is worth |

The conservatism is cross-validated: fitted on each session and scored on the others. When every value scores the same, the data does not identify it and 1 is kept; queues at or inside the touch are set by the book ticker, where the conservatism does not act.

The fitted keys look like this:

```text
[backtest]
fill_model = "l2_queue"
queue_conservatism = 0.75
md_arrival = "recorded"
latency_fixed_us = 360
latency_jitter_us = 300
latency_sigma = 0.5
latency_ack_us = 375
latency_ack_jitter_us = 160
latency_ack_sigma = 0.5
latency_cancel_us = 200
latency_cancel_jitter_us = 250
latency_cancel_sigma = 0.5
order_service_us = 220
```

With several venues, including a pool's accounts, the latency keys go under `[backtest.venues.<name>]`, one table per venue. `md_arrival = "recorded"` delivers market data to the strategy when the session received it.

| Latency key | Fitted from |
|---|---|
| `latency_*_us`, `latency_*_jitter_us` | each order's send time, the venue's time on its ack and the ack's arrival; each leg is a fixed part plus a lognormal excess of shape `latency_*_sigma` |
| `latency_cancel_*` | at least 20 isolated cancels, timed by their cancel acks |
| `order_service_us` | messages sent within 1 ms of the one before, which wait for the venue to take that one in |

On Binance Spot a burst of cancels on one connection is taken in about 0.25 ms apart, so the tenth cancel reaches the book some 2 ms after the first. The round trip's tail, a p99 of 40 ms in a live Binance Spot session, is beyond this model.

When a cancel path or service time is fitted, the fill check runs twice more with cancels replayed. `cancels, before` sends them through the order path alone; `cancels, fitted` through everything fitted. The strategy's decisions are those it made live, so the two rows measure the cancel race alone.

`--backtest` then re-runs each session over its journal with our orders stripped, once with its configuration's `[backtest]` and once with the fitted keys. It prints both beside the live session: orders, rejects, fills, time to fill, the PnL decomposition and markouts. This adds everything a backtest re-decides on top of the fill model, such as how often the strategy requotes. `--from-live-state` starts both runs from the session's recorded state ([below](../../explanation/backtesting.md#starting-from-the-live-sessions-state)); `--csv` writes the fill-check grid.

A live commission paid in a third asset such as BNB has no price in the journal. The live fees and net leave it out, as the engine does, and a line under the table gives those fills and their commission.

## Compare a backtest with the live session

A backtest over a live journal re-decides every order. To compare its fills with the session's, write both as CSV with `fastmm-data events`:

```bash
./build/release/bin/fastmm-data events j/session.7.fmj j/session.8.fmj --out live.csv
./build/release/bin/fastmm-backtest --config replay.toml --data "journal:j/session.7.fmj,strip_own=1" \
    --journal-out runs/r/bt.fmj --out runs/r
./build/release/bin/fastmm-data events runs/r/bt.fmj --out replay.csv
```

A backtest order within a few milliseconds of a live one, at the same price, symbol and side, is the same decision; its fills show how the fill model treated it. A live fill with no backtest order near it shows where the two runs parted.

Each row is one record the session sent or received. The columns are `type,time,venue_time,venue,symbol,id,side,price,qty,a,b,c,d,e,flags,trigger,engine`.

| Column | Meaning |
|---|---|
| `time` | receive time, or send time of an outbound message |
| `venue_time` | the venue's time |
| `venue` | the account the record came from or went to |
| `trigger` | receive time of the event an outbound message answered |
| `engine` | when the engine took an inbound record in |
| `flags` | header flags; for a fill or balance, its own flags shifted by 8 (256 marks a replayed execution) |

| Record | `a` to `e` |
|---|---|
| new order | order type, time in force, reduce-only |
| reject | reason, venue code, text |
| fill | liquidity (1 maker, 2 taker), fee, fee asset, exec id, leaves |
| balance | asset, free, locked, total, equity |
| flow | asset, signed amount (positive in), venue id |
| reconciliation | kind (0 begin, 1 open order, 2 position, 3 end), filled quantity |
| parameter update | `name=value` pairs, origin, source |

`--symbol <S>` adds that symbol's trades, book tickers and depth. `--symbol '*' --trades-only` adds every public trade; on Binance spot its id is the exec id of your fill against it.

## Check PnL

A session's PnL has four views, and they should agree:

| View | Source |
|---|---|
| engine | the summary line `fastmm-live: realized_pnl=<r> unrealized_pnl=<u> fees=<f> ...`; net is `r + u - f` at the last mid, and `r` includes funding on perpetuals |
| journal | the fills, through `tools/pnl_report.py` |
| store | `fastmm-pnl` or `fastmm.open_store()`, by day and instrument ([Query what you traded](query-trading-records.md)) |
| account | balance snapshots before and after the session |

1. Before the session, save a snapshot of the account from the venue's account and ticker endpoints. FastMM ships no tool for this. Balances are free plus locked:

    ```json
    {"utc": "2026-09-14T03:25:21Z", "btc": 0.004995, "usdt": 4614.81995, "mid": 77762.685, "open_orders": 0}
    ```

    Use `base` and `quote` as keys, or the lower-case asset names set by `--base-asset` and `--quote-asset` (default `BTC` and `USDT`). `equity` is optional; it defaults to `quote + base * mid`.

2. After the session, save the end snapshot the same way.

3. Run the report:

    ```bash
    python3 tools/pnl_report.py runs/demo-1/session.fmj --engine-log runs/demo-1/engine.log \
      --start runs/demo-1/equity_start.json --end runs/demo-1/equity_end.json
    ```

    It prints fills, maker share, volume, fees and inventory per hour, markouts, the journal's trading PnL, the engine's summary and the account reconciliation. `--self-test` checks the tool on a synthetic journal.

The account's change splits into the inventory you started with and trading:

```text
equity change = starting base balance * (end mid - start mid) + trading
```

The session reconciles when:

- the unexplained balance change is zero; otherwise there are fills outside the journal, deposits or withdrawals, or commission in another asset;
- the journal's trading PnL equals the account's within rounding;
- the engine's net PnL differs from the account's trading PnL only by inventory marked at its last mid instead of the end snapshot's.

Each execution is booked once, by venue execution id, instrument and side, since a reconnect replays executions the journal already has. Commission in the base asset is already inside the inventory: a buy receives `qty - fee`, a sell delivers `qty + fee`. Do not subtract it again.

Markouts use `--markout-horizons` seconds (default `1,10,60`) against the journal's `BookTicker` mids; fills past the last quote are excluded ([Markouts](../../explanation/backtesting.md#markouts)).

## Example: Binance Demo

Two one-hour `basic_mm` sessions on BTCUSDT in Binance Demo Mode, with the same risk limits and a quote size of 0.0003 BTC:

| Session | `half_spread_bps` | Fills in the hour | Account equity change | Of which trading |
|---|---:|---:|---:|---:|
| Quoting 15 bps from the mid | 15 | 0 | +2.11 USDT | 0.00 USDT |
| Quoting at the touch | 0 | 1640 (all maker) | -33.36 USDT | -32.94 USDT |

The second session's report:

| Item | Value |
|---|---|
| notional traded | 31,622.62 USDT |
| fees | 31.62 USDT, 10.00 bps (0.1% maker); 0.00020358 BTC on buys, 15.82 USDT on sells |
| inventory and cash change, after fees | -0.00044358 BTC and +1.5160 USDT, equal to the account's balance changes |
| account equity change | -33.36 USDT = revaluation -0.42 USDT + trading -32.94 USDT |
| engine | realised -1.32, unrealised +0.00, fees 31.62, net -32.94 USDT; 0.003 USDT from the account's trading PnL |
| post-only rejects (`PostOnlyWouldCross`) | 260 |
