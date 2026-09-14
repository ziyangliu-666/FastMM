# ADR-0013: Python strategies in live trading

Status: proposed (2026-09)

This record replaces the "backtests only" scope of ADR-0012 section 7. Python strategies run live, and the same class runs in backtests and replay. The decision rests on a survey of existing systems and two prototypes.

## Context

- ADR-0012 section 7 calls `fastmm.Strategy` hooks through a CPython adapter on the engine thread with the GIL held. A BasicMM port costs about 3.1 µs per hook call more than C++. That is enough for research and too slow for quoting.
- Systems that call Python on every event (Kungfu, wtpy, vn.py, LEAN) publish no Python latency figures, except wtpy: 70 µs for a Python strategy computation against 4.5 µs in C++. Systems that are fast from Python keep the interpreter off the event path: hftbacktest compiles strategies with Numba, and NautilusTrader recommends Rust for latency-sensitive code.
- Prototype 1 compiled hooks with Numba `cfunc` over engine-owned C structs and called them by function pointer from the engine thread.
  - A BasicMM port costs 56 ns per `on_book` call with exact fixed point and 34 ns with floats, against 28 ns for C++ BasicMM.
  - Tick-to-order p50 was 1215 ns against 1151 ns for C++ BasicMM on the same rig.
  - Backtests ran at 2.11 M events/s against 2.13 M, with the same outbound hash as C++.
  - The engine thread never took the GIL and made no allocations in 8 M calls.
  - Compilation took 0.13–0.91 s. numba and llvmlite add about 206 MB.
- Prototype 2 started the engine and venue threads from an extension module, released the GIL, and published parameters from a Python thread.
  - Engine sojourn p50 was 461 ns and p99.9 36 µs with Python idle, 37–53 µs under pandas and gc churn. A C++ call holding the GIL for 3 s did not change the engine's rate.
  - With every thread pinned off the engine core by `taskset`, matmul and model inference raised sojourn p99.9 to 0.78 ms (service time p99.9 2.1 µs). The same load in a separate process gave 0.32 ms. Unpinned, the engine fell behind.
  - A publish took effect 2.3 µs later on average (max 120 µs).
  - Five sessions (normal, SIGINT, exception, stall, thread death) replayed to the same outbound hash from their journals, and all five ended with orders cancelled.
  - Plain Python callbacks on the engine thread had p50 about 1 µs and p99.9 about 50 µs with no other Python thread. With one other Python thread, p90 was 4.2 ms and max 10 ms.

## Decision

### 1. Hot hooks and slow methods in one class

```python
import fastmm

class MyMM(fastmm.Strategy):
    half_spread_bps = fastmm.Param(5.0)
    fair_offset_bps = fastmm.Param(0.0)
    last_mid = fastmm.State(0.0)

    @fastmm.hot
    def on_book(self, ctx, book):
        mid = book.mid * (1.0 + self.fair_offset_bps * 1e-4)
        half = mid * self.half_spread_bps * 1e-4
        ctx.quote(mid - half, mid + half, 0.001)

    def on_start(self, ctx):
        self.model = load_model("fair.onnx")

    @fastmm.every("1s")
    def refit(self, ctx):
        snap = ctx.snapshot()
        ctx.publish(fair_offset_bps=self.model.predict(snap.features))

fastmm.run_backtest(cfg, strategy=MyMM, data="day.fmj")
fastmm.run_live(MyMM, "configs/binance-demo.toml")
```

Hot hooks:

- Methods decorated `@fastmm.hot` run on the engine thread, compiled by Numba in nopython mode. The first set is `on_book`, `on_fill`, `on_quoting` and `on_connection`; `@fastmm.hot(every="100ms")` declares a timer hook. `on_trade` follows later.
- Inside a hot hook, `self` is a record holding the instrument's parameters and `State` fields. `ctx` holds time, tick, lot, position, quoting state and fill fields. `book` holds the top of book and up to 10 levels per side; `book.n_bids` and `book.n_asks` count the levels present.
- Values are floats; `_raw` fields give the int64 fixed-point values and `fastmm.fx` has exact helpers. Prices and quantities written as floats are rounded to the nearest 1e-8, then to tick and lot by the same rule as C++ strategies.
- Hooks write intents through `ctx` methods (`quote`, `bid`, `ask`, `clear`, `pull`, `uncross`, `keep_passive`, `fail`) into a preallocated struct and never call into the engine. The engine passes the intents through risk, the OMS and the QuoteManager.
- The engine copies the parameter block into `self` before each call, so assignments to parameters inside a hook do not persist. `State` fields persist.
- Hot code is compiled with `boundscheck=True`. The docs state its measured cost.
- A generated wrapper catches every exception and returns a status. A non-zero status pulls quotes, disables the hot hooks and trips the kill switch with `KillReason::StrategyError`. With `on_kill = "exit"` the process exits with code 6. In replay the adapter stops calling hooks, as in ADR-0012.
- Before accepting a hook, FastMM scans the LLVM IR of every compiled function except Numba's generated C wrapper. A call outside an allowlist (LLVM intrinsics, libm, `NRT_MemInfo_call_dtor`) is rejected with the hook name and the symbol. This catches allocation, `print` and other Python API use. Calls through ctypes or cffi pointers are not detectable and are documented as unsupported. A source lint flags assignments to parameters.
- Compilation and warm-up finish before any venue connection. Warm-up calls each hook on scratch copies of `ctx`, `book` and `self`; nothing is journaled or sent, and `State` is reset afterwards.
- Live sessions always compile without Numba's cache, because a cached hook does not notice changes to FastMM's own overloads. Backtests use a cache directory keyed by the fastmm version, the ABI hash, the numba version and the CPU.

