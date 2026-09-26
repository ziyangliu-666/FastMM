# Storage

The store holds what a session traded as rows a query can answer: fills, orders and their terminal state, position snapshots, per-day and per-session PnL, kill events and session metadata. It survives a restart and is read without replaying a journal. Code: `include/fastmm/store/`, `src/store/`.

Two records, two jobs:

| | Journal (`.fmj`) | Store |
|---|---|---|
| Holds | every event the engine consumed and every message it sent, byte for byte | the interpreted result: fills, orders, positions, PnL, kill events |
| Answers | "does this session reproduce exactly?" (`fastmm-replay`) | "what did I trade yesterday?" (`fastmm-pnl`, `fastmm.open_store`) |
| Written by | `JournalFileWriter` on the `fm-journal` thread | a storage backend on the `fm-store` thread |
| Loss policy | a full ring trips the kill switch: replay needs every event | a full ring drops the record and counts it |
| Authority | yes | no: every row is derivable from the journal, though no tool rebuilds one |

Prices, quantities and PnL are raw fixed-point `int64` (1e-8) in the columns whose name ends in `_raw`; timestamps are `int64` nanoseconds since the Unix epoch in the columns whose name ends in `_ns`. `day` is the UTC day of the row's timestamp, denormalised so a day query needs no date arithmetic.

## Configuration

`[storage]` is free-form, like `[sim]` and `[backtest]`: the central schema (`include/fastmm/config/schema.hpp`) knows no key in it, and each backend reads its own.

```toml
[storage]
backend = "sqlite"        # "none" disables the store
path = "runs/mm1.db"      # sqlite: default "<[engine] journal_dir>/<[engine] name>.db"
ring_bytes = 4194304      # engine-to-store ring, a power of two (default 4194304)
```

| Key | Backend | Meaning |
|---|---|---|
| `backend` | all | backend name, or `none` (default `sqlite`) |
| `ring_bytes` | all | bytes of the ring the engine writes records into, a power of two (default 4194304) |
| `path` | sqlite | store file (default `<journal_dir>/<engine name>.db`) |

`backend = "none"` allocates no ring and starts no thread, and the engine's `RecordWriter` is disabled. A backend that cannot be opened stops the session before it trades, with exit code 3.

## How a record reaches the store

```text
engine thread --RecordWriter--> MsgRing --fm-store thread--> Backend --> rows
```

The engine copies a trivially copyable record into an SPSC ring, as it writes the journal: no allocation, no syscall, no wait. `StoreThread` drains the ring, batches up to 512 records into one backend transaction, and sleeps 2 ms when the ring is empty. Records are `include/fastmm/core/record_stream.hpp`:

| Record | Written when | Carries |
|---|---|---|
| `FillRecord` (256 B) | every execution, and every quantity the engine books from a `cum_qty` jump | price, quantity, booked quantity, fee and fee asset, liquidity, venue order id, exec id, the venue's time of the trade (`RecordHeader::exch_ts`), and the position it left behind |
| `OrderRecord` (192 B) | every OMS state change | the whole `Order`: state, previous state, price, quantity, filled quantity, reject reason |
| `PositionRecord` (192 B) | after every fill, after a venue position snapshot, and once per instrument at `finish()` | the instrument's position and the portfolio totals |
| `FundingRecord` (192 B) | every funding payment the engine books | amount, asset, venue funding id, the venue's time, and the instrument's position, realized PnL and funding after it |
| `KillRecord` (128 B) | every kill switch trip, global or per venue | the reason, the flag word and the PnL at the time |

Session metadata (`session_open`, `session_close`, the instrument table) is written by the control thread, not through the ring.

## What a backend guarantees

