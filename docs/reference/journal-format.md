# Journal format

The `.fmj` journal records every event a session's engine consumed, in consumption order, with the engine clock, plus a copy of every order message it sent. Format version 4; readers also open versions 1 to 3. The decision and its history are in [ADR-0010](../adr/0010-fmj-journal-format.md); the code is `include/fastmm/core/journal.hpp`.

All integers are little-endian. Prices, quantities and notionals are raw fixed-point `int64` (1e-8), timestamps are `int64` nanoseconds since the Unix epoch.

## File layout

```text
file   := header (256 B)
          | instrument[instrument_count] (128 B each)
          | config (config_bytes, padded to 64)
          | params (param_table_bytes, padded to 64)
          | meta (meta_bytes, padded to 64)
          | block* | trailer
block  := block header (64 B) | message* (byte_len bytes)
```

- `header_bytes` in the header is the offset of the first block.
- Blocks are at most 1 MiB (`block_bytes`); a message never straddles two blocks.
- A clean shutdown appends a trailer: an empty block with flag bit 0 set. Without it the file was not closed cleanly; a reader drops a tail block whose checksum or length does not match.
- `JournalReader::complete()` is true only for a file that ends in a trailer with no block discarded. `fastmm-replay` refuses an incomplete file unless `--allow-incomplete` is given: its outbound stream stops short of what the session sent.

## Durability

`[engine] journal_sync` chooses how far a write is pushed. The journal batches into 1 MiB blocks, so no record is durable at the instant the engine writes it.

| `journal_sync` | Every 100 ms | A record survives | Costs |
|---|---|---|---|
| `async` (default) | `msync(MS_ASYNC)` | this process dying, as soon as its block is copied into the map | nothing measurable |
| `fdatasync` | `msync(MS_SYNC)` and `fdatasync()` | power loss, once it is more than one tick old | one write-back of the dirty pages per tick |

A clean `stop()` always writes the trailer, `msync(MS_SYNC)`s, truncates the preallocated tail away and `fdatasync()`s, in both modes.

Extents are reserved with `posix_fallocate`, not `ftruncate`: a full filesystem fails when the next 64 MiB extent is reserved rather than with `SIGBUS` on the first store into a sparse page. `JournalFileWriter::failed()` latches that and every other write error, and `fastmm-live` polls it: a journal that cannot be written trips the kill switch, cancels everything and exits with code 5. `sessions.journal_complete` in the [store](storage.md) records whether the file was closed cleanly.

## Rotation and retention

`[engine] journal_max_bytes` rolls the file over when it reaches that size; 0 (the default) writes one file per session. A part only ends between blocks, so no message straddles two files. The first part keeps the configured name and the rest get a number before the extension:

```text
runs/mm1-1709510400123456789.fmj      part 0
runs/mm1-1709510400123456789.1.fmj    part 1
```

Each part is a complete journal: it repeats the header, the instrument table, the configuration and the parameter table, and carries the same `session_id`. Sequence numbers continue across parts. The backtest's `journal:` source reads the parts of a session as one stream, given the first ([data sources](data-sources.md#journal)); `fastmm-replay` verifies a session journal one part at a time; the [store](storage.md) records every part of a session in `session_journals`.

`[engine] journal_retention_days` deletes `*.fmj` files in `[engine] journal_dir` that were last written more than that many days ago, once, when a session starts; 0 (the default) keeps everything. Only the `.fmj` extension is removed, and a file it cannot remove is skipped with a warning.

