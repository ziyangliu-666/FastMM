# Query what you traded

Read what a deployment traded from the store its sessions wrote, without replaying a journal. The store is `runs/<engine name>.db` unless `[storage] path` says otherwise; the schema and what it guarantees are in [Storage](../../reference/storage.md).

It is on by default:

```toml
[storage]
backend = "sqlite"
path = "runs/mm1.db"
```

## What did I trade yesterday

```bash
build/release/bin/fastmm-pnl pnl --engine mm1 --day yesterday
```

```text
day         symbol   settlement_ccy  realized  funding  fees      net       gross_traded  fills
2024-03-04  BTCUSDT  USDT            12.4      -0.6     0.31      12.09     4.2           186
2024-03-04  ETHUSDT  USDT            -3.1      0        0.09      -3.19     11.5          204
2 row(s)
```

`--day`, `--since` and `--until` take a UTC day (`2024-03-04`) or `today` / `yesterday`. `realized` and `fees` are the change within that day; `net` is `realized - fees`. `funding` is the part of `realized` that perpetual funding paid or received. Unrealised PnL is a mark, not a flow, so it is not summed across days: read it from `fastmm-pnl positions`.

`--engine mm1` reads `runs/mm1.db` (`--store <path>` another file), `--instrument BTCUSDT` selects one symbol, `--csv` prints comma-separated values.

## What is my PnL by day and instrument

```bash
build/release/bin/fastmm-pnl pnl --engine mm1 --since 2024-03-01
```

Do not add instruments that settle in different currencies; the `settlement_ccy` column separates them ([Risk model](../../explanation/risk-model.md)). For a total per currency, use the `pnl_by_currency` view:

```bash
sqlite3 -header -column runs/mm1.db \
  "SELECT day, settlement_ccy, net_raw / 1e8 AS net FROM pnl_by_currency ORDER BY day"
```

## What did funding cost

```bash
build/release/bin/fastmm-pnl funding --engine mm1 --since 2024-03-01 --instrument BTCUSDT
```

One row per payment: `amount` in `asset` (negative paid), the venue's `funding_id`, the position it was paid on, and `replayed` = 1 for one booked from the venue's history rather than its stream.

## Show me the fills of session X

```bash
build/release/bin/fastmm-pnl sessions --engine mm1 --limit 5
build/release/bin/fastmm-pnl fills --engine mm1 --session 1709510400123456789
```

```text
ts                   symbol   side  liquidity  price      qty    fee      fee_asset  cl_ord_id         exec_id   position_qty  session_id
2024-03-04 09:14:02  BTCUSDT  Buy   Maker      61250.10   0.002  0.00061  quote      fm000300000a1b    88213401  0.002         1709510400123456789
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
  open      fm000300000a1c BTCUSDT Sell 0.002 (filled 0) @ 61260.5 Live
```

A `twice` line names an execution the store holds in more than one session ([below](#executions-stored-twice)).

`fastmm-live` logs the same summary when it starts. The open orders are the ones FastMM last saw open; the venue may have cancelled, filled or expired them since, and nothing that happened while the process was down is in here. The next `fastmm-live` start restores the position from the store, books what the venue executed in between and cancels the orders left open ([Recovery at start-up](../../reference/storage.md#recovery-at-start-up)).

## Executions stored twice

```bash
build/release/bin/fastmm-pnl duplicates --engine mm1
```

One row per venue execution or funding payment that more than one session stored (same venue, symbol and venue id): a restart booked it again. The columns are the kind (`fill` or `funding`), engine, venue, symbol, id, the number of copies, side, quantity (the amount of a funding payment), the time of the first copy and the sessions holding it, oldest first. Exit code 0 with no row, 4 with some. Every other command prints a warning on stderr when the rows it read from hold one: the positions, fees and PnL of those sessions count it twice. Nothing is rewritten; correct the figures from the listed rows. `store.duplicates()` returns the same rows in Python.

## Check the stored fills against the venue

Export the account's executions from the venue and compare them with what the store holds:

```bash
build/release/bin/fastmm-pnl audit --engine mm1 --venue binance --exchange trades.json
```

```text
kind     time_ms        symbol   exec_id   side  venue_qty  venue_price  venue_fee       booked_qty  booked_price  booked_fee       order_id  fields  session_id
missing  1709543642118  BTCUSDT  88213977  Buy   0.002      61248.3      0.0000012 BTC                                                    40012                     
differs  1709543700402  BTCUSDT  88214102  Sell  0.002      61251.2      0.06125 USDT    0.001       61251.2       0.03062 quote    40019     qty,fee 1709510400123456789
2 row(s)
```

`missing`: the venue has the execution and no session booked it. `phantom`: a session booked it and the venue does not have it. `differs`: both have it and `fields` names what differs (`qty`, `price`, `fee`, `side`, `order`; the fee and the order only where both sides report one). `twice`: stored by more than one session. Rows are matched by symbol (case and separators ignored: `BTC-USDT` is `BTCUSDT`), the venue's trade id and side. A summary goes to stderr; the exit code is 0 when everything agrees and 5 otherwise.

The window is the file's span unless `--from-ms`/`--to-ms` (Unix ms) or `--since`/`--until` (UTC days) say otherwise; the store is read a minute past both ends, so a fill the two sides stamp a little differently is compared rather than reported twice. `--venue` selects the store's fills of one `[venues.<name>]` (and the file's rows naming another are skipped); `--instrument` one symbol.

The file is either:

- a JSON array as Binance returns `GET /api/v3/myTrades` or `GET /fapi/v1/userTrades` (several replies can be joined into one array), or of objects with the generic names below;
- CSV with a header row naming the columns, in any order.

| Column | Aliases | |
|---|---|---|
| `symbol` | | required |
| `exec_id` | `id`, `trade_id`, `tradeId` | the venue's trade id, required |
| `side` | `isBuyer`, `buyer` (true: Buy) | `buy` or `sell`, any case, required |
| `price`, `qty` | `quantity` | decimals, required |
| `time_ms` | `time` | Unix ms or a UTC time `2024-03-04 09:14:02.118`, required |
| `order_id` | `orderId` | the venue's order id |
| `cl_ord_id` | `clientOrderId` | the client order id |
| `fee` | `commission` | as charged, in `fee_asset` |
| `fee_asset` | `commissionAsset` | |
| `venue` | | the `[venues.<name>]` |

fastmm-live can run the same comparison against the venue's trade history while it trades ([Fill audit](../../reference/configuration.md#fill-audit)).

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
- `journal_complete` is 0 when a session stopped without closing its journal, and NULL after a kill (the row was never closed). Either way `fastmm-replay` refuses that journal without `--allow-incomplete`.
- A store the engine cannot open stops the session at start-up with exit code 3. `[storage] backend = "none"` runs without one.

## Rebuilding from a journal

The journal is the byte-exact record and replays exactly ([Journals, replay and PnL](journals-replay-pnl.md)); the store is derived from the same events. When a store is missing or a session dropped records, `python3 tools/pnl_report.py <file.fmj>` computes fills, fees and PnL from the journal.
