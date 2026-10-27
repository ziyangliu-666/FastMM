# Storage

The store keeps what a session traded as rows a query can answer: fills, orders, positions, PnL, funding, rejects, kill events and parameter changes. It survives a restart and is read without replaying a journal. Code: `include/fastmm/store/`, `src/store/`.

| | Journal (`.fmj`) | Store |
|---|---|---|
| Holds | every event the engine consumed and every message it sent, byte for byte | the interpreted result: fills, orders, positions, PnL, kill events |
| Answers | does this session reproduce? (`fastmm-replay`) | what did I trade yesterday? (`fastmm-pnl`, `fastmm.open_store`) |
| Written on | the `fm-journal` thread | the `fm-store` thread, by a backend |
| A full ring | trips the kill switch: replay needs every event | drops the record and counts it |
| Authority | yes | no: every row is derivable from the journal |

Column conventions:

| Suffix or name | Meaning |
|---|---|
| `*_raw` | price, quantity or PnL as raw fixed-point `int64` (1e-8) |
| `*_ns` | `int64` nanoseconds since the Unix epoch |
| `day` | the UTC day of the row's timestamp |
| `venue_id` | the venue's place in the session's `[venues.*]` order; another session may give the id to another account |
| `account` | the `[venues.<name>]` of the row; group by it across sessions, not by `venue_id` |

## Configuration

`[storage]` is free-form: the central schema knows none of its keys, and each backend reads its own.

```toml
[storage]
backend = "sqlite"        # "none" disables the store
path = "runs/mm1.db"      # sqlite: default "<[engine] journal_dir>/<[engine] name>.db"
ring_bytes = 4194304      # engine-to-store ring, a power of two (default 4194304)
kill_on_failure = false   # trip the kill switch when a write fails or a record is dropped
```

| Key | Backend | Meaning |
|---|---|---|
| `backend` | all | backend name, or `none` (default `sqlite`) |
| `ring_bytes` | all | bytes of the ring the engine writes records into, a power of two (default 4194304) |
| `kill_on_failure` | all | `fastmm-live` trips the kill switch (`StoreFailure`) on each new failed write or dropped record (default false) |
| `path` | sqlite | store file (default `<journal_dir>/<engine name>.db`) |

`backend = "none"` allocates no ring and starts no thread. A backend that cannot be opened stops the session before it trades, with exit code 3.

## How a record reaches the store

The engine thread writes each record into a single-producer ring, and the `fm-store` thread drains it into the backend.

The engine copies each record into the ring without allocating, calling the kernel or waiting. The store thread batches up to 512 records into one backend transaction and sleeps 2 ms when the ring is empty. The control thread writes the session rows and the instrument table directly.