## Header

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `magic` | `FMJ1` |
| 4 | 4 | `version` | 4 (1 to 3 for old files) |
| 8 | 4 | `header_bytes` | offset of the first block |
| 12 | 4 | `instrument_count` | instruments that follow the header |
| 16 | 8 | `session_id` | session identifier |
| 24 | 8 | `start_ts_ns` | wall-clock start time |
| 32 | 8 | `tsc0` | TSC reading of the calibration anchor |
| 40 | 8 | `tsc_ns0` | wall-clock ns of that anchor |
| 48 | 8 | `tsc_ns_per_cycle_q32` | ns per TSC cycle, 32.32 fixed point |
| 56 | 8 | `config_hash` | FNV-1a hash of the embedded configuration text |
| 64 | 8 | `rng_seed` | the engine's RNG seed |
| 72 | 4 | `message_version` | version of the message layouts |
| 76 | 4 | `block_bytes` | maximum block size (1048576) |
| 80 | 32 | `strategy` | strategy name, NUL-padded |
| 112 | 2 | `session_epoch` | client order id epoch (v2) |
| 114 | 1 | `quoting_enabled` | 0 for a dry run behind a gateway; a dry run of its own quotes on paper (v2) |
| 115 | 1 | `header_flags` | bit 0: the three session fields above are valid (v2); bit 1: the engine replaced quotes per venue (without it, a replay replaces only if every traded venue could, as the engine did then); bit 2: the session's venue feeds showed its own orders (a backtest with `[backtest] own_orders_in_feed`; a live session's always do) |
| 116 | 4 | `config_bytes` | length of the configuration text; 0 = none (v2) |
| 120 | 8 | `replace_venues` | bit v set when venue v traded with cancel-replace (v2) |
| 128 | 4 | `config_crc32c` | CRC32C of the configuration text (v2) |
| 132 | 4 | `param_count` | entries of the parameter table (v3) |
| 136 | 4 | `param_table_bytes` | length of the parameter table; 0 = none (v3) |
| 140 | 4 | `param_table_crc32c` | CRC32C of the parameter table (v3) |
| 144 | 4 | `meta_bytes` | length of the strategy metadata; 0 = none (v3) |
| 148 | 4 | `meta_crc32c` | CRC32C of the strategy metadata (v3) |
| 152 | 100 | `reserved` | zero |
| 252 | 4 | `crc32c` | CRC32C of bytes 0 to 251 |

The configuration is `Config::effective_toml()`: the configuration after command-line overrides (`--strategy`, `--param`) as deterministic TOML, without `api_key`, `api_secret` and `api_passphrase`, zero-padded to a multiple of 64 bytes. The instrument records are `fastmm::Instrument` (128 bytes).

The parameter table lists the strategy's parameters in schema order. Each entry is the type (`uint8`: 0 `int`, 1 `double`, 2 `bool`, 3 `decimal`, 4 `bps`, 5 `ms`), the name length (`uint8`) and the name; the table is zero-padded to a multiple of 64 bytes.

The strategy metadata is UTF-8 `key=value` lines, zero-padded to a multiple of 64 bytes. A Python strategy run live writes `class` (`module:qualname`), `hot_source_sha256`, `fastmm`, `numba`, `llvmlite` and `python`; `fastmm.inspect_journal(path)["strategy_meta"]` returns them as a dict.

## Block header

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `magic` | `FMJB` |
| 4 | 4 | `byte_len` | payload bytes after this header |
| 8 | 8 | `seq_first` | sequence number of the first message |
| 16 | 8 | `seq_last` | sequence number of the last message |
| 24 | 4 | `count` | messages in the block |
| 28 | 4 | `crc32c` | CRC32C of the payload |
| 32 | 4 | `flags` | bit 0: trailer |
| 36 | 28 | `reserved` | zero |

## Messages

Every message starts with the 64-byte `EventHeader`; its total length (`len`) is a multiple of 64.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `len` | message bytes including the header |
| 4 | 1 | `type` | `EventType` |
| 5 | 1 | `version` | message layout version |
| 6 | 1 | `venue` | venue id |
| 7 | 1 | `flags` | see below |
| 8 | 4 | `instrument` | instrument id |
| 12 | 4 | `reserved0` | with `kEngineTime`: engine clock delta, signed int32 ns |
| 16 | 8 | `seq` | journal sequence number, consumption order |
| 24 | 8 | `venue_seq` | venue update id or sequence |
| 32 | 8 | `exch_ts` | venue event time |
| 40 | 8 | `recv_ts` | receive time |
| 48 | 8 | `t0_cycles` | TSC at receive |
| 56 | 8 | `t1_delta`, `t2_delta` | cycles to decode and to book apply, `uint32` each |

