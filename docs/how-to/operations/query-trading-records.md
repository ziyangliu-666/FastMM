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

## Who refused my orders

```bash
build/release/bin/fastmm-pnl rejects --engine mm1 --day today --summary
```

One row per account, instrument, side, whether the order only reduced the position, reason and source, with the refusals summed and the first and last time:

```text
account    symbol   side  reduces  reason          source              rejects  first                last
binance-b  BTCUSDT  Buy   0        RateLimit       account_orders_10s  6        2024-03-04 00:00:00  2024-03-04 00:00:00
binance-b  BTCUSDT  Sell  1        RateLimit       risk_bucket         1        2024-03-04 00:00:00  2024-03-04 00:00:00
binance    BTCUSDT  Buy   0        VenueRateLimit  venue               1        2024-03-04 00:00:00  2024-03-04 00:00:00
```

`source` keeps the limits apart: `risk_bucket` is the engine's `[risk] orders_per_sec` bucket every account shares, `account_orders_10s` (`_1m`, `_1d`, `account_paused`) one account's window as the engine counts it, `venue_local` the connector's own limiter (never sent), `venue` the exchange's answer. `reduces` 1 is an order that would have taken the position towards zero: an inventory exit, not new risk. Without `--summary`, one row per refusal with the budget then (`tokens`, `orders_10s` as used/admitted, ...); [Storage](../../reference/storage.md#rejects) lists the columns. The live budget is in `fastmm-top` (the `order budget` lines and `risk bucket`) and its `--json`.

## What were the parameters at a time

```bash
build/release/bin/fastmm-pnl params --engine sim-local --at "2026-10-10 05:10:20"
```

The values in effect then, in the newest session of the engine that had started by that time (`--session` names one; without `--at`, its last update). One row per parameter and scope: `*` for every instrument, or a symbol that has its own value; `--instrument SYM` shows that instrument's view. `set` is when the value took effect, `origin` is `initial` (the session started with it), `control` (`fastmm-ctl param`) or `strategy` (the strategy's own publisher), `source` the `--source` it was given:

```text
name                     instrument  value  set                  origin   source  session_id
half_spread_bps          *           5      2026-10-10 05:10:23  control  manual  1791609012330808010
level_step_ticks         *           1      2026-10-10 05:10:12  initial          1791609012330808010
...
quote_qty                *           0.002  2026-10-10 05:10:23  control  manual  1791609012330808010
```

`param-changes` lists every value of every update in order, the starting set first (`seq` 0); `changed` is 0 for a value a control update repeated:

```bash
build/release/bin/fastmm-pnl param-changes --engine sim-local --session 1791609012330808010
```

```text
ts                   session_id           seq  origin   source           instrument  name             value  changed
2026-10-10 05:10:12  1791609012330808010  0    initial                   *           half_spread_bps  5      1
...
2026-10-10 05:10:22  1791609012330808010  386  control  scheduled:drain  *           half_spread_bps  9      1
2026-10-10 05:10:23  1791609012330808010  419  control  manual           *           half_spread_bps  5      1
2026-10-10 05:10:23  1791609012330808010  419  control  manual           *           quote_qty        0.002  1
```

## What did a restart change

```bash
build/release/bin/fastmm-pnl param-diff --engine mm1 --session <new session id>
```

The parameters the previous session of the engine ended with against those the named one started with, one row per value that differs: a parameter set with `fastmm-ctl param` and not put in the configuration comes back to its configured value. `--against <id>` compares with another session.

## Which configuration did a session run with

```bash
build/release/bin/fastmm-pnl config --engine mm1 --session <id>
```

The effective configuration the session started with (the file after `--strategy` and `--param`, secrets left out), as the journal header holds it. Take a backtest's configuration from here rather than from the file on the host, which may have been edited since. `--at <time>` picks the session that was running then.

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

## The economic ledger

```bash
build/release/bin/fastmm-pnl ledger --engine mm1 --day today
```

`fills` lists the rows the store holds; after a restart that booked an execution again, or a replay that brought in what traded while the process was down, those are not the same as what happened. `ledger` gives one row per execution (the venue's id of it on an account, symbol and side) however many sessions stored it, as the first one did:

```text
received             venue_time           account  symbol   side  liquidity  price     qty    booked_qty  fee      fee_asset  cl_ord_id       exec_id     flags         copies  session_id
2024-03-04 09:14:02  2024-03-04 09:14:02  bybit    ETHUSDT  Buy   Maker      3412.55   0.05   0.05        0.00085  quote      fm000700000003  2966124872  repeated      2       5
2024-03-04 09:20:41  2024-03-04 09:16:30  bybit    ETHUSDT  Buy   Maker      3411.90   0.05   0.05        0.00085  quote      fm000700000005  2966124877  before_start  1       8
2024-03-04 09:14:09  2024-03-04 09:14:09  binance  BTCUSDT  Buy   Maker      61250.10  0.002  0.002       0.00061  quote      fm000700000004  2966124872                1       6
```

`received` is when FastMM first had it, `venue_time` when the venue says it traded, `booked_qty` what the position took. `flags`: `repeated` (stored by more than one session; `copies` says how many), `before_start` (traded before the session that booked it started: the restart's replay brought it), `late` (for an order that had already ended), `synthetic` (booked from a jump in the order's filled quantity, without a venue report). `--session` keeps the executions that session booked first. `--raw` lists every stored row instead, with its `copy` number. The positions and PnL tables are computed from the raw rows; a `repeated` execution is counted twice there ([below](#executions-stored-twice)).

## What happened to an order

```bash
build/release/bin/fastmm-pnl order --engine mm1 --order fm000700000003
```

Everything the store has about one client order, in time order, its last known state at the end:

```text
ts                   event    symbol   side  price    qty   cum_qty  detail                                 session_id
2024-03-04 09:14:01  sent     ETHUSDT  Buy   3412.55  0.05           Limit GTC                              5
2024-03-04 09:14:01  refused  ETHUSDT  Buy   3412.55  0.05           VenueRateLimit by venue: Too many new orders  5
2024-03-04 09:14:02  fill     ETHUSDT  Buy   3412.55  0.05  0.05     Maker exec 2966124872                  5
2024-03-04 09:14:02  state    ETHUSDT  Buy   3412.55  0.05  0.05     Filled after 3 update(s)               5
```

`refused` rows (schema 9) name the limit that refused it, as `rejects` does. The store keeps an order's last state, not each transition; the journal has every message ([Journal format](../../reference/journal-format.md)). `--session` narrows it to one session.

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