| Record | Written when | Carries |
|---|---|---|
| `FillRecord` | every execution, and every quantity booked from a `cum_qty` jump | price, quantity, booked quantity, fee, liquidity, ids, the venue's trade time and the position after it |
| `OrderRecord` | every OMS state change | the whole order: state, previous state, price, quantity, filled quantity, reject reason |
| `PositionRecord` | after every fill, after a venue position snapshot, and per instrument at the end | the instrument's position and the portfolio totals |
| `FundingRecord` | every funding payment booked | amount, asset, venue funding id and time, the position and its PnL after it |
| `ReplayedRecord` | a venue's reconciliation begins after a whole execution replay | the venue: its executions until then are all stored |
| `KillRecord` | every kill switch trip, global or per venue | reason, flag word, PnL |
| `RejectRecord` | every refused order, repeats folded ([rejects](#rejects)) | account, instrument, side, reason, what refused it, the budget then |
| `LimitsRecord` | every `fastmm-ctl limits` applied | the `[risk]` limits before and after, and whether `--force` turned one off |
| `ParamRecord` | every strategy parameter update applied | target, origin, source, publisher sequence and the (field, raw value) pairs ([Journal format](journal-format.md#parameter-updates)) |

The record layouts are in `include/fastmm/core/record_stream.hpp`.

## What a backend guarantees

| Guarantee | Meaning |
|---|---|
| Ordering | records arrive on one thread in the engine's order; `seq` increases within a session |
| At most once | a dropped record, or a batch the process died inside, is gone; keys (session id and `seq`, a fill's exec id) make re-ingesting a record a no-op |
| No back pressure | the store never signals the engine; a full ring drops records and counts them |
| No exceptions | none cross the interface, and no `abort()` |

A failed write is counted and the first one logged; the backend keeps going. A restart restores positions from the store, so `fastmm-live` treats a loss as an error:

| Where | What |
|---|---|
| log | the first failure or drop, as ERROR; the totals at shutdown |
| [status file](status-file.md) | `store_errors`, `store_dropped` |
| metrics | `fastmm_store_errors_total`, `fastmm_store_records_dropped_total`, alert `FastMMStoreFailing` |
| `sessions.records_dropped` | the drops of the session |
| `kill_on_failure = true` | trips the kill switch |

```text
store: 1284 record(s) in 37 batch(es), 1284 row(s), 0 error(s), 0 dropped
```

## Durability

The SQLite backend opens the file with `journal_mode=WAL` and `synchronous=NORMAL`, and checkpoints every 256 pages.

| Event | What survives |
|---|---|
| the process dies (crash, `SIGKILL`, `abort()`) | every committed batch: the next open replays the write-ahead log |
| the process dies inside a batch | nothing of that batch: it rolls back whole |
| power loss | the batches up to the last checkpoint; the journal holds the rest |
| a reader during a session | a reader never blocks the writer or sees half a batch |

## Schema

The schema is version 12, in `src/store/sqlite_schema.cpp`. An older store is migrated in place when opened, after a copy to `<path>.v<version>.bak`. A store from a newer FastMM is refused with the version it holds.

### sessions

One row per session, written at start and completed at shutdown.

| Column | Meaning |
|---|---|
| `session_id` | the engine's session id (primary key) |
| `engine`, `strategy` | `[engine] name` and the strategy that ran |
| `session_epoch` | client order id epoch of this session |
| `started_ns`, `started_day`, `stopped_ns` | start, its UTC day, and stop; `stopped_ns` is null when the process did not shut down |
| `version`, `build` | FastMM version and build info |
| `config_hash`, `config_toml` | the effective configuration, secrets omitted, and its hash, as in the journal header |
| `host`, `pid`, `dry_run` | where and how it ran |
| `pnl_carry_raw` | the net PnL carried in from earlier sessions |
| `clean_shutdown` | 1 when the session closed; 0 when the process was killed |
| `exit_code`, `kill_reason`, `kill_latched` | how it ended; a clean shutdown trips the kill switch itself, so `kill_reason` is `Requested` |
| `journal_complete`, `journal_bytes` | whether the journal was closed cleanly, and its size over every part |
| `events`, `orders_sent`, `cancels_sent`, `replaces_sent`, `fills`, `risk_rejects`, `venue_rejects` | the session's counters |
| `records_dropped` | records the ring could not take: the other tables miss that many |
| `realized_raw`, `unrealized_raw`, `fees_raw` | the session's final PnL |
| `funding_raw` | the part of `realized_raw` that is funding (version 4) |
| `param_refreshes` | strategy parameter updates that changed nothing and wrote no row |

### session_journals

| Column | Meaning |
|---|---|
| `session_id`, `part`, `path` | the journal parts the session wrote, in order |

### session_venues

| Column | Meaning |
|---|---|
| `session_id`, `venue_id`, `name` | the `[venues.<name>]` behind each `venue_id`, so a restart finds its venues by name (version 3) |
| `replayed_seq` | `seq` of the venue's last `ReplayedRecord`; 0 while the session had not finished an execution replay there (version 6) |

### instruments

One row per `(session_id, instrument_id)`, as the session loaded it: a restart may load different reference data.

| Column | Meaning |
|---|---|
| `venue_id`, `account` | its venue; `account` from version 10 |
| `symbol`, `base`, `quote`, `settlement_ccy`, `asset_class`, `inverse` | what it is |
| `tick_raw`, `lot_raw`, `multiplier_raw` | its grid and contract size |

### fills

One row per execution, keyed `(session_id, seq)`. A unique index on `(session_id, instrument_id, side, exec_id)` stops an execution being stored twice in a session.

| Column | Meaning |
|---|---|
| `ts_ns`, `day`, `symbol`, `venue_id`, `instrument_id` | when on the engine clock, what and where |
| `exch_ns` | the venue's time of the trade; 0 for a synthetic fill or a venue that reports none |
| `cl_ord_id`, `venue_order_id`, `exec_id` | the ids FastMM and the venue know it by |
| `side`, `liquidity` | `Buy`/`Sell`; `Maker`/`Taker`/`Unknown` |
| `price_raw`, `qty_raw` | the execution |
| `booked_qty_raw` | what the position moved by: less a base-asset fee, less what an estimate already moved |
| `cum_qty_raw`, `leaves_qty_raw` | the order after it |
| `fee_raw`, `fee_amount_raw`, `fee_asset` | the fee booked in the settlement currency, the venue's amount, and its asset (`quote`, `base`, `other`; `other` is not booked) |
| `position_qty_raw`, `position_avg_px_raw`, `position_realized_raw`, `position_fees_raw` | the instrument's position after this fill |
| `synthetic` | 1 for an estimate: quantity booked from a `cum_qty` jump at the order's price, with no fee |
| `late` | 1 for a fill that arrived after its order was terminal |
| `account` | the `[venues.<name>]` it executed on, a pool member included (version 10) |
| `named_qty_raw` | on an estimate, how much executions have named since; on an execution, how much of it named an estimate (version 10) |
| `arm` | the `[ab]` arm its order was sent under; NULL outside a test (version 12) |

An estimate's execution corrects its price and fee without moving the position again. Both rows stay, so sum quantity this way:

| Row | Counts |
|---|---|
| `synthetic = 0` | `qty_raw` |
| `synthetic = 1` | `qty_raw - named_qty_raw`, the part still estimated |

### fill_repeats

An execution the engine booked a second time in a session that had stored it (version 10). The position and PnL the engine recorded count it twice; `fills` does not.

| Column | Meaning |
|---|---|
| `session_id`, `seq` | the refused fill record |
| `account`, `instrument_id`, `symbol`, `side`, `exec_id` | the execution |
| `price_raw`, `qty_raw`, `booked_qty_raw` | as in `fills` |
| `first_seq` | `seq` of the row `fills` holds |

Sessions from before version 10 are backfilled from their position records.

### funding

One row per funding payment booked, keyed `(session_id, seq)`, unique on `(session_id, instrument_id, funding_id)` (version 4). Funding is realized PnL, so every `realized_raw` includes it.

| Column | Meaning |
|---|---|
| `ts_ns`, `day`, `symbol`, `venue_id`, `instrument_id` | when on the engine clock, on what, where |
| `exch_ns` | the venue's time of the payment |
| `funding_id` | the venue's id of it (Binance `tranId`, Bybit `execId`) |
| `amount_raw`, `asset` | in the settlement currency: negative paid, positive received |
| `position_qty_raw`, `position_realized_raw`, `position_funding_raw` | the position paid on, and the instrument's realized PnL and funding after it |
| `replayed` | 1 when it came from the venue's history, not its private stream |

### orders

One row per order, keyed `(session_id, cl_ord_id)` and updated in place with the last state seen.

| Column | Meaning |
|---|---|
| `updates` | state changes the order went through |
| `terminal` | 1 for `Filled`, `Canceled`, `Rejected`, `Expired` and `Replaced`; the `open_orders` view lists the rest |
| `arm` | the `[ab]` arm it was last sent or replaced under; NULL outside a test (version 12) |

`Replaced` is the store's own state. A cancel-replace renames the order to a new client id, so the old id never reports a terminal state; its row closes when the replacement is acked.

A fill that arrives after its order ended updates the row's `cum_qty` and, when it completes the order, turns `Canceled` into `Filled`.

### positions

One row per position snapshot, keyed `(session_id, seq)`.

| Column | Meaning |
|---|---|
| `qty_raw`, `avg_px_raw` | the position |
| `realized_raw`, `unrealized_raw`, `fees_raw`, `funding_raw` | its PnL; `funding_raw` from version 4 |
| `gross_traded_raw`, `fills` | traded so far |
| `total_realized_raw`, `total_unrealized_raw`, `total_fees_raw`, `total_funding_raw`, `pnl_carry_raw` | the portfolio totals |
| `account` | the instrument's venue, the pool's primary (version 10) |

### kill_events

One row per trip, keyed `(session_id, seq)`.

| Column | Meaning |
|---|---|
| `scope` | `global` or `venue` |
| `venue_id` | -1 for a global trip |
| `reason` | the [`KillReason`](errors.md#kill-reasons) |
| `kill_flags` | the flag word |
| PnL columns | the PnL at the time |

### pnl_daily

One row per `(session_id, day, instrument_id)`, maintained as position records arrive. These are the engine's own books.

| Column | Meaning |
|---|---|
| `realized_raw`, `fees_raw`, `gross_traded_raw`, `fills`, `funding_raw` | the change within that UTC day; `funding_raw` is part of `realized_raw` |
| `unrealized_raw`, `qty_raw` | the day's last snapshot |
| `symbol`, `settlement_ccy`, `account` | from `instruments` |

A change that straddles midnight lands on the day of the snapshot that reports it. Do not add PnL across settlement currencies: group by `settlement_ccy` ([Risk model](../explanation/risk-model.md)).

### param_schema, param_updates, param_values

The strategy parameters over time: the starting set and every update applied (version 8).

| Table | One row per | Columns |
|---|---|---|
| `param_schema` | `(session_id, field)` | `name`, `type` (`int`, `double`, `bool`, `decimal`, `bps`, `ms`) |
| `param_updates` | `(session_id, seq)` | `ts_ns`, `instrument_id`, `symbol`, `origin`, `source`, `publish_seq`, `fields`, `changed`, `complete`, `arm` |
| `param_values` | `(session_id, seq, name)` | `value` as text, `raw` (NULL when not known), `changed` |

`param_updates` columns:

| Column | Meaning |
|---|---|
| `seq` | 0 for the starting set, else the record's `seq` |
| `origin` | `initial`, `control`, `ab` or `strategy` |
| `source` | `fastmm-ctl param --source`, or `ab:<test>` |
| `complete` | on `seq` 0: 1 when every parameter is known, 0 when the strategy brings its own publisher |
| `instrument_id`, `symbol` | the target; NULL for every instrument |
| `fields`, `changed` | values carried, values changed |
| `arm` | the arm the update put its instruments in, `''` out of the test, NULL when not the test's (version 12) |

A control or `[ab]` update is stored whole. A strategy publisher's update keeps only changed values and writes nothing when none changed. An update to every instrument replaces what single instruments had.

### rejects

One row per refused order, keyed `(session_id, seq)` (version 9).

| Column | Meaning |
|---|---|
| `ts_ns`, `account`, `instrument_id`, `symbol`, `side` | when, where the order went, what |
| `kind` | `new` or `replace` |
| `reduces`, `reduce_only` | the order only moved the position towards zero; it was flagged reduce-only |
| `reason` | the [`RejectReason`](errors.md) |
| `source` | what refused it (below) |
| `count` | refusals folded into the row; `SUM(count)` is exact |
| `price_raw`, `qty_raw`, `cl_ord_id` | the order; `cl_ord_id` on a venue's refusal |
| `venue_code`, `text` | the venue's code and text |
| `budget_known`, `paused`, `tokens`, `token_wait_us` | the budget the engine saw; `tokens` is NULL with the limit off |
| `orders_10s_used`, `orders_10s_admits`, `orders_1m_*`, `orders_1d_*`, `weight_*` | each window's count and cap; 0 admits means the window is not known |

| `source` | Refused by |
|---|---|
| `risk` | a pre-trade check other than the rate limits |
| `risk_bucket` | the `[risk] orders_per_sec` bucket, shared by every account |
| `account_paused` | the account's connector is paused (429, 418) |
| `account_orders_10s`, `account_orders_1m`, `account_orders_1d` | the account's order window at the connector's cap ([account pools](configuration.md#account-pools)) |
| `venue_local` | the connector's own limiter; `text` names the window |
| `venue` | the venue |

Refusals with the same instrument, side, account, reason, source and flags within 100 ms of the row are folded into it. A pending count is written at the next once-a-second publication or at the session's end.

### ab_arms

One row per arm of the session's `[ab]` test, keyed `(session_id, arm)` (version 12). `fastmm-pnl fills` and `orders` show each row's arm; markouts need the journal ([Run an A/B test](../how-to/strategies/ab-test.md#read-it-out)).

| Column | Meaning |
|---|---|
| `arm` | its index; 0 is the control |
| `test`, `name` | the test's and the arm's names |
| `params` | the arm's parameters as space-separated `name=value`; empty for the configured ones |

### limit_changes

One row per `[risk]` key a `fastmm-ctl limits` changed, keyed `(session_id, seq, key)` (version 11). `fastmm-pnl limit-changes` lists them.

| Column | Meaning |
|---|---|
| `ts_ns`, `day` | when the engine applied it |
| `key` | `max_loss`, `check_balance`, `underlying.BTC.max_net`; empty when the command changed nothing |
| `before`, `after` | the values as `[risk]` writes them |
| `forced` | 1 when `--force` turned a limit off |

### Views

| View | Rows |
|---|---|
| `reject_summary` | `day`, `account`, `symbol`, `side`, `reduces`, `reason`, `source` and `rejects`, the summed `count` |
| `param_history` | `param_updates` joined with `param_values`: one row per value of every update |
| `pnl_by_day` | `day`, `account`, `symbol`, `settlement_ccy`, `realized_raw`, `funding_raw`, `fees_raw`, `net_raw`, `gross_traded_raw`, `fills`, summed over sessions (`fastmm-pnl pnl --books`) |
| `pnl_by_currency` | the same by `day` and `settlement_ccy` |
| `open_orders` | `orders` with `terminal = 0` |

## Writing a backend

1. Implement `fastmm::store::Backend` (`include/fastmm/store/backend.hpp`) and, to be queryable, `fastmm::store::Reader` (`include/fastmm/store/reader.hpp`).
2. Register both factories from your `main`, before `run_live`; nothing registers itself, and the name `none` is reserved:

    ```cpp
    fastmm::store::StoreRegistry::instance().add("clickhouse", &make_ch_backend, &make_ch_reader);
    ```

3. Read your keys in `Backend::open` from `BackendOptions`: the `[storage]` section verbatim, `[engine] name` and `[engine] journal_dir`.

`fastmm::store::register_builtin_backends()` adds the backends FastMM ships. Every call runs off the engine thread and may allocate, in this order: `open`, `session_open`, `instruments`, then `begin`, records and `commit` repeatedly, then `session_close` and `close`. A `Reader` opens the store read-only and answers `sessions`, `fills`, `orders`, `pnl`, `funding`, `positions` and `recovery`; each query takes a `QueryFilter` and returns string rows.

## Recovery at start-up

`fastmm-live` reads the store before any session thread starts and logs what the previous session of the same `[engine] name` left: its PnL, clean shutdown or not, kill state, journal parts, last position per instrument and its open orders. `fastmm-pnl recover --engine <name>` prints the same.

1. Read the store and log the previous session.
2. With `restore_position`, restore the last position per venue and symbol.
3. Replay each venue's executions from its resume point.
4. Take the open-order snapshot.
5. Turn quoting on.

With `[engine] restore_position` the session carries over the last position per venue and symbol ([What survives a restart](../how-to/operations/running-in-production.md#restarts)). The position is the newest stored row for that venue and symbol, a position row or a fill: each fill row carries the position it left.

Until every venue that replays executions has finished its first reconciliation, the engine sends no order and new orders are refused with `NotReconciled`. The log says `every venue has reconciled its orders and executions since the start` when it ends.

### Where a venue's replay starts

The resume point goes to the connector's execution replay, and in the attach request behind a gateway.

| Case | Replay starts |
|---|---|
| a fill or funding payment stored | the venue time of the last one, less 1 s, skipping the trade and funding ids stored from there on, by every session of the engine |
| a later session died during its replay | no later than the earliest of that session's last fills; its fills are skipped as known |
| more than 128 ids in that second | later, past the oldest millisecond that does not fit, with a log line |
| Binance Spot and USDⓈ-M, in process | per symbol at the trade id after the highest stored (`fromId`), skipping higher ids stored since |
| no stored fill | the start of the newest clean session, else the oldest session, less 10 s |
| a store from before version 3 | 10 s before the session's last fill, skipping the ids of the 20 s before it |

The last fill counts from the newest session that finished its replay on the venue (`replayed_seq` > 0). The 1 s margin covers a venue publishing executions slightly out of trade-time order. Behind a gateway the venue is shared, and the gateway filters the replay by time and ids.

### Duplicates

A venue execution or funding payment stored in more than one session was booked twice by a restart. The start-up summary logs each one as an error (the first 32) with their count; `fastmm-pnl duplicates` lists them. Rows are not changed. The positions and PnL of the sessions holding the copies count them twice, until the venue reports its own position.

The open orders the recovery lists are those FastMM last saw open; the venue's view arrives with the reconciliation. The `[risk] max_loss` budget is carried by the kill-state file (`include/fastmm/core/session_state.hpp`), so it survives with `backend = "none"`.

## Tools

| Tool | Answers |
|---|---|
| `fastmm-pnl pnl` | each day's PnL recomputed from `fills` at average cost per account and symbol: an execution once, an estimate only for what no execution named; prints `net` and the engine's `books_net` |
| `fastmm-pnl pnl --books` | `pnl_daily` alone, which counts a repeat twice |
| `fastmm-pnl ledger` | one row per execution across sessions, with its copies and flags ([Query what you traded](../how-to/operations/query-trading-records.md#the-economic-ledger)) |
| `fastmm-pnl order --order <id>` | `orders`, `rejects` and `fills` of one client order |
| `fastmm-pnl` | the other daily questions ([command lines](cli.md), [Query what you traded](../how-to/operations/query-trading-records.md)) |
| `fastmm.open_store(path)` | pandas DataFrames ([Python API](python-api.md)) |
| `sqlite3 runs/mm1.db` | anything else; the schema above is the contract |