| Flag bit | Name | Meaning |
|---:|---|---|
| 0 | `kSynthetic` | generated locally (a timer, a reconciliation fill, the simulator) |
| 1 | `kReplayed` | read from a journal |
| 2 | `kSnapshot` | a book message that carries a full snapshot |
| 3 | `kOutbound` | a copy of an order message the engine sent (`OutNewOrder`, `OutCancel`, `OutReplace`) |
| 4 | `kEngineTime` | a consumed event; `reserved0` is the engine clock minus the previous engine-time record |
| 5 | `kDropped` | an outbound copy the transport did not accept |
| 6, 7 | `kTruncatedBids`, `kTruncatedAsks` | a book delta whose side kept only the levels nearest the touch of a longer venue update; a book drops its levels behind the last one carried |

Message layouts are the structs in `include/fastmm/core/messages.hpp` (`EventType` in `core/enums.hpp`): book snapshots and deltas, trades, book tickers, option tickers, L3 order adds, executions, cancels and replaces, order acks, rejects, cancel acks and rejects, fills, expiries, positions, timers, control commands, connection states, reconciliation records, latency samples, outbound orders, parameter updates, funding payments (`FundingMsg`, 128 bytes: the signed amount in the settlement asset at offset 64, the venue's funding id, the asset, and flag bit 0 when it came from the venue's history) balances (`BalanceMsg`, 128 bytes: free, locked, total, equity and maintenance at offsets 64 to 96, the asset at 104, flags at 113: bit 0 snapshot, bit 1 snapshot end, bit 2 account-wide margin), money that entered or left an account other than by its trades and funding (`AccountFlowMsg`, type 31, 128 bytes, format v4: the signed amount at 64, positive in, the asset at 72, the venue's id of it at 81; `venue` is the account, `exch_ts` the venue's time; Binance Spot reports it as `balanceUpdate`, Binance USDⓈ-M as the `bc` of an `ACCOUNT_UPDATE` that is not for an order or a funding payment) and a venue's mark, index and funding (`PerpStateMsg`, 128 bytes: mark and index price at 64 and 72, the funding rate (f64, per interval) at 80, the interval (ns) at 88, the next funding time at 96, open interest at 104, and at 112 the fields it carries: bit 0 mark, bit 1 index, bit 2 funding, bit 3 open interest).

## Trades

A `Trade` message (`TradeMsg`) is 128 bytes: price at offset 64, quantity at 72, the venue's trade id at 80, the aggressor's side at 88. Where the venue publishes who traded (Hyperliquid, Lighter), the record is 192 bytes and carries `TradeParties` at offset 128: the buyer at 128 and the seller at 160, 32 bytes each.

| Offset in the party | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | `kind` | 0 not named, 1 `Account` (a numeric account id), 2 `Address` (an address or public key) |
| 1 | 1 | `len` | bytes of `bytes` in use: 8 for an account, the address's length (20 on Hyperliquid) |
| 2 | 30 | `bytes` | an account as a little-endian `uint64`; an address as its raw bytes, first byte first |

`hdr.len` tells the two apart, so the format version did not change: a journal written before parties existed reads as trades that name nobody, and an older reader steps over the 64 bytes it does not know.

## Engine clock

- Every consumed event and every fired timer (a synthetic `Timer` message) carries `kEngineTime` and a delta from the previous such record.
- An `EngineTime` message (128 bytes) holds the absolute engine clock in `engine_ts` (offset 64) with `kind` (offset 72): 0 `Sync` before an event whose delta does not fit in int32 ns (a gap of more than 2.1 s) or whose predecessor was lost, 1 `Start`, 2 `Finish`.
- The engine clock may step backwards (TSC recalibration); replay sets the simulated clock to it.
- Sequence numbers count `EngineTime` records too.
- A `Timer` message with byte 68 (`engine`) set to 1 is the engine's `max_param_age_ms` deadline, not a strategy timer.

