# Architecture

FastMM is built around one idea: **a single engine thread owns all trading state, every input
crosses into it as a trivially-copyable message in an SPSC ring, every input is journaled, and
the same `Engine<Strategy, Clock, Transport, Feed>` template runs live, in simulation, and in
replay.**

```
             ┌──────────────────────────── net thread (per venue) ────────────────────────────┐
 Venue WS ──►│ epoll ─ TLS (OpenSSL memory BIO) ─ WebSocket frames ─ simdjson ─ book sync      │──► md / order MsgRing ──┐
 Venue WS/REST◄ WebSocket write / HTTP/1.1 keep-alive ◄── order encoder + signer ◄──────────────│◄── outbound MsgRing ◄─┐ │
             └────────────────────────────────────────────────────────────────────────────────┘                        │ │
                                                                                                                        │ ▼
   ┌──────────────────────────────── engine thread (pinned, busy-spin) ─────────────────────────────┐                  │
   │  dispatch(switch on EventType) → L2/L3 book apply → Strategy hooks → QuoteManager diff         │                  │
   │  → RiskEngine.check_new (O(1)) → OMS state machine → Transport.send(batch) ─────────────────────┼──────────────────┘
   │  TimerWheel · PositionTracker · LatencyTracker(T0..T5) · JournalWriter(seq) · Seqlocked snapshots│
   └───────────────────────────────────────────┬────────────────────────────────────────────────────┘
                                               │ journal ring            per-thread log rings
                                               ▼                                 ▼
                       journal thread: .fmj append (CRC32C)      log sink thread: fmt formatting, FILE*
                       main thread: control, stats, TSC recalibration, kill switch

   Backtest / replay: SimClock + SimTransport(MatchingEngine + LatencyModel) + InlineFeed/JournalFeed
```

## Threads

The live application (`fastmm-live`, `apps/fastmm-live/live_backend.cpp`) runs a fixed set of
threads for the whole session:

