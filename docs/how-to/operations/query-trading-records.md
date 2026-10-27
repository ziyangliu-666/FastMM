# Query what you traded

Read what a deployment traded from the store its sessions wrote, without replaying a journal. You need `fastmm-pnl` from a C++ build or the [release tarball](deploy.md); with only the pip package, use [Python](#from-python).

The store is on by default, at `runs/<engine name>.db` unless `[storage] path` says otherwise ([Storage](../../reference/storage.md)):

```toml
[storage]
backend = "sqlite"
path = "runs/mm1.db"
```

The commands below share these options:

| Option | Selects |
|---|---|
| `--engine mm1` | `runs/mm1.db`; `--store <path>` reads another file |
| `--day`, `--since`, `--until` | a UTC day (`2024-03-04`), `today` or `yesterday` |
| `--session <id>` | one session |
| `--instrument BTCUSDT` | one symbol |
| `--csv` | comma-separated output |

## What did I trade yesterday

```bash
build/release/bin/fastmm-pnl pnl --engine mm1 --day yesterday
```

```text
day         account  symbol   settlement_ccy  realized  funding  fees  net    gross_traded  fills  estimates  books_net
2024-03-04  binance  BTCUSDT  USDT            12.4      -0.6     0.31  12.09  4.2           186    0          12.09
2024-03-04  binance  ETHUSDT  USDT            -3.1      0        0.09  -3.19  11.5          204    1          -3.19
2 row(s)
```

The numbers are recomputed from the stored executions at average cost, carried across sessions; an execution counts once however many records hold it.

| Column | Means |
|---|---|
| `account` | the `[venues.<name>]` holding the position; a pool's members trade into its primary's |
| `realized`, `fees` | the change within the day |
| `funding` | the part of `realized` paid or received as perpetual funding |
| `net` | `realized - fees` |
| `estimates` | fills booked at the order's price before any execution named them, for example after a cancel ack arrived ahead of the fill |
| `books_net` | what the engine booked from its own positions; differs from `net` when it booked an execution twice ([`fill_repeats`](../../reference/storage.md#fill_repeats)) |

Unrealised PnL is a mark, not a flow, so it is not summed by day: read it from `fastmm-pnl positions`. `--books` prints the engine's books alone.

## What is my PnL by day and instrument

```bash
build/release/bin/fastmm-pnl pnl --engine mm1 --since 2024-03-01
```

Do not add instruments that settle in different currencies; `settlement_ccy` keeps them apart ([Risk model](../../explanation/risk-model.md)). The `pnl_by_currency` view totals per currency:

```bash
sqlite3 -header -column runs/mm1.db \
  "SELECT day, settlement_ccy, net_raw / 1e8 AS net FROM pnl_by_currency ORDER BY day"
```

## What did funding cost

```bash
build/release/bin/fastmm-pnl funding --engine mm1 --since 2024-03-01 --instrument BTCUSDT
```

One row per payment: `amount` in `asset` (negative when paid), the venue's `funding_id`, the position it was paid on, and `replayed` = 1 for a payment booked from the venue's history rather than its stream.

## Who refused my orders

```bash
build/release/bin/fastmm-pnl rejects --engine mm1 --day today --summary
```

```text
account    symbol   side  reduces  reason          source              rejects  first                last
binance-b  BTCUSDT  Buy   0        RateLimit       account_orders_10s  6        2024-03-04 00:00:00  2024-03-04 00:00:00
binance-b  BTCUSDT  Sell  1        RateLimit       risk_bucket         1        2024-03-04 00:00:00  2024-03-04 00:00:00
binance    BTCUSDT  Buy   0        VenueRateLimit  venue               1        2024-03-04 00:00:00  2024-03-04 00:00:00
```

`reduces` = 1 is an order that would have taken the position towards zero: an inventory exit, not new risk. `source` says which limit refused it:

| `source` | Limit |
|---|---|
| `risk_bucket` | the engine's `[risk] orders_per_sec` bucket, shared by every account |
| `account_orders_10s`, `_1m`, `_1d`, `account_paused` | one account's window, as the engine counts it |
| `venue_local` | the connector's own limiter; never sent |
| `venue` | the exchange's answer |

Without `--summary` there is one row per refusal, with the budget at that moment ([columns](../../reference/storage.md#rejects)). The live budget is in `fastmm-top` (`order budget` and `risk bucket`) and its `--json`.

## What were the parameters at a time

```bash
build/release/bin/fastmm-pnl params --engine sim-local --at "2026-10-10 05:10:20"
```

```text
name                     instrument  value  set                  origin   source  session_id
half_spread_bps          *           5      2026-10-10 05:10:23  control  manual  1791609012330808010
level_step_ticks         *           1      2026-10-10 05:10:12  initial          1791609012330808010
...
quote_qty                *           0.002  2026-10-10 05:10:23  control  manual  1791609012330808010
```

It reads the newest session started by that time; `--session` names one, and without `--at` you get its last values. A row's `instrument` is `*` for every instrument or a symbol with its own value; `--instrument SYM` shows one instrument's view. `set` is when the value took effect, and `source` is the `--source` it was sent with.

| `origin` | Set by |
|---|---|
| `initial` | the configuration the session started with |
| `control` | `fastmm-ctl param` |
| `strategy` | the strategy's own publisher |

`param-changes` lists every value of every update in order, the starting set first as `seq` 0. `changed` is 0 for a value an update repeated.

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

One row per value that differs between what the previous session ended with and what this one started with. A parameter set with `fastmm-ctl param` and not put in the configuration shows here, back at its configured value. `--against <id>` compares with another session.

## Which configuration did a session run with

```bash
build/release/bin/fastmm-pnl config --engine mm1 --session <id>
```

It prints the effective configuration from the journal header: the file after `--strategy` and `--param`, without secrets. Take a backtest's configuration from here, not from the file on the host, which may have changed since. `--at <time>` picks the session running then.

## Show me the fills of session X

```bash
build/release/bin/fastmm-pnl sessions --engine mm1 --limit 5
build/release/bin/fastmm-pnl fills --engine mm1 --session 1709510400123456789
```

```text
ts                   account  symbol   side  liquidity  price      qty    named_qty  fee      fee_asset  cl_ord_id         exec_id   synthetic  position_qty  arm      session_id
2024-03-04 09:14:02  binance  BTCUSDT  Buy   Maker      61250.10   0.002  0          0.00061  quote      fm000300000a1b    88213401  0          0.002         control  1709510400123456789
```

A `synthetic` row is an estimate booked from a venue's filled quantity before the execution arrived. Its `named_qty` is how much executions have named since; on an execution, how much of it named an estimate. Count an execution whole and a synthetic row as `qty - named_qty`.

`arm` is the `[ab]` arm the order was sent under, empty outside a test ([Run an A/B test](../strategies/ab-test.md)). `fastmm-pnl orders --session <id>` lists each order in its last state, and `fastmm-pnl positions --session <id>` the last position per instrument.

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

`fastmm-live` logs the same summary when it starts. A `twice` line names an execution stored by more than one session ([below](#executions-stored-twice)).

The open orders are the ones FastMM last saw open; nothing that happened while the process was down is here. The next start restores the position, books what the venue executed in between and cancels the orders left open ([Recovery at start-up](../../reference/storage.md#recovery-at-start-up)).

## Executions stored twice

```bash
build/release/bin/fastmm-pnl duplicates --engine mm1
```

One row per execution or funding payment that more than one session stored, because a restart booked it again. Rows show the kind (`fill` or `funding`), engine, venue, symbol, id, number of copies, side, quantity or amount, first time, and the sessions holding it, oldest first. The exit code is 0 with no rows and 4 with some.

The positions, fees and PnL of those sessions count it twice, and every other command warns on stderr when it reads such a row. Nothing is rewritten: correct the figures from the listed rows. `store.duplicates()` returns the same rows in Python.

## The economic ledger

`fills` lists the stored rows, which after a re-booking restart or an execution replay are not the same as what happened. `ledger` gives one row per execution, however many sessions stored it, as the first one did:

```bash
build/release/bin/fastmm-pnl ledger --engine mm1 --day today
```

```text
received             venue_time           account  symbol   side  liquidity  price     qty    booked_qty  fee      fee_asset  cl_ord_id       exec_id     flags         copies  session_id
2024-03-04 09:14:02  2024-03-04 09:14:02  bybit    ETHUSDT  Buy   Maker      3412.55   0.05   0.05        0.00085  quote      fm000700000003  2966124872  repeated      2       5
2024-03-04 09:20:41  2024-03-04 09:16:30  bybit    ETHUSDT  Buy   Maker      3411.90   0.05   0.05        0.00085  quote      fm000700000005  2966124877  before_start  1       8
2024-03-04 09:14:09  2024-03-04 09:14:09  binance  BTCUSDT  Buy   Maker      61250.10  0.002  0.002       0.00061  quote      fm000700000004  2966124872                1       6
```

`received` is when FastMM first had it, `venue_time` when the venue says it traded, `booked_qty` what the position took.

| Flag | Means |
|---|---|
| `repeated` | stored by more than one session; `copies` says how many |
| `before_start` | traded before the session that booked it started; a restart's replay brought it |
| `late` | arrived for an order that had already ended |
| `synthetic` | booked from a jump in the order's filled quantity, with no venue report |

`--session` keeps the executions that session booked first; `--raw` lists every stored row with its `copy` number. The positions and PnL tables are computed from the raw rows, so a `repeated` execution counts twice there.

## What happened to an order

```bash
build/release/bin/fastmm-pnl order --engine mm1 --order fm000700000003
```

```text
ts                   event    symbol   side  price    qty   cum_qty  detail                                 session_id
2024-03-04 09:14:01  sent     ETHUSDT  Buy   3412.55  0.05           Limit GTC                              5
2024-03-04 09:14:01  refused  ETHUSDT  Buy   3412.55  0.05           VenueRateLimit by venue: Too many new orders  5
2024-03-04 09:14:02  fill     ETHUSDT  Buy   3412.55  0.05  0.05     Maker exec 2966124872                  5
2024-03-04 09:14:02  state    ETHUSDT  Buy   3412.55  0.05  0.05     Filled after 3 update(s)               5
```

The rows are everything the store has about one client order, ending with its last known state. A `refused` row names the limit, as `rejects` does. The store keeps the last state, not each transition; the journal has every message ([Journal format](../../reference/journal-format.md)).

## Check the stored fills against the venue

1. Export the account's executions from the venue, as JSON or CSV ([formats](#export-formats)).
2. Compare them with the store:

    ```bash
    build/release/bin/fastmm-pnl audit --engine mm1 --venue binance --exchange trades.json
    ```

    ```text
    kind     time_ms        symbol   exec_id   side  venue_qty  venue_price  venue_fee       booked_qty  booked_price  booked_fee       order_id  fields  session_id
    missing  1709543642118  BTCUSDT  88213977  Buy   0.002      61248.3      0.0000012 BTC                                                    40012
    differs  1709543700402  BTCUSDT  88214102  Sell  0.002      61251.2      0.06125 USDT    0.001       61251.2       0.03062 quote    40019     qty,fee 1709510400123456789
    2 row(s)
    ```

3. Read the exit code: 0 when everything agrees, 5 otherwise. A summary goes to stderr.

| Kind | Means |
|---|---|
| `missing` | the venue has the execution; no session booked it |
| `phantom` | a session booked it; the venue does not have it |
| `differs` | both have it; `fields` names what differs: `qty`, `price`, `fee`, `side`, `order` (fee and order only where both report one) |
| `twice` | stored by more than one session |

Rows match on symbol (case and separators ignored, so `BTC-USDT` is `BTCUSDT`), the venue's trade id and side. The window is the file's span unless `--from-ms`/`--to-ms` (Unix ms) or `--since`/`--until` (UTC days) set it. The store is read one minute past both ends, so a fill the two sides stamp slightly apart is compared, not reported twice. `--venue` selects one `[venues.<name>]` and skips the file's rows naming another.

`fastmm-live` can run the same comparison while it trades ([Fill audit](../../reference/configuration.md#fill-audit)).

### Export formats

The file is either a JSON array or a CSV with a header row, columns in any order. A JSON array can be Binance's `GET /api/v3/myTrades` or `GET /fapi/v1/userTrades` reply, several joined into one array, or objects with the names below.

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

## From Python

`fastmm.open_store(path)` reads fills, orders, positions, PnL, funding and kill events as DataFrames ([Python](../../python.md#other-helpers)). It needs pandas: `pip install 'fastmm-engine[pandas]'`.

```python
import fastmm

with fastmm.open_store("runs/mm1.db") as store:
    daily = store.pnl(since="2024-03-01")            # DataFrame: day, symbol, realized, funding, fees, net
    funding = store.funding(instrument="BTCUSDT")
    fills = store.fills(instrument="BTCUSDT")
    open_orders = store.orders(open_only=True)
    store.query("SELECT day, SUM(fills) FROM pnl_daily GROUP BY day")
```

Fixed-point columns come back as floats without the `_raw` suffix (`fee_raw` reads as `fee`), and nanosecond columns as UTC datetimes without `_ns`.

## When the numbers look short

| Sign in `fastmm-pnl sessions` | Cause | Action |
|---|---|---|
| `records_dropped` above 0 | the engine-to-store ring filled; that many records never reached the store | raise `[storage] ring_bytes` |
| `clean_shutdown = 0` | the process was killed; the last batch and the closing row are missing | use the session's journal |
| `journal_complete` 0 or NULL | the session stopped without closing its journal (NULL after a kill) | `fastmm-replay` needs `--allow-incomplete` |

A store the engine cannot open stops the session at start-up with exit 3; `[storage] backend = "none"` runs without one.

The journal is the byte-exact record ([Journals, replay and PnL](journals-replay-pnl.md)), and the store is derived from the same events. When a store is missing or dropped records, compute fills, fees and PnL from the journal:

```bash
python3 tools/pnl_report.py <file.fmj>
```
