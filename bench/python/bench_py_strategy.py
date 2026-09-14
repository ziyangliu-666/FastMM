"""Cost of Python strategies against C++ basic_mm on the same data.

    python bench/python/bench_py_strategy.py [--seconds 3600] [--repeat 5]

Data: the seeded synthetic L2 queue market of configs/backtest-example.toml (book updates every
100 ms, book tickers and trades); every case replays exactly the same events. For each case it
prints the best wall time of --repeat runs, market-data events per second end to end (the synthetic
market and the simulated venue included), and the added cost per Python hook call:

- empty / reads: (case - "no hooks") / market-data events, one hook call per event;
- BasicMMExact: (case - C++ basic_mm) / (book updates + fills), the calls its on_book and on_fill
  receive; both send the same orders, so the difference is the strategy code alone.

Not a CI benchmark; the numbers in docs/reference/python-api.md come from it.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import fastmm
from fastmm import Strategy

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "examples" / "python" / "strategies"))

from basic_mm_exact import BasicMMExact  # noqa: E402
from skew_mm import SkewMM  # noqa: E402


class NoHooks(Strategy):
    """Defines no hook: the adapter never calls Python."""


class EmptyEveryEvent(Strategy):
    """One empty Python call per market-data event (book, ticker, trade)."""

    def on_book(self, ctx, inst, book):
        pass

    def on_book_ticker(self, ctx, inst, msg):
        pass

    def on_trade(self, ctx, inst, trade):
        pass


class ReadEveryEvent(EmptyEveryEvent):
    """Reads two or three fields on every event."""

    def on_book(self, ctx, inst, book):
        book.mid
        book.best_bid

    def on_book_ticker(self, ctx, inst, msg):
        msg.bid_price
        msg.ask_price

    def on_trade(self, ctx, inst, trade):
        trade.price
        ctx.position(inst).qty


def best_run(cfg, strategy, repeat):
    best = None
    result = None
    for _ in range(repeat):
        t0 = time.perf_counter()
        result = fastmm.run_backtest(cfg, data="synthetic", strategy=strategy)
        dt = time.perf_counter() - t0
        best = dt if best is None else min(best, dt)
    return best, result


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=3600.0)
    ap.add_argument("--repeat", type=int, default=5)
    args = ap.parse_args()

    cfg = fastmm.BacktestConfig.from_toml(REPO / "configs" / "backtest-example.toml")
    cfg.duration_s = args.seconds
    cfg.measure_wall_clock = False
    empty = cfg.copy()
    empty.clear_params()

    print(f"fastmm {fastmm.__version__}, Python {sys.version.split()[0]}; synthetic L2 queue "
          f"market of configs/backtest-example.toml, {args.seconds:.0f} s simulated, "
          f"best of {args.repeat}")
    runs = {}
    for label, c, strategy in [
        ("C++ basic_mm", cfg, "basic_mm"),
        ("Python BasicMMExact", cfg, BasicMMExact),
        ("Python SkewMM", empty, SkewMM),
        ("Python, no hooks", empty, NoHooks),
        ("Python, empty hook per event", empty, EmptyEveryEvent),
        ("Python, field reads per event", empty, ReadEveryEvent),
    ]:
        dt, r = best_run(c, strategy, args.repeat)
        runs[label] = (dt, r)
        print(f"{label:30s} {r.md_events:8d} events  {dt:6.3f} s  {r.md_events / dt / 1e6:5.2f} M "
              f"events/s  outbound {r.outbound_messages}")

    base_dt, base = runs["Python, no hooks"]
    for label in ("Python, empty hook per event", "Python, field reads per event"):
        dt, r = runs[label]
        print(f"{label:30s} +{(dt - base_dt) / r.md_events * 1e9:6.0f} ns per hook call")
    cpp_dt, cpp = runs["C++ basic_mm"]
    py_dt, py = runs["Python BasicMMExact"]
    calls = py.engine_stats()["book_updates"] + py.engine_stats()["fills"]
    same = "same outbound hash" if py.outbound_sha256 == cpp.outbound_sha256 else "HASH DIFFERS"
    per_call = (py_dt - cpp_dt) / calls * 1e9
    print(f"{'Python BasicMMExact':30s} +{per_call:6.0f} ns per on_book/on_fill call over C++ "
          f"({calls} calls, {same})")


if __name__ == "__main__":
    main()