Slow methods:

- Methods without `@fastmm.hot` are plain Python on one slow thread and may use any library. The slow thread runs `on_start`, `on_stop` and methods decorated `@fastmm.every(period, timeout="10s")`. It has no event hooks.
- A class with a `@fastmm.hot` method may not define ADR-0012 hooks; this raises TypeError at class creation. Parameter and `State` names share one namespace, and duplicates also raise TypeError.
- `ctx.snapshot()` returns a copy of the latest engine state per instrument: top of book, position, PnL, quoting state and the parameters' age. The engine publishes it at a bounded rate. `ctx.recent(inst)` returns a bounded numpy array of recent top-of-book changes and trades for model features, with a count of rows that did not fit.
- `ctx.fills()` returns the fills since the last call, each with a sequence number. The ring is sized for the configured order rate limit; if it overflows, the slow tier fails (section 4). Order updates do not reach the slow tier.
- `ctx.publish(inst=None, **values)` names any subset of parameters. The named values apply together at one engine event and the others keep their values; `inst=None` applies to every instrument. Type, `min`/`max` and `validate()` run in the calling thread on a copy of the parameters, and an invalid or unknown name raises ValueError there. The engine only copies a validated binary block.
- `strategy.publish(...)` does the same from any Python thread while the session runs, for models that run outside the slow thread.
- The slow tier does not place orders.

A strategy without hot hooks does not run live. The ADR-0012 adapter stays in the backtest wheel.

### 2. Parameter updates are engine inputs

- A publish becomes a fixed-size `ParamUpdate` message: instrument, count, and up to 32 (field index u16, raw i64) pairs. Larger publishes raise at the call. The message goes over an SPSC ring that the engine drains with its other inputs; the publisher refuses when the ring is full, and the engine never waits.
- `EventType::ParamUpdate = 26` and `kJournalVersion = 3`. The journal header embeds the parameter name table, so field indices resolve in replay. Readers accept versions 1–3. Existing golden journals and `sample_1000.sha256` replay unchanged.
- The engine applies the pairs to its parameter storage, journals the message, and calls `on_params(ctx)`. C++ strategies get the same hook, which joins the hook table in `hooks.hpp` with near misses `on_param` and `on_parameters`.
- `fastmm.replay(journal, MyMM, verify=True)` replays a Python strategy from the journal's `ParamUpdate` messages without running the slow tier. The journal header records the class as `module:qualname`, a hash of the hot-hook source, and the fastmm and numba versions. A mismatch makes it a what-if replay.
- In backtests the slow tier runs synchronously at simulated times, and `slow_delay_ms` (default 0) delays when its publishes take effect. `BacktestResult` reports each slow method's wall time (p50 and p99) and warns when `slow_delay_ms` is below that p50. `ctx.snapshot()` and `ctx.fills()` behave the same in backtests and live.

### 3. Live runtime in the Python process

- `fastmm.run_live(StrategyClass, config, params=None)` and `python -m fastmm run pkg.module:Class --config file.toml` start the existing live session (`fastmm::live`) from an extension module, release the GIL and run the slow thread. `run_live` returns the exit code. `fastmm-live` does not link libpython.
- Signal handlers, exit codes, the status file, the journal, the kill switch and `on_kill` behave as in `fastmm-live`. A new exit code 7 means the slow tier failed and cancel-all succeeded; a failed cancel-all still gives 5, and a hot hook that does not compile gives 3.
- Before the engine starts, `run_live` sets the CPU affinity of every existing thread in `/proc/self/task`, other than the engine and network threads, to the cores not listed in `[engine]`; threads created later inherit it. Thread-count variables such as `OPENBLAS_NUM_THREADS` have no effect once numpy is imported, so `python -m fastmm run` sets them before importing the user module, and the docs show them on the command line for `run_live`.
- One session per process: a second `run_live` raises RuntimeError. A forked child gets an inert session (`os.register_at_fork`); the docs recommend the `spawn` or `forkserver` start method.

