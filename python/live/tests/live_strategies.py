"""Hot strategies for test_run_live.py, imported by the child processes it starts."""

import ctypes
import json
import os
import sys
import threading
import time
from pathlib import Path

import numpy as np

import fastmm
from fastmm import Param, State

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "examples" / "python" / "strategies"))

from basic_mm_hot import BasicMMHot  # noqa: E402,F401
from hot_slow_mm import HotSlowMM  # noqa: E402

# The thread-count variables as the module saw them when it was imported.
if os.environ.get("FASTMM_TEST_ENV_OUT"):
    Path(os.environ["FASTMM_TEST_ENV_OUT"]).write_text(" ".join(
        os.environ.get(v, "-") for v in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS",
                                         "MKL_NUM_THREADS")))


def _out(record):
    """Appends a JSON line to $FASTMM_TEST_OUT."""
    path = os.environ.get("FASTMM_TEST_OUT")
    if path:
        with open(path, "a") as f:
            f.write(json.dumps(record) + "\n")


class RaisesAfter(fastmm.Strategy):
    """Quotes around mid, then indexes past the book's levels on the `calls`-th book update."""

    calls = Param(20, min=1)
    seen = State(0)

    @fastmm.hot
    def on_book(self, ctx, book):
        self.seen += 1
        if self.seen >= self.calls:
            return book.bid_px[self.seen + 100]
        if book.valid:
            half = book.mid * 5e-4
            ctx.quote(book.mid - half, book.mid + half, 0.001)
            ctx.keep_passive()


class Allocates(fastmm.Strategy):
    """Fails the IR check: the hook builds an array."""

    @fastmm.hot
    def on_book(self, ctx, book):
        levels = np.zeros(4)
        ctx.quote(book.mid - levels[0], book.mid + 1.0, 0.001)


class NoHotHooks(fastmm.Strategy):
    def on_book(self, ctx, inst, book):
        pass


class HotSlowLive(HotSlowMM):
    """HotSlowMM with a second publisher: a thread started in on_start calls strategy.publish every
    100 ms. A 250 ms slow method reads the snapshot and the fills; on_stop writes what it saw."""

    def on_start(self, ctx):
        super().on_start(ctx)
        self.thread_publishes = [0, 0]  # accepted, refused
        self.fills_seen = 0
        self.last_fill_seq = 0
        self.snapshots = 0
        self.max_position = 0.0
        self.quit = threading.Event()
        self.publisher = threading.Thread(target=self._publish_loop, name="model")
        self.publisher.start()

    def _publish_loop(self):
        k = 0
        while not self.quit.wait(0.1):
            k += 1
            ok = self.publish(half_spread_bps=0.1 + 0.01 * (k % 5))
            self.thread_publishes[0 if ok else 1] += 1

    @fastmm.every("250ms")
    def watch(self, ctx):
        snap = ctx.snapshot()
        if snap.version > 0 and snap[0].book_valid:
            self.snapshots += 1
            self.max_position = max(self.max_position, abs(snap[0].position))
        fills = ctx.fills()
        if len(fills):
            assert fills["seq"][0] == self.last_fill_seq + 1, (fills["seq"], self.last_fill_seq)
            self.last_fill_seq = int(fills["seq"][-1])
            self.fills_seen += len(fills)

    def on_stop(self, ctx):
        self.quit.set()
        self.publisher.join()
        self.watch(ctx)
        _out({"fits": self.fits, "thread_publishes": self.thread_publishes,
              "fills_seen": self.fills_seen, "snapshots": self.snapshots,
              "on_stop_thread": threading.current_thread().name,
              "after_stop": self.publish(half_spread_bps=0.2)})


class SlowBase(fastmm.Strategy):
    """Quotes one level per side at mid -/+ half_spread_bps; slow methods keep the parameters fresh."""

    half_spread_bps = Param(5.0, min=0.0, max=100.0)
    calls_before = Param(3, min=0, max=1000, doc="slow calls before the failure")
    stall_s = Param(5.0, min=0.0, max=3600.0)

    @fastmm.hot
    def on_book(self, ctx, book):
        if not book.valid:
            return
        half = book.mid * self.half_spread_bps * 1e-4
        ctx.quote(book.mid - half, book.mid + half, 0.001)
        ctx.keep_passive()

    def on_start(self, ctx):
        self.calls = 0
        ctx.publish()


class SlowRaises(SlowBase):
    @fastmm.every("200ms")
    def model(self, ctx):
        self.calls += 1
        ctx.publish()
        if self.calls > self.calls_before:
            raise ValueError("model diverged")


class SlowStalls(SlowBase):
    """Stops publishing and sleeps past its timeout: max_param_age_ms (1000 ms) pulls the quotes
    before the watchdog stops the session."""

    @fastmm.every("200ms", timeout="3s")
    def model(self, ctx):
        self.calls += 1
        ctx.publish()
        if self.calls > self.calls_before:
            time.sleep(self.stall_s)


class SlowThreadDies(SlowBase):
    """Ends the slow thread with pthread_exit: no Python code runs after it, so only the session's
    thread check notices."""

    @fastmm.every("200ms")
    def model(self, ctx):
        self.calls += 1
        ctx.publish()
        if self.calls > self.calls_before:
            ctypes.CDLL(None).pthread_exit(None)


class SlowNeverDrains(SlowBase):
    """Publishes from inside one long call, so fills pile up in the ring undrained."""

    @fastmm.every("100ms", timeout="60s")
    def model(self, ctx):
        end = time.monotonic() + self.stall_s
        while time.monotonic() < end:
            if not ctx.publish():  # the session has stopped
                return
            time.sleep(0.1)


class SlowStartsSlowly(SlowBase):
    """on_start takes longer than the timeout of the slow method."""

    def on_start(self, ctx):
        time.sleep(self.stall_s)
        super().on_start(ctx)

    @fastmm.every("200ms", timeout="1s")
    def model(self, ctx):
        ctx.publish()


class SlowStopHangs(SlowBase):
    """on_stop never returns."""

    @fastmm.every("200ms")
    def model(self, ctx):
        ctx.publish()

    def on_stop(self, ctx):
        _out({"on_stop": "entered"})
        time.sleep(3600)


class SlowMatmul(SlowBase):
    """Latency measurement: `stall_s` > 0 multiplies 400x400 matrices for that many seconds out of
    every 100 ms."""

    @fastmm.every("100ms")
    def model(self, ctx):
        ctx.publish()
        if self.stall_s > 0:
            a = np.random.default_rng(self.calls).random((400, 400))
            end = time.monotonic() + self.stall_s
            while time.monotonic() < end:
                a = (a @ a) / 400.0
        self.calls += 1
