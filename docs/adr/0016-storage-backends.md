# ADR-0016: Pluggable storage backends, SQLite first

Status: accepted (2026-09)

A session's trading record — fills, orders, positions, PnL, kill events and session metadata — is written to a pluggable storage backend chosen by `[storage] backend`, and can be queried without replaying a journal. SQLite ships as the first backend.

## Context

- The only durable record of a session was the `.fmj` journal: the byte-exact event stream (ADR-0010). Answering "what did I trade yesterday" meant replaying it or parsing it with `tools/pnl_report.py`, per file, with no index and no cross-session view.
- The other durable state was a one-line session epoch file and a kill-state file (`core/session_state.hpp`). The `/dev/shm` status segment dies with the process.
- The engine already hands the journal its records through an SPSC `MsgRing` written from the trading thread without allocating or waiting. The same mechanism serves a second consumer.
- Storage is a deployment choice. One operator wants a file next to the journal, another an existing ClickHouse or Postgres, a third nothing at all. Baking one into the engine makes the other two fork it.

## Decision

1. **The engine writes a record stream, not rows.** `core/record_stream.hpp` defines four trivially copyable records (fill, order, position, kill) and a `RecordWriter` that copies them into a `MsgRing`. It is allocation-free and wait-free; a full ring drops the record and counts it. The engine knows nothing about storage.
2. **A backend is an interface behind a registry.** `store::Backend` consumes records; `store::Reader` answers queries. `store::StoreRegistry` maps a name to two factories, and `[storage] backend` names one. `none` is reserved and allocates nothing. Registration is explicit (`register_builtin_backends()`), like the strategy registry and for the same reason: a static library's self-registering object is dropped by the linker.
3. **A backend parses its own configuration.** `[storage]` is a free-form section like `[sim]` and `[backtest]`; the central schema knows no key in it. An out-of-tree backend adds keys without touching FastMM.
4. **Losing a record is allowed; blocking the engine is not.** The store is a reporting record, not the risk path: a full ring or a failing backend is counted and logged, and the session keeps trading. The journal stays the authority and can rebuild it. A backend that cannot be *opened* stops the session at start-up, so a silent non-record is impossible.
5. **SQLite, bundled through CPM.** WAL, `synchronous=NORMAL`, one file, one schema version table with a migration step per version. Bundled rather than taken from the host because FastMM vendors its dependencies (ADR-0005), the WAL and UPSERT behaviour the store relies on is version-dependent, and a trading host should not need a `-dev` package.

## Consequences

- A second queue out of the engine: a fill costs one 256-byte and one 192-byte copy into a ring, an order update one 192-byte copy. With `backend = "none"` the writer is disabled and costs a null check.
- A third durable artefact per deployment (journal, kill state, store) with its own retention. The store grows with fills, not with market data.
- The store is not a ledger. It records what FastMM saw; it does not reconcile against the venue, and nothing that happened while the process was down is in it.
- Two records can disagree after a crash: the journal loses at most its last unflushed block, the store its last uncommitted batch. `sessions.clean_shutdown` and `sessions.journal_complete` say which.
