# Architecture

The engine thread holds the books, orders, positions and risk state; venue threads feed it through single-producer, single-consumer rings, and it journals each message it consumes. Live sessions, backtests and replays build the same engine with a different clock, feed and transport. The overview with an animation is [How FastMM works](how-it-works.md).

## Threads

`fastmm-live` runs a fixed set of threads for the whole session:

| Thread | Owns | Waits by |
|---|---|---|
| `fm-net-<i>`, one per venue | the network reactor: sockets, TLS, WebSocket and HTTP, decoding, book sync, order encoding and signing, the rate limiter, the order latency histograms | `epoll_wait` or `io_uring_enter` (`[engine] net_backend`), or busy |
| `fm-engine` | books, strategy, risk, OMS, quote manager, timers, positions, journal sequencing | busy spin, or a futex with `spin_mode = "adaptive"` |
| journal writer | the journal ring into the `.fmj` file; runs when journaling is on | spin, then waits of 50 µs to 1 ms |
| `fm-store` | fill, order, position, kill and funding rows; runs unless `[storage] backend = "none"` | 2 ms sleeps when idle |
| log sink | every thread's log records | spin, then waits of 50 µs to 10 ms |
| main | configuration, thread start and stop, the control socket, signals, the engine's heartbeat | `poll`, at most 50 ms |

The main thread also keeps time-driven work off the engine:

