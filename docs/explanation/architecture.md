# Architecture

FastMM is built around one idea: **a single engine thread owns all trading state, every input
crosses into it as a trivially-copyable message in an SPSC ring, every input is journaled, and
the same `Engine<Strategy, Clock, Transport, Feed>` template runs live, in simulation, and in
replay.**

```
             ┌──────────────────────────── net thread (per venue) ────────────────────────────┐
 Venue WS ──►│ reactor ─ TLS (OpenSSL memory BIO) ─ WebSocket frames ─ simdjson ─ book sync    │──► md / order MsgRing ──┐
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

The live application (`fastmm-live`, `src/live/session.cpp`) runs a fixed set of
threads for the whole session:

| Thread | Owns | Waits by |
|---|---|---|
| `fm-net-<i>` (one per venue) | `net::Reactor`: sockets, TLS, WebSocket/HTTP, JSON decode, book sync, order encoding and signing, rate limiter, the venue's order latency histograms | `epoll_wait` or `io_uring_enter` (`[engine] net_backend`), busy (`spin_mode = busy`) or with a 1 ms timeout (`adaptive`) |
| `fm-engine` | books, strategy, risk, OMS, quote manager, timers, positions, journal sequencing, its `TscClock` copy | busy-spin, adaptive back-off with `spin_mode = adaptive` |
| `fm-journal` (when journaling) | `JournalFileWriter`: drains the journal ring into the `.fmj` file | spin, then short sleeps |
| log sink (`Logger::start`) | formats log records from every thread's ring and writes them | spin, then short sleeps |
| main thread | control: parses the config, loads reference data, starts and stops the other threads, posts `Venue::on_timer` and prints the stats line every second, recalibrates the TSC every `[engine] tsc_recalibrate_s`, handles SIGINT/SIGTERM and `--duration` (kill switch, `cancel_all` on every venue over an independent REST connection) | 50 ms sleeps |

All queues are single-producer/single-consumer (`MsgRing`, byte-oriented, variable-length
64-byte-aligned messages). N producers means N rings; the engine polls them round-robin with a
batch cap so one venue cannot starve another. The order in which the engine consumes events *is*
the canonical order and is what the journal records, so replay is exact.

## Network reactor

`net::Reactor` (`include/fastmm/net/reactor.hpp`) is the single-threaded event loop under every
connection: descriptors registered with an `IoHandler`, a timer min-heap, and a mailbox
(`post()` / `wake()` through an eventfd), the only part that other threads may call. One
`run_once()` waits for I/O (at most until the next timer or `max_wait_ms`), dispatches it, then
runs posted tasks and expired timers, without allocating. The connection, TLS, WebSocket and HTTP
code only uses registration (`add`, `modify`, `remove`) and timers, so it runs unchanged on either
backend. `[engine] net_backend` selects the backend for `fastmm-live` and `fastmm-sim-exchange`.

| | `epoll` (default) | `io_uring` |
|---|---|---|
| registration | `epoll_ctl`, edge-triggered, `EPOLLRDHUP` | one multishot `IORING_OP_POLL_ADD` per fd with `POLLRDHUP`; `POLLOUT` only while the registration asks for `Write` |
| modify | `EPOLL_CTL_MOD` | `IORING_POLL_UPDATE_EVENTS` on the live poll request |
| remove | `EPOLL_CTL_DEL` | `IORING_OP_POLL_REMOVE`, submitted immediately |
| wait | `epoll_wait`, timeout rounded up to whole ms | `io_uring_enter` with `IORING_ENTER_EXT_ARG`, timeout in ns |
| busy poll | `epoll_wait` with a zero timeout every iteration | reads the completion ring in user space; enters the kernel only to submit, or when the kernel sets `IORING_SQ_TASKRUN` or `IORING_SQ_CQ_OVERFLOW` |

Both backends map events the same way: `POLLERR` calls `on_error(SO_ERROR)`, `POLLIN`,
`POLLRDHUP` or `POLLHUP` call `on_readable()`, `POLLOUT` calls `on_writable()`. The handler is looked
up again for every event, so a handler that an earlier callback in the same batch removed is not
called, and one it replaced receives the event instead.

The io_uring backend talks to the kernel through the raw `io_uring_setup` / `io_uring_enter`
system calls and `<linux/io_uring.h>`; liburing is not a dependency.

* **Ring.** The SQ and CQ rings and the SQE array are mmap'd (one mapping when the kernel has
  `IORING_FEAT_SINGLE_MMAP`): 512 SQEs, 4096 CQEs, with `IORING_SETUP_SUBMIT_ALL`, `COOP_TASKRUN`
  and `TASKRUN_FLAG` on 5.19 and newer. `EXT_ARG` and `NODROP` are required. Registrations are
  queued and go out with the next wait, except `remove()`, which submits at once.
* **Support probe.** `Reactor::io_uring_supported()` creates a small ring once and checks that a
  multishot poll on an eventfd can be updated and then reports `IORING_CQE_F_MORE`. It is false on
  ENOSYS, EPERM (`kernel.io_uring_disabled`, seccomp), ENOMEM and kernels older than 5.13;
  `fastmm-live` and the simulator then log a warning and use epoll.
* **Stale completions.** `user_data` packs the operation (4 bits), a registration generation
  (28 bits) and the fd (32 bits). `add()` bumps the generation, so completions still queued for a
  removed registration, or for an earlier file that had the same fd number, are dropped.
* **Re-arming.** A multishot poll that ends without `IORING_CQE_F_MORE` (CQ overflow, a racing
  update) is re-armed while its registration exists. An update or remove that races with a
  completing poll (`-EALREADY`) is retried. A poll the kernel refuses (for example `-EBADF`) is
  reported once through `on_error(errno)`.
* **Descriptor lifetime.** A poll request holds a reference to its file, so a socket closed while
  still registered stays open (no FIN, port still bound) until the request is cancelled. Always
  `remove()` before `close()`, as the net classes do. If a socket number is reused while the old
  registration is still there, `add()` sees a different inode and cancels the stale request.
  eventfd and timerfd descriptors share one anonymous inode, so this check cannot tell two of them
  apart.
* **Wake-ups.** A multishot poll reports every wake-up of the socket's wait queue, so a handler
  can be called for readiness it has already consumed. Handlers read or write until EAGAIN, so
  this costs one failed system call.

**Which one to use.** epoll stays the default. `bench/bench_reactor.cpp` measures 64-byte
loopback TCP echo round trips with both backends. On the development machine (WSL2, Linux 6.6, a
shared 8-core host) the two are within measurement noise: p50 about 12.8 µs for both when client
and server share one reactor; 10.8 to 11.3 µs for both, depending on the run, with the server on
its own busy-polling thread; about 74 µs for both when both sides block and each round trip needs
two cross-thread wake-ups (one epoll run out of five came in at 18 µs and did not reproduce). The
time goes to the loopback TCP stack and the `read`/`write` system calls, which both backends make
the same way, not to readiness notification. io_uring only saves the empty `epoll_wait` of an idle busy-polling loop.
It could win clearly once reads and writes themselves go through the ring (multishot receive,
registered buffers), which this reactor does not do. Measure on the production kernel before
switching.

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
stream is byte-identical. [Determinism](determinism.md) explains how live sessions replay exactly
and what breaks a replay; [Event flow](event-flow.md) follows one event through the engine.

Strategies are built through the `StrategyRegistry`, which keeps one factory per transport kind
(Sim, Replay, Live). A strategy library registers its strategies with one function that calls
`register_strategy<S>(r)` per strategy and so adds all three factories; the built-in strategies are
the module `fastmm::register_builtin_strategies` in `fastmm::strategies`. The command lines
(`fastmm::cli::live`, `backtest`, `replay`) register the built-in strategies and then the modules
the app passes, explicitly at startup; `run_backtest(cfg, name)`, `replay_journal`, sweeps and the
Python module register the built-in strategies through `bt::register_builtin_strategies()`. The
factory templates are declared in `strategies/module.hpp` and defined in
`strategies/factory_{sim,replay,live}.hpp`, so a registered factory that no file compiles is a link
error. See [Register a strategy](../how-to/strategies/register-a-strategy.md).

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

See the [failure handling table](../reference/configuration.md#failure-handling): market-data gaps trigger a resync (quotes
pulled), order-channel loss triggers `cancel_all` through an independent REST connection and a
reconciliation pass after reconnect, and the kill switch (global or per venue) mass-cancels and
stops quoting while still allowing cancels.
