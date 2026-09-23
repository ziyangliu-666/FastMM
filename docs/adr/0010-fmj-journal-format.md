# ADR-0010: The .fmj journal format

Status: accepted (2026-09); amended by format version 2 and version 3 (2026-09)

## Context

Deterministic replay needs every inbound event (market data, order events, timers, connection states) in consumption order.

## Decision

Append-only mmap file: header (magic FMJ1, session, TSC calibration, instrument table, config hash) + 1 MiB blocks with CRC32C. The engine assigns a global sequence number as it consumes.

## Consequences

A truncated tail block is detected by CRC and dropped. Journals are self-contained inputs for `fastmm-backtest` and `fastmm-replay`.

## Version 2: live sessions replay exactly

### Context

Version 1 journals of `fastmm-live` sessions did not replay to a match; they diverged at the first outbound message. The events were complete, but three inputs of the engine were missing:

- **The engine clock.** Live, the engine reads a `TscClock` that drifts from `CLOCK_REALTIME` and is re-anchored or stepped by recalibration. Replay drove its `SimClock` from each event's `recv_ts` (the venue thread's wall clock) or a timer's `fire_ts` (the engine clock), a mixed time base that even runs backwards. Out* stamps and every time-based decision (minimum requote interval, rate limiter, stale market data, crossed-book grace, strategy timers) came out differently.
- **The session settings.** Client order ids carry the session epoch, which replay always set to 1, so every recorded ack named an unknown order and replay sent cancels. Dry run (quoting disabled), the RNG seed and each venue's effective cancel-replace (venue capability and configuration) were not recorded either.
- **The configuration.** `fastmm-replay` fell back to `configs/backtest-example.toml` and never checked the header's config hash.

### Decision

`kJournalVersion` is 2. Readers open versions 1 and 2.

- **Engine clock per event.** The engine reads its clock once per consumed event, per fired timer, at start and at finish, and uses that value for every decision and Out* stamp inside. The journal stores it without growing market-data records: a consumed event (or synthetic `TimerMsg`) is flagged `kEngineTime` and `EventHeader::reserved0` holds the signed int32 ns delta from the previous `kEngineTime` record. An `EngineTimeMsg` (128 bytes, `EventType::EngineTime`) holds the absolute value at engine start (`Start`), at finish (`Finish`), and before an event whose delta does not fit in int32 ns or whose predecessor was lost to a full ring (`Sync`). Outbound copies, latency samples and market-data journals written with `JournalWriter::record()` carry no engine time, and `record()` clears the flag on copied messages.
- **Session header.** The former reserved bytes hold `session_epoch` (u16), `quoting_enabled` (u8), `header_flags` (u8, `kHeaderSession` when the three session fields are valid), `config_bytes` (u32), `replace_venues` (u64, bit v set when venue v traded with cancel-replace) and `config_crc32c` (u32). `rng_seed` is the engine seed of the session.
- **Embedded configuration.** `Config::effective_toml()` (deterministic TOML of every configuration value, defaults included, after command-line overrides, without `api_key` and `api_secret`) follows the instrument table, zero-padded to a multiple of 64 bytes and covered by `config_crc32c`; `header_bytes` includes it. `config_hash` is the FNV-1a hash of that text, so it identifies the effective configuration rather than the file's formatting, secrets or environment.
- **Transport refusals.** The engine journals an outbound batch after handing it to the transport. Transports accept a prefix of a batch; copies of the refused remainder carry `kDropped`, and the replay transport refuses the same messages, so the replayed engine trips the same kill switch.

Replay (`bt::replay_journal`, `fastmm-replay`) sets `SimClock` to the recorded engine clock, backwards steps included, restores the session settings from the header, and uses the embedded configuration unless one is given explicitly (a different effective hash is reported as a what-if run). A version 1 journal replays as before: clock from receive and fire times, session settings and configuration from the given configuration.

### Consequences

- `fastmm-live` journals replay to the identical outbound stream (`tests/integration/live_replay_test.cpp` records sessions against the simulator, one of them with TSC recalibration steps, and replays them from the journal alone).
- A journal is replayable without the configuration file or its `${VAR}` secrets.
- Every consumed event costs a subtraction and a compare in the journal writer; the extra records are two per session plus one per idle gap longer than 2.1 s.
- Seq numbers now also count `EngineTime` records.
- Tools reading the header must use the version 2 layout (`tools/journal_dump.py` does).

## Amendment: format version 3 (2026-09)

### Context

ADR-0013 makes a strategy's parameters a journaled input: a `ParamUpdate` record (`EventType::ParamUpdate`) names a schema index, and replay has to resolve that index to the same field. A version 2 journal carries no parameter names, so a replay against a build whose schema order changed would apply the wrong field silently.

### Decision

`kJournalVersion` is 3. Readers open versions 1, 2 and 3.

- **Parameter table.** After the padded configuration the header carries `param_count`, `param_table_bytes` and `param_table_crc32c`, followed by one entry per schema index with the parameter's name and `ParamType` (`include/fastmm/core/journal.hpp`, up to `kJournalMaxParams` = 32 entries). Replay matches recorded `ParamUpdate` records to the running strategy's schema by name, not by index.
- **Strategy metadata.** `strategy_meta`, `key=value` lines describing the strategy, follows the same header region; it is empty when the strategy supplies none.

### Consequences

- A version 2 journal replays unchanged; its parameter table is empty, so a `ParamUpdate` in such a file resolves by index as before.
- `tools/journal_dump.py` and `fastmm-replay` read all three versions ([Journal format](../reference/journal-format.md)).