## Strategy state

A `StrategyState` message (type 30, 128 bytes followed by its bytes, padded to 64) records what a replay or a backtest can start from. It is never dispatched to the engine.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 64 | 8 | `id` | the n-th state the session wrote; the records of one state share it |
| 72 | 4 | `total` | bytes of the part |
| 76 | 4 | `offset` | where this record's bytes go in the part |
| 80 | 4 | `bytes` | bytes after the 128-byte head, at most 16384 |
| 84 | 1 | `kind` | 0 `Restored`: the bytes the strategy was handed to `restore()` at the start; 1 `Snapshot`: a checkpoint |
| 85 | 1 | `part` | 0 the strategy's bytes; 1 positions, 24 bytes each: instrument id (u32), venue (u8), 3 pad, quantity and average price (fixed point, i64); 2 (v4) open orders, 32 bytes each: client order id (u64), instrument id (u32), the account's venue id (u8), side, `OrderState` and order flags (u8 each), price and quantity left (fixed point, i64) |
| 86 | 1 | `flags` | bit 0 (`Restored`): the strategy took the bytes |

The engine writes the `Restored` state (its strategy part only) once, after `on_start`, when `[strategy] state_file` held something. A checkpoint is a `Snapshot` of all three parts: every `[engine] checkpoint_s` ([Configuration](configuration.md#engine)), and at a warm standby's take-over, right after the `TakeOver` control record, with the state it restored. A strategy that keeps no state writes no bytes in part 0. A checkpoint of format v3 has no part 2. `recv_ts` is the engine time it was taken at; the records of one state are written back to back. `fastmm-replay` stages the strategy bytes of the checkpoint after each `TakeOver` for the take-over to restore.

## Parameter updates

A `ParamUpdate` message (448 bytes) is an engine input with new strategy parameter values; the engine assigns all of them at that event.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 8 | 4 | `instrument` | the target instrument; `0xFFFFFFFF` for all instruments |
| 64 | 4 | `count` | pairs used, at most 32 |
| 68 | 1 | `origin` | 0 the strategy's own publisher (or not recorded), 1 the control socket (`fastmm-ctl param`), 2 the `[ab]` test |
| 69 | 1 | `arm` | the `[ab]` arm it puts its instruments in: 0 none named, k + 1 arm k, 255 out of the test |
| 72 | 8 | `publish_seq` | the publisher's sequence number, from 1 |
| 80 | 64 | `field` | `uint16` each: the parameter's index in the parameter table |
| 144 | 40 | `source` | who asked for it (`fastmm-ctl param --source`), NUL-terminated; empty when not given |
| 192 | 256 | `value` | `int64` each: the raw value |

`origin`, `arm` and `source` sit in bytes that were padding before, so a journal written before them reads with origin 0, no arm and an empty source. The order messages carry the arm their order was sent under in a byte that was padding as well: `OutNewOrder` at offset 92, `OutReplace` at 144 (0 none, k + 1 arm k).

A raw value is the value of an `int` or `bool`, raw fixed point for `decimal` and `bps` (1 bp = 10000), nanoseconds for `ms` and the IEEE-754 bits of a `double`. Replay maps each field through the parameter table to the replaying strategy's parameter with the same name and type, and drops a field that has none.

## Tools

- `python3 tools/journal_dump.py <file.fmj> [--first 20] [--type OrderFill] [--no-crc]` prints the header, instruments, parameter table, events and a count per type.
- `python3 tools/pnl_report.py <file.fmj>` computes fills, fees and PnL ([Journals, replay and PnL](../how-to/operations/journals-replay-pnl.md)).
- `fastmm-replay --journal <file.fmj> --verify` replays it ([Determinism](../explanation/determinism.md)); `--allow-incomplete` replays a file the writer never closed.
- `fastmm-pnl` and `fastmm.open_store()` answer the same questions from the [store](storage.md), which is written alongside the journal and needs no replay.