- **Ordering.** Records arrive on one thread in the order the engine made them; `seq` is strictly increasing within a session.
- **At most once.** A record the ring dropped, or a batch the process died inside of, is gone. A backend must not invent one. Records carry keys (session id plus `seq`, and a venue exec id on a fill) so re-ingesting the same record is a no-op.
- **No back pressure.** The store never signals the engine. A ring that fills drops records on the engine side and counts them in `EngineStats::records_dropped`; the count is logged at shutdown and stored in `sessions.records_dropped`. A backend that fails counts the error, logs the first one and keeps going. Neither stops trading: the journal is the authority for anything lost.
- **No exceptions across the interface** and no `abort()`.

`fastmm-live` logs the totals when a session ends:

```text
store: 1284 record(s) in 37 batch(es), 1284 row(s), 0 error(s), 0 dropped
```

## Durability

The SQLite backend opens the file with `journal_mode=WAL` and `synchronous=NORMAL`, and checkpoints every 256 pages.

- A **committed batch survives the process dying** (a crash, `SIGKILL`, an `abort()`): the write-ahead log is in the page cache and the next open replays it.
- A batch the process died **inside of** is rolled back whole: no partial batch is ever visible.
- **Power loss** can cost the batches written since the last checkpoint. `synchronous=FULL` would fsync the log on every commit; the journal already holds every record, so the store does not pay for it.
- A **reader never blocks the writer** and never sees a half-written batch: `fastmm-pnl` and a notebook can query a store while the session writes it.

## Schema

Version 4 (`kSqliteSchemaVersion`). The SQL is `src/store/sqlite_schema.cpp`, one migration step per version; an existing store is migrated in place at open, and a store written by a newer FastMM is refused with the version it holds.

### sessions

One row per session, written at start and completed at shutdown.

| Column | Meaning |
|---|---|
| `session_id` | the engine's session id (primary key) |
| `engine`, `strategy` | `[engine] name` and the strategy that ran |
| `session_epoch` | client order id epoch of this session |
| `started_ns`, `started_day`, `stopped_ns` | start, its UTC day, and stop; `stopped_ns` is null when the process did not shut down |
| `version`, `build` | `FASTMM_VERSION_STRING` and the build info |
| `config_hash`, `config_toml` | the effective configuration (secrets omitted) and its hash, as in the journal header |
| `host`, `pid`, `dry_run`, `pnl_carry_raw` | where it ran, and the net PnL carried in from earlier sessions |
| `clean_shutdown` | 1 when `session_close` ran; 0 means the process was killed |
| `exit_code`, `kill_reason`, `kill_latched` | how it ended; a clean shutdown asks for the kill switch itself, so `kill_reason` is then `Requested` |
| `journal_complete`, `journal_bytes` | whether the journal was closed cleanly, and its size over every part |
| `events`, `orders_sent`, `cancels_sent`, `replaces_sent`, `fills`, `risk_rejects`, `venue_rejects` | the runner's counters |
| `records_dropped` | store records the ring could not take: the rows below are incomplete by that many |
| `realized_raw`, `unrealized_raw`, `fees_raw` | the session's final PnL |
| `funding_raw` | the part of `realized_raw` that is funding; null for sessions from before version 4 |

### session_journals

`(session_id, part, path)`: the journal parts the session wrote, in order.

### session_venues

`(session_id, venue_id, name)`: the `[venues.<name>]` behind each `venue_id` of the session, so a restart finds its venues by name. Version 3; sessions recorded before it have none.

### instruments

`(session_id, instrument_id)` and the instrument as the session loaded it: `venue_id`, `symbol`, `base`, `quote`, `settlement_ccy`, `asset_class`, `inverse`, `tick_raw`, `lot_raw`, `multiplier_raw`. Per session, because a restart may load different reference data.

### fills

One row per execution, keyed `(session_id, seq)`, with a unique index on `(session_id, exec_id)` for the rows that have one.

