# Determinism

A backtest with the same inputs sends the same orders, and replaying a journal sends the same order messages as the session that wrote it. `fastmm-replay --verify` checks the second property by comparing the SHA-256 of both outbound streams ([How it is checked](#how-it-is-checked)).

## Why it matters

- Replaying the journal of a live session that misbehaved runs the same decisions again under a debugger, without a venue.
- Golden hashes (`tests/backtest/golden_strategies_test.cpp`, `tests/fixtures/journals/sample_1000.sha256`) fail when a change alters what a strategy sends.
- Two backtests with different parameter sets differ only by the parameters.

## How the engine keeps it

`Engine<Strategy, Clock, Transport, Feed>` takes the sources of non-determinism as compile-time policies:

| Input | Live | Backtest | Replay |
|---|---|---|---|
| clock | `TscClock` (CPU counter calibrated against wall time) | `SimClock`, driven by event times | `SimClock`, set to the recorded engine clock |
| events | `RingFeed` from the network threads | `InlineFeed` from the data source or generator | `JournalFeed` in recorded order |
| orders | `LiveTransport` to the venue | `SimTransport`: matching engine with a seeded latency model | `ReplayTransport`: recorded acks and refusals |
| randomness | `ctx.rng()` seeded from `[engine] rng_seed` | the same | the seed from the journal header |

The rest is deterministic because it uses one engine thread, no reads of the system clock, integer arithmetic for money, fixed-capacity containers iterated in a defined order, and timers that fire in engine time.

A live session is not repeatable (network timing decides which event comes first), but its journal is. Journal format version 2 ([Journal format](../reference/journal-format.md)) records, for every consumed event and fired timer, the engine clock at which it was processed, and in the header the session epoch, whether quoting was enabled, each venue's cancel-replace setting and the effective configuration. Replay restores all of them. Parameter updates are journaled inputs like market data; version 3 adds the strategy's parameter table, so replay matches their fields by name.

## How it is checked

`fastmm-replay --verify` (or `tutorial-replay`, or any app built with `cli::replay`) compares every outbound order message the replayed engine sends with the copy in the journal and prints the SHA-256 of both streams:

```text
recorded outbound 712 msgs sha256 1116a9bd...
replayed outbound 712 msgs sha256 1116a9bd...
replay MATCH
```

On a mismatch it prints the first differing message as recorded and as replayed and exits with code 1. Only the first difference means anything: replay feeds the recorded acknowledgements whatever it sent, so later messages diverge too.

## What breaks it

| Cause | Why | Instead |
|---|---|---|
| Reading the system clock (`std::chrono::system_clock`, `clock_gettime`) in a hook | replay runs at a different wall time | `ctx.now()` |
| `std::random_device`, an unseeded or time-seeded generator | a different sequence on every run | `ctx.rng()` |
| State kept outside the strategy object (static or global variables, a counter shared by two strategies) | survives from one run to the next in the same process, as in sweeps or tests | members of the strategy |
| Reading files, environment variables or the network from a hook | not an engine input, not journaled | parameters, read once at configuration |
| Logic that depends on logs, or on `EngineStats` latency figures | logs are not journaled; wall-clock latencies differ | engine state visible through `ctx` |
| Threads or asynchronous work started by the strategy | results arrive at an unrecorded time | do the work in a hook, or publish results as engine events |
| Uninitialised members, iteration over pointer-keyed hash maps | values or order change between runs | initialise every member; order by ids |
| Floating-point decisions that differ between builds (`-ffast-math`, a different compiler, `-march=native`) | the same inputs round differently | fixed-point arithmetic for prices and sizes; replay with the binary that recorded |
| A different binary: changed strategy or engine code, other parameters | the decisions differ | a replay with `--config` or `--strategy` is reported as a what-if run |
| A journal written before format version 2, from a live session | it has no engine clock or session settings | replay with `--config` as a what-if run |
| A full journal ring during recording | events were lost; the engine trips the kill switch | size `[engine] journal_ring_bytes` for the session |

A mismatch with the same binary and the embedded configuration is a bug: some code read an input the journal does not record.

## Related

- [Journals, replay and PnL](../how-to/operations/journals-replay-pnl.md#replay): running a replay
- [ADR-0010](../adr/0010-fmj-journal-format.md): the journal format decision
- [Architecture](architecture.md#clock-calibration): how the live clock is calibrated