### 4. Safety

- `max_param_age_ms`: while no `ParamUpdate` was applied for that long in engine time, and before the first publish, quoting is disabled. `set_quotes` is ignored, `on_quoting(false)` fires, and the next publish fires `on_quoting(true)`. The default for a strategy with slow methods is three times its shortest `@every` period and at least 1 s; without slow methods it is off.
- The control thread watches the slow thread. An exception that escapes a slow method, a dead thread, a fills ring overflow, or an `@every` call running past its `timeout` stops the session like SIGTERM: kill switch, cancel-all, exit code 7. `on_start` runs before any venue connection and is not watched.
- The slow-tier channel owns its rings jointly with the session and outlives it; `publish` after the session stops returns False. If the slow thread has not returned `slow_tier_timeout_ms` (default 10 000) after stop, `python -m fastmm run` exits with `os._exit`.
- A native crash in the process, including a segfault in a hot hook, kills the engine without cancel-all. Venue cancel-on-disconnect is the remaining protection; the go-live checklist lists which venues offer it.

### 5. Packaging

- `fastmm` stays the backtest wheel without network code.
- `fastmm-live` is a second wheel from the same CI run, pinned to `fastmm==` the same version, with the network stack and OpenSSL 3 linked statically with hidden symbols. Its extension module exchanges only Python objects with `fastmm._core` and binds no C++ type that `_core` binds. `pip install "fastmm[live]"` installs it.
- TLS finds CA certificates from `SSL_CERT_FILE` and `SSL_CERT_DIR`, then `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt` and `/etc/ssl/cert.pem`, then `certifi`.
- `fastmm[hot]` adds `numba` and `llvmlite` within a tested version range; a CI job runs against the newest numba. Hot hooks raise a clear error when numba is missing.
- `fastmm[hot]` and `fastmm-live` need CPython 3.10 or later. Wheels are built for 3.9–3.14 with a cibuildwheel release that supports 3.14.

## Consequences

- Quoting code written in Python runs within tens of nanoseconds of C++, and model code runs as ordinary Python beside it. Backtests, replay and live use one class.
- Hot hooks are limited to Numba's nopython subset: scalars, the provided records and their methods, loops and arithmetic. No containers, strings, array creation or dynamic exceptions. Numba's error messages are hard to read; FastMM adds the hook name.
- Slow-tier values are always somewhat old. `ctx.snapshot()` carries its age, and `max_param_age_ms` bounds how old parameters may get.
- Heavy computation in the process still widens the engine's tail latency (0.78 ms p99.9 under matmul), even with threads pinned.
- Hot compilation relies on Numba's public `cfunc`, `overload_method` and record APIs. Ahead-of-time compilation needs a private Numba API and is not part of this decision.
- The engine gains a journaled input and a hook (`ParamUpdate`, `on_params`) that C++ strategies can use.
- Every OpenSSL advisory requires a new `fastmm-live` release.
- Python strategies run on Linux x86-64 with standard CPython. Free-threaded builds and subinterpreters are not supported.

## Implementation order

Steps 1, 2 and 5 can start in parallel. Step 3 needs step 1; step 4 needs steps 1–3 and the wheel skeleton from step 5.

1. Engine: `ParamUpdate`, journal version 3, `on_params`, `max_param_age_ms`, `KillReason::StrategyError`. Existing journals replay unchanged.
2. Hot tier in backtests: ABI header, adapter in `_core`, decorators, IR check, `fastmm.fx`. Tests: a hot BasicMM port reproduces `sample_1000.sha256`; a negative suite covers allocation, `print`, out-of-bounds access, parameter assignment and aliasing; ADR-0012 `python/tests` pass unchanged.
3. Slow tier in backtests: scheduling on simulated time, `publish`, `snapshot`, `recent`, `fills`, slow-method timing in `BacktestResult`; `fastmm.replay`.
4. Live runtime: extension module, `run_live`, `python -m fastmm run`, watchdog, thread affinity, exit code 7 in `session.hpp`, `docs/reference/cli.md` and the operations guides. Tests: a hot and slow session against the simulated exchange replays to the same hash; tick-to-order stays within 15% of C++ BasicMM.
5. Packaging: second wheel, static OpenSSL, CA lookup, extras, Python 3.14, CI.
6. Docs: Python strategy guide (hot hooks, slow methods, limits, measured latency), tutorial page, README line; then a Binance Demo session with a hot and slow Python strategy.