| Every | Main thread work |
|---|---|
| 250 ms | writes the status file and the kill file |
| 1 s | posts the venues' timers and prints the stats line |
| `[engine] tsc_recalibrate_s` | recalibrates the clock ([Clock calibration](#clock-calibration)) |
| SIGINT, SIGTERM, `--duration` | sets the kill switch, then cancels all orders on each venue over a separate REST connection |

With `spin_mode = "adaptive"` nothing spins while idle. A network thread polls while active and for 200 µs after, then waits with a 1 ms timeout. The engine spins, then sleeps on a futex that the network threads signal, at most until the next timer.

Every ring has one producer and one consumer, so N producers mean N rings. The engine visits them round-robin, at most 64 messages per ring per visit, and the journal records events in the order the engine consumed them.

With `fastmm-live --gateway <socket>` there are no `fm-net-<i>` threads. The venues run in `fastmm-gateway`, and each venue's rings live in shared memory with the same protocol ([Run behind a gateway](../how-to/operations/run-behind-a-gateway.md)).

### Run-to-completion

`[engine] threading = "single"` removes `fm-net-0`: the pinned engine thread runs the venue's network loop before every engine step. No event crosses a core between the packet read and the order write. It takes one venue; a configuration with more is refused.

| Event | With `threading = "single"` |
|---|---|
| market data | journaled, applied and handed to the strategy while the venue is still in the receive callback that decoded it |
| outbound orders | encoded and written in one call from the engine thread, so tick-to-trade ends with the order on the socket |
| order events, control messages, timers | taken by the step after each network iteration |

The rings stay, as same-thread queues, and the journal records the same order of events, so replay works unchanged. The cost: while the engine works nobody reads the sockets, and a slow strategy hook delays the venue's heartbeats too. Numbers: [Benchmarks](benchmarks.md).

## Network reactor

Every connection runs on a single-threaded event loop with a timer heap and a mailbox that other threads may post to. One iteration waits for I/O until the next timer, dispatches it, then runs posted tasks and expired timers, without allocating. `[engine] net_backend` picks the backend for `fastmm-live` and `fastmm-sim-exchange`:

| | `epoll` (default) | `io_uring` |
|---|---|---|
| registration | `epoll_ctl`, edge-triggered | one multishot poll per descriptor |
| wait | `epoll_wait`, timeout in whole ms | `io_uring_enter`, timeout in ns |
| busy poll | `epoll_wait` with a zero timeout | reads the completion ring in user space, enters the kernel only to submit |
| needs | any kernel | Linux 5.13 or newer, not blocked by `kernel.io_uring_disabled` or seccomp |

When io_uring is unavailable, `fastmm-live` and the simulator log a warning and use epoll. Docker's default seccomp profile blocks it. The two backends are within measurement noise of each other on a loopback echo ([bench/full-results.md](../../bench/full-results.md)): the `read` and `write` calls dominate, and io_uring saves only the empty wait of a busy-polling loop.

Code that registers its own descriptors, as a new venue does, follows three rules:

| Rule | Because |
|---|---|
| `remove()` before `close()` | an io_uring poll holds the file open, so a closed socket would stay bound until the poll is cancelled |
| tolerate a spurious readiness call | a multishot poll reports every wake-up; the extra read or write returns EAGAIN |
| expect a removed handler not to be called | the handler is looked up again for each event in a batch |

## Memory

The engine reserves all its memory before the first event and touches each slot once, so no page fault lands in the middle of trading:

<video class="fastmm-video" autoplay loop muted playsinline preload="metadata" poster="../../assets/anim/reserved-at-startup.jpg" src="../../assets/anim/reserved-at-startup.mp4"></video>

| Reserved | Capacity |
|---|---|
| open orders | 4,096 |
| instruments: books, positions, risk state | 256 |
| orders sent per event | 32 |
| market-data ring per venue | 4 MiB, `[engine] md_ring_bytes` |
| order-event ring per venue | 1 MiB, `[engine] order_ring_bytes` |
| journal ring | 16 MiB, `[engine] journal_ring_bytes` |

While trading, an order takes a free slot and gives it back when it is done. A usual program asks the allocator, and sometimes the kernel, for each new order:

<video class="fastmm-video" autoplay loop muted playsinline preload="metadata" poster="../../assets/anim/no-allocation.jpg" src="../../assets/anim/no-allocation.mp4"></video>

## Hot-path rules

1. No heap allocation after warm-up; `tests/hotpath` counts every global `operator new` and fails on one.
2. No exceptions: hot functions return `Result<T, E>` and are `noexcept` ([ADR-0002](../adr/0002-exceptions-off-hot-path.md)).
3. No `double` in price and quantity arithmetic: they are `int64` with a 1e-8 scale ([Fixed point](../reference/fixed-point.md)).
4. No virtual calls in the engine loop: strategy, clock, transport and feed are template parameters ([ADR-0009](../adr/0009-crtp-strategies-over-virtual.md)). Run-to-completion adds one indirect call per market-data event and one per outbound batch.
5. Every message between threads is trivially copyable with an explicit size.

## Strategy registration

A strategy is built through a registry that keeps one factory per transport: simulated, replay and live. A strategy library registers its strategies with one function that calls `register_strategy<S>(r)` for each. The command lines register the built-in strategies and then the modules the program passes; a factory no file compiles is a link error ([Register a strategy](../how-to/strategies/register-a-strategy.md)).

## Clock calibration

The live clock reads the CPU's time-stamp counter and converts it to wall-clock ns with a measured rate and an anchor. The first rate comes from a 50 ms measurement against `CLOCK_MONOTONIC_RAW`; the anchor is `CLOCK_REALTIME`.

| Step | What happens |
|---|---|
| recalibration | every `[engine] tsc_recalibrate_s` (default 10 s, 0 off), the main thread measures the rate over the whole time since the last anchor, so clock jitter costs a few ppm |
| log | `tsc recalibrated: drift <ns> over <s> (<ppm>) ...` |
| hand-over | the engine picks up the new calibration once per loop iteration, with one atomic load when nothing changed |
| small offset | the clock stays continuous and slews the offset away over one period, at most 500 ppm |
| offset over 1 ms | the clock steps to the measurement, counts `clock_steps` and logs a warning |

The calibration follows `CLOCK_REALTIME`, so when the host steps its wall clock the engine clock steps with it. NTP corrections and WSL2 resyncing with Windows do that, sometimes by hundreds of milliseconds.

| CPU flags in `/proc/cpuinfo` | Wall time | Latency intervals |
|---|---|---|
| `constant_tsc` and `nonstop_tsc` | the calibrated counter | counter cycles at the measured rate |
| `constant_tsc` only, as on many KVM cloud instances | `clock_gettime`, no recalibration | counter cycles at the measured rate |
| neither | `clock_gettime`, no recalibration | `clock_gettime` ns |

## Latency instrumentation

Each stamp is a counter reading; intervals go into allocation-free log-linear histograms.

| Stamp | Thread | Taken when |
|---|---|---|
| T0 | network | the frame was received |
| T1 | network | it was decoded into an engine message |
| T2 | engine | the book, position or OMS was updated |
| T3 | engine | the strategy hook returned |
| T4, T5 | engine | around the hand-over of the outbound batch: a ring push, or with `threading = "single"` the encoding and write |

The engine publishes decode, book apply, strategy, serialize (T3 to T4), send (T4 to T5), tick-to-trade (T0 to T5) and wire-to-book (T0 to T2) every `latency_publish_ms`. Each outbound order carries its trigger's T0, and the network thread measures the rest:

| Interval | Measures |
|---|---|
| encode | JSON encoding and signing |
| send | the WebSocket or REST send call |
| wire tick-to-trade | T0 to after the send; orders from timers are not counted |

`fastmm-live` prints the wire tick-to-trade p50 and p99 and the encode and send p50 per venue in its stats line and final summary.

## Failure handling

| Failure | Detection | Action |
|---|---|---|
| Market-data disconnect | EPOLLRDHUP, a read of 0 bytes, a TLS error | quotes pulled, reconnect with backoff, fresh snapshot |
| Sequence gap | the book sync state machine | quotes pulled, re-snapshot (rate limited) |
| Stale feed | no traffic for the connector's `stale_ms` | quotes pulled; `dead_ms` without traffic forces a reconnect ([Venue connectors](../reference/venues.md#configuration-keys)) |
| Order channel loss | the connection state machine | cancel-all over REST (`cancel_on_order_channel_loss`); after reconnect, replay executions, then reconcile open orders |
| Kill switch | `max_loss`, a full ring, every venue killed, a strategy error, client order ids used up, a signal, `--duration`, a journal write failure | stop quoting and cancel working orders; at shutdown each venue cancels all over REST, 5 s timeout ([Risk model](risk-model.md#the-kill-switch), [Kill switch and shutdown](../how-to/operations/kill-switch-and-shutdown.md)) |
| Venue unusable | a fatal error such as a bad key or signature, Binance HTTP 418, a dead man's switch not refreshed, a multicast feed that cannot rebuild | that venue's kill switch; every venue killed is a global kill |
| Rate limit | response headers and error codes | cool down until the limit resets; after HTTP 418 REST stays stopped until restart |
| Clock skew | timestamp rejection codes | re-measure the offset from the venue's time endpoint |
| Engine thread stuck | its heartbeat has not moved for `[engine] stall_ms` | cancel-all on every venue from the main thread, exit code 5 ([Engine stall](../how-to/operations/kill-switch-and-shutdown.md#engine-stall)) |
| Ring overflow | a push fails | market data: drop the delta and resync; order events: kill switch |