| Thread | Owns | Waits by |
|---|---|---|
| `fm-net-<i>` (one per venue) | `net::Reactor`: sockets, TLS, WebSocket/HTTP, JSON decode, book sync, order encoding and signing, rate limiter, the venue's order latency histograms | `epoll_wait`, busy (`spin_mode = busy`) or with a 1 ms timeout (`adaptive`) |
| `fm-engine` | books, strategy, risk, OMS, quote manager, timers, positions, journal sequencing, its `TscClock` copy | busy-spin, adaptive back-off with `spin_mode = adaptive` |
| `fm-journal` (when journaling) | `JournalFileWriter`: drains the journal ring into the `.fmj` file | spin, then short sleeps |
| log sink (`Logger::start`) | formats log records from every thread's ring and writes them | spin, then short sleeps |
| main thread | control: parses the config, loads reference data, starts and stops the other threads, posts `Venue::on_timer` and prints the stats line every second, recalibrates the TSC every `[engine] tsc_recalibrate_s`, handles SIGINT/SIGTERM and `--duration` (kill switch, `cancel_all` on every venue over an independent REST connection) | 50 ms sleeps |

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
`CLOCK_REALTIME`, see below) and `LiveTransport` (writes into the venue's outbound ring). Backtests
use `SimClock` (virtual time driven by event timestamps) and `SimTransport` (in-process matching
engine plus a seeded latency model). Timers and RNG are virtualised the same way. The
`fastmm-replay` tool feeds a recorded journal through the engine and verifies that the outbound
stream is byte-identical.

Strategies are built through the `StrategyRegistry`, which keeps one factory per transport kind:
the backtest library registers the Sim and Replay factories
(`bt::register_builtin_strategies()`), and `fastmm-live` registers the Live ones
(`register_live_strategies()`), both explicitly at startup.

## Clock calibration

`TscClock` maps a TSC reading to wall-clock ns with a 32.32 fixed-point rate and an anchor
(`TscCalibration`). `calibrate_tsc()` measures both against `CLOCK_REALTIME` over a 50 ms spin.
The rate is only as good as that measurement, and the wall clock is itself slewed by NTP, so the
mapping drifts over a long session; stale-market-data checks and timers read it.

* Only startup uses a short measurement window. Afterwards `TscCalibrator` takes a fresh anchor
  (a tight rdtsc bracket around `CLOCK_REALTIME` and `CLOCK_MONOTONIC_RAW`) and computes the rate
  over the whole interval since the previous anchor, against `CLOCK_MONOTONIC_RAW`. Tens of
  microseconds of `clock_gettime` jitter then cost a few ppm instead of thousands, and NTP slews or
  host clock steps cannot distort the rate. Re-measuring the rate over 50 ms every time used to
  drift the mapping by milliseconds between recalibrations on WSL2 and force steps.
* The main thread recalibrates every `[engine] tsc_recalibrate_s` seconds (default 10, 0 turns
  it off), logs how far the previous calibration had drifted
  (`tsc recalibrated: drift <ns> over <s> (<ppm>) ...`) and publishes the result in a
  `Seqlocked<TscCalibration>`.
* Nobody else's clock is written from the main thread. Every user owns a copy: the engine's
  `TscClock` is attached to the seqlock (`attach_calibration_source`) and the engine calls
  `TscClock::refresh()` once per loop iteration next to the timer poll (not per event).
  `refresh()` is one atomic load when nothing was published; on a new version it copies the
  calibration. `ControlCommand::RecalibrateTsc` makes the engine refresh immediately.
* Re-anchoring keeps time continuous: at the refresh point the clock computes the old mapping's time
  for the current TSC reading and anchors there, so successive `now()` calls never go backwards.
  From there it runs at the newly measured rate plus a bounded correction that absorbs the measured
  offset over one recalibration period (offset × rate ÷ period, at most 500 ppm), so the mapping
  converges on the measurements instead of accumulating error. If the old mapping disagrees with the
  fresh measurement by more than 1 ms, the clock steps to the measured anchor instead, counts the
  step (`EngineStats::clock_steps`) and the engine logs a warning.
* The calibration follows `CLOCK_REALTIME`. If the host steps its wall clock (NTP corrections, or
  WSL2 resynchronising with Windows, which can jump by hundreds of milliseconds), the next
  recalibration steps the engine clock with it and counts it in `EngineStats::clock_steps`.
* The venues read the latest calibration when they publish their status, to convert their
  cycle-based latency histograms to ns.

## Latency instrumentation

Stamps are `rdtscp` readings (`Cycles`); intervals go into allocation-free log-linear histograms
(`LogLinearHistogram`, 40 x 16 buckets).

| Stamp | Thread | Where |
|---|---|---|
| T0 | network | the frame was received (carried as `EventHeader::t0_cycles`) |
| T1 | network | decoded into an engine message (`t1_delta`) |
| T2 | engine | book (or position/OMS) updated |
| T3 | engine | strategy hook returned |
| T4, T5 | engine | around `transport.send()` of the outbound batch; live, that is the push into the venue's outbound ring |

The engine's `LatencyTracker` (decode, book apply, strategy, serialize = T3 to T4, send = T4 to
T5, tick-to-trade = T0 to T5, wire-to-book = T0 to T2) is published through a seqlock every
`latency_publish_ms`. Live, the engine's tick-to-trade therefore ends when the order is handed to
the network thread. The engine copies the triggering event's `t0_cycles` into every outbound
message header, and the network thread measures the rest in `venues::WireLatencyRecorder`: for
each outbound order message it stamps before encoding, after encoding and signing, and after the
WebSocket write (or REST request call) returned, and records

* encode: JSON encoding and signing,
* send: the WebSocket/REST send call,
* wire tick-to-trade: after-send minus `t0_cycles`, for orders triggered by an inbound event
  (`t0_cycles != 0`; orders from timers are not counted).

These histograms stay in cycles on the network thread and are converted to ns with the published
calibration when the venue publishes its `VenueStatus` (once per stats interval). `fastmm-live`
prints the wire tick-to-trade p50/p99 and the encode/send p50 per venue in its stats line and in
the final summary.

## Failure handling

See the table in `docs/configuration.md#failure-handling`: market-data gaps trigger a resync (quotes
pulled), order-channel loss triggers `cancel_all` through an independent REST connection and a
reconciliation pass after reconnect, and the kill switch (global or per venue) mass-cancels and
stops quoting while still allowing cancels.
