"""Hot strategies for test_run_live.py, imported by the child processes it starts."""

import os
import sys
from pathlib import Path

import numpy as np

import fastmm
from fastmm import Param, State

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "examples" / "python" / "strategies"))

from basic_mm_hot import BasicMMHot  # noqa: E402,F401

# The thread-count variables as the module saw them when it was imported.
if os.environ.get("FASTMM_TEST_ENV_OUT"):
    Path(os.environ["FASTMM_TEST_ENV_OUT"]).write_text(" ".join(
        os.environ.get(v, "-") for v in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS",
                                         "MKL_NUM_THREADS")))


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
