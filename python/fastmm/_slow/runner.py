"""The slow runner: calls a strategy's on_start, @fastmm.every methods and on_stop against a slow
channel (ADR-0013, sections 1 and 4).

A backtest drives it from the engine thread at simulated times: start(), then wake() at each time
it returns, then stop(). A live session runs run_thread() on its slow thread with a wall clock. The
channel is fastmm._core._SlowChannel or another wrapper with the methods of
python/src/slow_channel_py.hpp; the runner records call deadlines and failures in it for the
session's watchdog.
"""

from __future__ import annotations

import threading
import time
from typing import TYPE_CHECKING, Any, Dict, List, Optional, Tuple

import numpy as np

from . import abi, context
from .decl import every_of
from .publish import Publisher

if TYPE_CHECKING:  # pragma: no cover
    from typing import Protocol

    from .._hot.decl import HotSpec

    class Clock(Protocol):
        def now_ns(self) -> int: ...

SNAPSHOT_INTERVAL_NS = 10_000_000
"""The engine publishes a snapshot at most once per 10 ms of engine time."""
IDLE_WAKE_NS = 1_000_000_000
"""Without @fastmm.every methods a backtest wakes the runner once per second to drain fills."""
LIVE_POLL_NS = 10_000_000
"""run_thread(): heartbeat and fills drain interval while no method is due."""


class WallClock:
    """Session time of live sessions: CLOCK_REALTIME nanoseconds, the clock engine times use."""

    def now_ns(self) -> int:
        return time.time_ns()


class SlowRunner:
    def __init__(self, instance: Any, spec: "HotSpec") -> None:
        self.instance = instance
        self.spec = spec
        self.methods: List[Tuple[str, Any, int, int]] = []
        for name, fn in spec.slow.items():
            mark = every_of(fn)
            assert mark is not None
            self.methods.append((name, fn, mark.period_ns, mark.timeout_ns))
        self.channel: Any = None
        self.publisher: Optional[Publisher] = None
        self.ctx = context.SlowContext(self)
        self.now_ns = 0
        self.failed_method: Optional[str] = None
        self.timings: Dict[str, List[int]] = {name: [] for name, *_ in self.methods}
        self._due: List[int] = []
        self._fills: List[np.ndarray] = []

    # ---- session side ----------------------------------------------------------------------------

    def start(self, channel: Any, now_ns: int) -> int:
        """Binds the channel, runs on_start and returns the first wake time (ns)."""
        self.channel = channel
        self.publisher = Publisher(type(self.instance), self.spec, self.instance, channel)
        self.instance.__dict__["_fastmm_publisher"] = self.publisher
        self.now_ns = now_ns
        channel.heartbeat(time.monotonic_ns())
        on_start = self.spec.lifecycle.get("on_start")
        if on_start is not None:
            self._call("on_start", on_start, 0)
        self._due = [now_ns] * len(self.methods)
        return self._next(now_ns)

    def wake(self, now_ns: int) -> int:
        """Runs the methods due at `now_ns`, earliest due first and then in declaration order, and
        returns the next wake time (ns). An exception from a method propagates after the channel
        records the failure."""
        self.now_ns = now_ns
        self.channel.heartbeat(time.monotonic_ns())
        self._drain()
        while True:
            best = -1
            for i, due in enumerate(self._due):
                if due <= now_ns and (best < 0 or due < self._due[best]):
                    best = i
            if best < 0:
                break
            name, fn, period, timeout = self.methods[best]
            nxt = self._due[best] + period
            if nxt <= now_ns:  # the previous call ran past whole periods: skip them, keep the phase
                nxt += ((now_ns - nxt) // period + 1) * period
            self._due[best] = nxt
            self._call(name, fn, timeout)
        return self._next(now_ns)

    def stop(self, now_ns: int) -> None:
        """Runs on_stop."""
        self.now_ns = now_ns
        on_stop = self.spec.lifecycle.get("on_stop")
        if on_stop is not None:
            self._call("on_stop", on_stop, 0)

    def run_thread(self, channel: Any, clock: "Clock", stop: threading.Event, *,
                   started: bool = False, poll_ns: int = LIVE_POLL_NS) -> None:
        """The loop of a live session's slow thread: start() unless `started`, then wake() whenever a
        method is due and a heartbeat and fills drain every `poll_ns` otherwise, until `stop` is
        set; then stop(). An exception propagates after the channel records SlowFailure::Exception.
        The watchdog reads the channel: check(time.monotonic_ns()) reports a call past its timeout."""
        try:
            nxt = self._next(clock.now_ns()) if started else self.start(channel, clock.now_ns())
            while not stop.is_set():
                now = clock.now_ns()
                if now >= nxt:
                    nxt = self.wake(now)
                else:
                    channel.heartbeat(time.monotonic_ns())
                    self._drain()
                wait = min(poll_ns, max(0, nxt - clock.now_ns()))
                if wait > 0:
                    stop.wait(wait / 1e9)
            self.stop(clock.now_ns())
        except BaseException:
            channel.fail(abi.FAILURE_EXCEPTION)
            raise

    # ---- ctx -------------------------------------------------------------------------------------

    def snapshot(self) -> context.Snapshot:
        return context.snapshot_from(self.channel.snapshot(), self.channel.symbols, self.now_ns)

    def recent(self, inst: Any) -> context.Recent:
        k = context.instrument_index(inst, self.channel.symbols)
        data, dropped = self.channel.recent(k)
        return context.recent_from(data, dropped)

    def take_fills(self) -> np.ndarray:
        self._drain()
        chunks, self._fills = self._fills, []
        return context.fills_from(chunks)

    # ---- reporting -------------------------------------------------------------------------------

    def timing_rows(self) -> List[Tuple[str, int, int, int, int]]:
        """(name, calls, p50 ns, p99 ns, max ns) of each @fastmm.every method's wall time."""
        rows = []
        for name, samples in self.timings.items():
            if samples:
                a = np.asarray(samples, dtype=np.int64)
                rows.append((name, len(a), int(np.percentile(a, 50)), int(np.percentile(a, 99)),
                             int(a.max())))
            else:
                rows.append((name, 0, 0, 0, 0))
        return rows

    # ---- internals -------------------------------------------------------------------------------

    def _next(self, now_ns: int) -> int:
        if self.methods:
            return min(self._due) if self._due else now_ns
        return now_ns + IDLE_WAKE_NS

    def _drain(self) -> None:
        data = self.channel.drain_fills()
        if data:
            self._fills.append(np.frombuffer(data, abi.FILL_DTYPE))

    def _call(self, name: str, fn: Any, timeout_ns: int) -> None:
        ch = self.channel
        t0 = time.monotonic_ns()
        if timeout_ns > 0:
            ch.begin_call(t0, timeout_ns)
        try:
            fn(self.instance, self.ctx)
        except BaseException:
            self.failed_method = name
            ch.fail(abi.FAILURE_EXCEPTION)
            raise
        finally:
            t1 = time.monotonic_ns()
            if timeout_ns > 0:
                ch.end_call(t1)
            else:
                ch.heartbeat(t1)
            if name in self.timings:
                self.timings[name].append(t1 - t0)
