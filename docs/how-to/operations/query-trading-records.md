# Query what you traded

Answer the daily questions from the store a session wrote, without replaying its journal. The store is `runs/<engine name>.db` unless `[storage] path` says otherwise; the schema and what it guarantees are in [Storage](../../reference/storage.md).

Turn it on in the configuration (it is on by default):

```toml
[storage]
backend = "sqlite"
path = "runs/mm1.db"
```

## What did I trade yesterday

```bash
build/release/bin/fastmm-pnl pnl --since yesterday --until yesterday
```

```text
day         symbol   settlement_ccy  realized  funding  fees      net       gross_traded  fills
2024-03-04  BTCUSDT  USDT            12.4      -0.6     0.31      12.09     4.2           186
2024-03-04  ETHUSDT  USDT            -3.1      0        0.09      -3.19     11.5          204
2 row(s)
```

`--since` and `--until` take a UTC day (`2024-03-04`) or `today` / `yesterday`. `realized` and `fees` are the change within that day; `net` is `realized - fees`. `funding` is the part of `realized` that perpetual funding paid or received. Unrealised PnL is a mark, not a flow, so it is not summed across days: read it from `fastmm-pnl positions`.

Add `--instrument BTCUSDT` for one symbol, `--engine mm1` for one deployment, `--csv` to pipe it somewhere.

## What is my PnL by day and instrument

```bash
build/release/bin/fastmm-pnl pnl --since 2024-03-01
```

Instruments that settle in different currencies must not be added: the `settlement_ccy` column separates them, and [the risk model](../../explanation/risk-model.md) explains why. For a total per currency, use the `pnl_by_currency` view:

```bash
sqlite3 -header -column runs/mm1.db \
  "SELECT day, settlement_ccy, net_raw / 1e8 AS net FROM pnl_by_currency ORDER BY day"
```

## What did funding cost

```bash
build/release/bin/fastmm-pnl funding --since 2024-03-01 --instrument BTCUSDT
```

One row per payment: `amount` in `asset` (negative paid), the venue's `funding_id`, the position it was paid on, and `replayed` = 1 for one booked from the venue's history rather than its stream.

## Show me the fills of session X

```bash
build/release/bin/fastmm-pnl sessions --limit 5
build/release/bin/fastmm-pnl fills --session 1709510400123456789
```

```text
ts                   symbol   side  liquidity  price      qty    fee      fee_asset  cl_ord_id         exec_id   position_qty  session_id
2024-03-04 09:14:02  BTCUSDT  Buy   Maker      61250.10   0.002  0.00061  quote      0003000000000a1b  88213401  0.002         1709510400123456789
```

`fastmm-pnl orders --session <id>` lists each order in its last state; `fastmm-pnl positions --session <id>` the last position per instrument.

## What did the last session leave behind

```bash
build/release/bin/fastmm-pnl recover --engine mm1
```

```text
session 1709510400123456789 (basic_mm)
  started   2024-03-04 09:00:01
  stopped   never recorded: the process did not shut down cleanly
  exit      0 (kill None)
  pnl       realized 12.4 (funding -0.6) unrealized -0.8 fees 0.31 net 11.29
  fills     186
  journal   runs/mm1-1709510400123456789.fmj
  position  BTCUSDT 0.002 @ 61250.1 realized=12.4 (funding -0.6) unrealized=-0.8 fees=0.31 fills=186
  open      0003000000000a1c BTCUSDT Sell 0.002 (filled 0) @ 61260.5 Live
```

`fastmm-live` logs the same summary when it starts, so an operator sees it without running anything. The open orders are the ones FastMM last saw open; the venue may have cancelled, filled or expired them since, and nothing that happened while the process was down is in here. [Storage](../../reference/storage.md) lists the gaps.

## From Python

```python
import fastmm

with fastmm.open_store("runs/mm1.db") as store:
    daily = store.pnl(since="2024-03-01")            # DataFrame: day, symbol, realized, funding, fees, net
    funding = store.funding(instrument="BTCUSDT")
    fills = store.fills(instrument="BTCUSDT")
    open_orders = store.orders(open_only=True)
    store.query("SELECT day, SUM(fills) FROM pnl_daily GROUP BY day")
```

Raw fixed-point columns come back as floats without the `_raw` suffix (`fee_raw` reads as `fee`) and nanosecond columns as UTC datetimes without `_ns`. It needs pandas: `pip install 'fastmm-engine[pandas]'`.

## When the numbers look short

- `fastmm-pnl sessions` has a `records_dropped` column. A non-zero value means the engine-to-store ring filled and that many records never reached the store, so its rows are incomplete by that many. Raise `[storage] ring_bytes`.
- `clean_shutdown = 0` means the process was killed: the last batch of records and the session's closing row are missing. The journal of that session is the authority.
- `journal_complete = 0` means the journal was not closed either; `fastmm-replay` refuses it without `--allow-incomplete`.
- A store the engine cannot open stops the session at start-up with exit code 3. Set `[storage] backend = "none"` to run without one.

## Rebuilding from a journal

The store can be lost and the journal cannot: the journal is the byte-exact stream and replays exactly ([Journals, replay and PnL](journals-replay-pnl.md)). `python3 tools/pnl_report.py <file.fmj>` computes fills, fees and PnL straight from a journal, which is what to reach for when a store is missing or a session's records were dropped.
