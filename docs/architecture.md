# Architecture

FastMM is built around one idea: **a single engine thread owns all trading state, every input
crosses into it as a trivially-copyable message in an SPSC ring, every input is journaled, and
the same `Engine<Strategy, Clock, Transport, Feed>` template runs live, in simulation, and in
replay.**

```
             ┌──────────────────────────── net thread (per venue) ────────────────────────────┐
 Venue WS ──►│ epoll ET ─ TLS (OpenSSL memory BIO) ─ WebSocket frames ─ simdjson ─ BookSyncer │──► MsgRing ──┐
 Venue REST◄─│ HTTP/1.1 keep-alive ◄──────────── OrderGateway encoder ◄───────────────────── MsgRing ◄──┐   │
             └────────────────────────────────────────────────────────────────────────────────┘        │   │
                                                                                                       │   ▼
   ┌──────────────────────────────── engine thread (pinned, busy-spin) ─────────────────────────────┐  │
   │  dispatch(switch on EventType) → L2/L3 book apply → Strategy hooks → QuoteManager diff         │  │
   │  → RiskEngine.check_new (O(1)) → OMS state machine → Transport.send(batch) ─────────────────────┼──┘
   │  TimerWheel · PositionTracker · LatencyTracker(T0..T5) · JournalWriter(seq) · Seqlocked snapshots│
   └───────────────────────────────────────────┬────────────────────────────────────────────────────┘
                                               │ JournalRing / LogRing
                                               ▼
                       journal thread: .fmj mmap append (1 MiB blocks, CRC32C) · async fmt logger
                       control thread: REST snapshots, reconciliation, listenKey keepalive, stats export

   Backtest / replay: SimClock + SimTransport(MatchingEngine + LatencyModel) + InlineFeed/JournalFeed
```

## Threads

| Thread | Owns | Waits by |
|---|---|---|
| `net-<venue>` | sockets, TLS, WS/HTTP parsers, JSON decode, book sync FSM, order encoder, rate limiter | `epoll_wait(0)` spin, adaptive back-off to 1 ms on WSL2 |
| `engine` | books, strategy, risk, OMS, quote manager, timers, positions, journal sequencing | busy-spin with `_mm_pause` |
| `journal` | `.fmj` file, log formatting | spin then `nanosleep` |
| `control` | REST (snapshots, open orders), listenKey, TSC recalibration, stats | blocking |

All queues are single-producer/single-consumer (`MsgRing`, byte-oriented, variable-length
64-byte-aligned messages). N producers means N rings; the engine polls them round-robin with a
batch cap so one venue cannot starve another. The order in which the engine consumes events *is*
the canonical order and is what the journal records, so replay is exact.

## Hot-path rules

1. No heap allocation after `warm_up()`; enforced by `tests/hotpath` (global `operator new` counter).
2. No exceptions; hot functions return `Result<T, E>` and are `noexcept`.
3. No `double` in price/quantity arithmetic: `Price`/`Qty`/`Notional` are `int64` with a global 1e-8
   scale; products go through `__int128`.
4. No virtual calls inside `Engine::run()`: strategies, clocks, transports and feeds are template
   parameters (CRTP/concepts); virtual dispatch is only used on the control path.
5. Every cross-thread message is trivially copyable with an explicit size.

## Determinism

`Clock` and `Transport` are compile-time policies. Live uses `TscClock` (rdtsc calibrated against
`CLOCK_REALTIME`) and `LiveTransport` (writes into the venue's outbound ring). Backtests use
`SimClock` (virtual time driven by event timestamps) and `SimTransport` (in-process matching engine
plus a seeded latency model). Timers and RNG are virtualised the same way. The `fastmm-replay`
tool feeds a recorded journal through the engine and verifies that the outbound stream is
byte-identical.

## Latency instrumentation

`rdtscp` stamps at T0 (recv returned), T1 (decoded), T2 (book applied), T3 (strategy decided),
T4 (order serialised), T5 (send returned). Per-hop deltas go into allocation-free log-linear
histograms (`LogLinearHistogram`, 40 x 16 buckets) published once per second via a seqlock.

## Failure handling

See the table in `docs/configuration.md#failure-handling`: market-data gaps trigger a resync (quotes
pulled), order-channel loss triggers `cancel_all` through an independent REST connection and a
reconciliation pass after reconnect, and the kill switch (global or per venue) mass-cancels and
stops quoting while still allowing cancels.