| Column | Meaning |
|---|---|
| `ts_ns`, `day`, `symbol`, `venue_id`, `instrument_id` | when (the engine's clock), what and where |
| `exch_ns` | the venue's time of the trade, in the venue's clock; 0 for a synthetic fill, a connector that reports none, and rows from before version 3 |
| `cl_ord_id`, `venue_order_id`, `exec_id` | the ids the venue and FastMM know it by |
| `side`, `liquidity` | `Buy`/`Sell`, `Maker`/`Taker`/`Unknown` |
| `price_raw`, `qty_raw`, `booked_qty_raw` | the execution, and the quantity the position moved by (a base-asset fee is deducted) |
| `cum_qty_raw`, `leaves_qty_raw` | the order after it |
| `fee_raw`, `fee_amount_raw`, `fee_asset` | the fee booked in the settlement currency, the amount the venue reported, and its asset (`quote`, `base`, `other`; an `other` fee is reported but not booked) |
| `position_qty_raw`, `position_avg_px_raw`, `position_realized_raw`, `position_fees_raw` | the instrument's position after this fill |
| `synthetic` | 1 for a quantity the engine booked from a `cum_qty` jump, at the order's own price and with no fee |
| `late` | 1 for a fill that arrived after the order was already terminal |

### funding

One row per perpetual funding payment the engine booked, keyed `(session_id, seq)`, with a unique index on `(session_id, instrument_id, funding_id)`. Version 4.

| Column | Meaning |
|---|---|
| `ts_ns`, `day`, `symbol`, `venue_id`, `instrument_id` | when (the engine's clock), on what, where |
| `exch_ns` | the venue's time of the payment |
| `funding_id` | the venue's id of it (Binance `tranId`, Bybit `execId`) |
| `amount_raw`, `asset` | the payment in the settlement currency: negative paid, positive received |
| `position_qty_raw`, `position_realized_raw`, `position_funding_raw` | the position it was paid on, and the instrument's realized PnL and funding after it |
| `replayed` | 1 when it came from the venue's history rather than its private stream |

Funding is realized PnL (not a fee), so it is inside every `realized_raw` below; the `funding_raw` columns say how much of it.

### orders

One row per order, keyed `(session_id, cl_ord_id)`, updated in place: the row holds the last state the session saw and `updates` counts how many changes it went through. `terminal` is 1 for `Filled`, `Canceled`, `Rejected`, `Expired` and `Replaced`; the `open_orders` view selects the rest.

`Replaced` is the store's own state, not an `OrderState`: a cancel-replace to a new client order id renames the order in place, so the id it superseded never reports a terminal state of its own. Its row keeps the price and quantity it had and is closed when the replacement is acked. Without it every replaced quote would read as still open.

### positions

One row per position snapshot, keyed `(session_id, seq)`: `qty_raw`, `avg_px_raw`, `realized_raw`, `unrealized_raw`, `fees_raw`, `gross_traded_raw`, `fills`, the portfolio totals `total_realized_raw`, `total_unrealized_raw`, `total_fees_raw`, `pnl_carry_raw`, and (version 4) `funding_raw` and `total_funding_raw`.

### kill_events

One row per trip, keyed `(session_id, seq)`: `scope` (`global` or `venue`), `venue_id` (-1 for a global trip), `reason` ([`KillReason`](errors.md#kill-reasons)), `kill_flags` and the PnL at the time.

### pnl_daily

One row per `(session_id, day, instrument_id)`, maintained as position records arrive.

| Column | Meaning |
|---|---|
| `realized_raw`, `fees_raw`, `gross_traded_raw`, `fills`, `funding_raw` | the change within that UTC day, not a running total (`funding_raw`, version 4, is part of `realized_raw`) |
| `unrealized_raw`, `qty_raw` | the last snapshot of the day: a mark, not a flow |
| `symbol`, `settlement_ccy` | denormalised from `instruments` |

A change that straddles midnight lands on the day of the snapshot that reports it. Realised PnL and fees of instruments that settle in different currencies must not be added: group by `settlement_ccy` ([the risk model](../explanation/risk-model.md) says why).

### Views

| View | Rows |
|---|---|
| `pnl_by_day` | `day`, `symbol`, `settlement_ccy`, `realized_raw`, `funding_raw`, `fees_raw`, `net_raw`, `gross_traded_raw`, `fills`, summed over sessions |
| `pnl_by_currency` | the same by `day` and `settlement_ccy` |
| `open_orders` | `orders` with `terminal = 0` |

## Writing a backend

A backend implements `fastmm::store::Backend` (`include/fastmm/store/backend.hpp`) and, to be queryable, `fastmm::store::Reader` (`include/fastmm/store/reader.hpp`), then registers two factories before a session starts:

```cpp
fastmm::store::StoreRegistry::instance().add("clickhouse", &make_ch_backend, &make_ch_reader);
```

Nothing registers itself: the linker drops a static library's self-registering object. `fastmm::store::register_builtin_backends()` adds the ones FastMM ships; an out-of-tree backend calls `add()` from its own `main` before `run_live`. The name `none` is reserved.

`Backend::open` receives a `BackendOptions` holding the `[storage]` section verbatim, `[engine] name` and `[engine] journal_dir`: a backend parses its own keys from `GenericSection` and nothing is added to the central schema. Everything the interface calls runs off the engine thread and may allocate.

The call order is `open`, `session_open`, `instruments`, then `begin` / records / `commit` repeatedly, then `session_close` and `close`. A `Reader` opens the same store read-only and answers `sessions`, `fills`, `orders`, `pnl`, `funding`, `positions` and `recovery`; every query takes the same `QueryFilter` and returns string rows.

## Recovery at start-up

`fastmm-live` reads the store before the first session thread starts and logs what the previous session of the same `[engine] name` left behind: its PnL, whether it shut down cleanly, the kill state, the journal parts, the last position per instrument, and every order that was still open at its last record. `fastmm-pnl recover --engine <name>` prints the same thing.

With `[engine] restore_position` the session also carries the last position per instrument over and books what happened while it was down from the venue's trade history ([What survives a restart](../how-to/operations/running-in-production.md#1-what-survives-a-restart)). Where each venue's replay starts (`Recovery::venue_resume`, passed to `Venue::resume_executions`, and in the attach request behind a gateway):

- **In the venue's clock.** From `exch_ns` of the venue's last stored fill or funding payment, less 1 s, skipping the trade ids and funding ids (as `funding:<id>`) stored from there on. The funding replay starts there too, so a payment made while nothing ran is booked by the next session and one the store holds is not booked again. Both ends are venue time, so the host's clock does not enter. The 1 s covers a venue publishing executions out of trade-time order (other symbols, a batch), which is milliseconds.
- **At most 128 ids a venue.** When more stored fills fall in that second, the start moves later, past the oldest millisecond that does not fit whole, and the session logs it: an id left out would be booked twice.
- **Binance Spot and USDⓈ-M** resume each symbol at the trade id after the highest one stored (`fromId`), with no overlap and no ids (`Venue::resume_trade_ids`). In-process only: a gateway's venue is shared, and it filters another attachment's replay for this one by time and ids.
- **A store from before version 3**, or a venue whose fills carry no venue time, starts from the engine clock as before: 10 s before the session's last fill, skipping the ids of the 20 s before it.

What the store cannot tell you:

- **Whether the venue still holds the open orders.** The orders the recovery lists are the ones FastMM last saw open; the venue may have cancelled, filled or expired them since.
- **The position, authoritatively.** It is FastMM's view at the last record. The venue's view arrives with the reconciliation.

The loss budget `[risk] max_loss` is carried across restarts by the kill-state file (`include/fastmm/core/session_state.hpp`), not by the store: it is read before any store is opened, so a session with `backend = "none"` still carries it.

## Tools

- `fastmm-pnl` ([command lines](cli.md), [the daily questions](../how-to/operations/query-trading-records.md)).
- `fastmm.open_store(path)` returns pandas DataFrames ([Python API](python-api.md)).
- `sqlite3 runs/mm1.db` for anything the two do not cover; the schema above is the contract.
