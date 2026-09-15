"""Cost of Python hot hooks against C++ basic_mm.

    python bench/python/bench_hot_strategy.py [--build build/release] [--seconds 3600] [--repeat 5]

Sections:

- hook: ns per on_book call without the engine. C++ BasicMM::on_book against HotStrategy::on_book
  running BasicMMHot (exact) and BasicMMHotFloat (floats), with and without bounds checks, on the
  same four books, positions and parameters. Needs the _hot_bench module of an in-tree build with
  FASTMM_BUILD_PYTHON and FASTMM_BUILD_BENCH (<build>/bench/python).
- compile: compile time of each class in a fresh process, without and then with the Numba cache.
- backtest: best wall time of --repeat runs and market-data events per second on the synthetic market
  of configs/backtest-example.toml; hooks are compiled before timing. The added cost per hook call is
  (hot - C++) / hook calls.

Not a CI benchmark; the numbers in docs/reference/python-api.md come from it.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EXAMPLES = REPO / "examples" / "python" / "strategies"
sys.path.insert(0, str(EXAMPLES))

import fastmm  # noqa: E402
from fastmm._hot import compiler  # noqa: E402

from basic_mm_hot import BasicMMHot, BasicMMHotFloat  # noqa: E402

HOOK_PARAMS = {"half_spread_bps": 1.25, "skew_bps_per_unit": 0.5, "quote_qty": 0.002,
               "max_inventory": 0.01, "levels": 2, "level_step_ticks": 2,
               "requote_threshold_ticks": 0, "pull_on_stale_ms": 0}


def section_hook(build: Path, iters: int, runs: int, rounds: int) -> None:
    """Every case once per round, interleaved, so load from other processes hits all of them; the
    table shows the fastest run of each case and the median over rounds of each round's fastest."""
    sys.path.insert(0, str(build / "bench" / "python"))
    import _hot_bench

    ref = BasicMMHot()
    ref.configure(HOOK_PARAMS)
    cases = [("C++ BasicMM::on_book", lambda: _hot_bench.cpp_on_book(ref.param_values(), iters,
                                                                      runs))]
    keep = []
    for cls in (BasicMMHot, BasicMMHotFloat):
        for boundscheck in (True, False):
            inst = cls()
            inst.configure(HOOK_PARAMS)
            compiled = compiler.CompiledHot(cls._fastmm_hot, cache=False, boundscheck=boundscheck)
            keep.append(compiled)
            program = compiled.program(inst)
            label = f"{cls.__name__}, boundscheck={boundscheck}"
            cases.append((label, lambda p=program: _hot_bench.hot_on_book(p, iters, runs)))
            if cls is BasicMMHot and boundscheck:
                forced = dict(program, book_depth=True)
                cases.append((f"{cls.__name__}, 10 levels copied",
                              lambda p=forced: _hot_bench.hot_on_book(p, iters, runs)))
    best = {label: [] for label, _ in cases}
    for _ in range(rounds):
        for label, run in cases:
            best[label].append(min(run()))
    print(f"\n## hook: ns per on_book call ({rounds} rounds x {runs} runs x {iters:,} calls)")
    print(f"{'case':44s} {'fastest':>8s} {'median':>8s}")
    for label, mins in best.items():
        print(f"{label:44s} {min(mins):8.1f} {statistics.median(mins):8.1f}")


_COMPILE = """
import json, sys, time
t0 = time.perf_counter()
import numba
t1 = time.perf_counter()
sys.path.insert(0, {examples!r})
import fastmm
from fastmm._hot import compiler
from basic_mm_hot import {name}
t2 = time.perf_counter()
c = compiler.compiled({name}, {name}._fastmm_hot, cache={cache})
t3 = time.perf_counter()
print(json.dumps({{"numba_import_s": t1 - t0, "compile_s": t3 - t2,
                  "hits": sum(c.cache_hits.values()), "hooks": len(c.cfuncs)}}))
"""


def section_compile() -> None:
    print("\n## compile: seconds in a fresh process")
    print(f"{'class':18s} {'numba import':>12s} {'cold':>8s} {'cached':>8s} {'hooks':>6s}")
    for name in ("BasicMMHot", "BasicMMHotFloat"):
        with tempfile.TemporaryDirectory() as d:
            env = dict(os.environ, FASTMM_CACHE_DIR=d)
            out = []
            for cache in (False, True, True):
                code = _COMPILE.format(examples=str(EXAMPLES), name=name, cache=cache)
                p = subprocess.run([sys.executable, "-c", code], env=env, check=True,
                                   capture_output=True, text=True)
                out.append(json.loads(p.stdout))
        cold, cached = out[0], out[2]
        assert cached["hits"] == cached["hooks"], cached
        print(f"{name:18s} {cold['numba_import_s']:12.2f} {cold['compile_s']:8.2f} "
              f"{cached['compile_s']:8.2f} {cold['hooks']:6d}")


def section_backtest(seconds: int, repeat: int) -> None:
    cfg = fastmm.BacktestConfig.from_toml(REPO / "configs" / "backtest-example.toml")
    cfg.duration_s = seconds

    def best(run):
        walls = []
        for _ in range(repeat):
            t0 = time.perf_counter()
            r = run()
            walls.append(time.perf_counter() - t0)
        return min(walls), r

    cpp_wall, cpp = best(lambda: fastmm.run_backtest(cfg, data="synthetic", strategy="basic_mm"))
    print(f"\n## backtest: synthetic market, {seconds} s, {cpp.md_events:,} market-data events, "
          f"best of {repeat}")
    print(f"{'case':18s} {'wall s':>8s} {'events/s':>10s} {'hook calls':>11s} {'ns/call over C++':>17s}")
    print(f"{'C++ basic_mm':18s} {cpp_wall:8.3f} {cpp.md_events / cpp_wall / 1e6:9.2f}M")
    for cls in (BasicMMHot, BasicMMHotFloat):
        inst = cls()
        inst.configure(dict(cfg.params))
        name = cls.strategy_name()
        compiler.compiled(cls, cls._fastmm_hot, cache=False)
        wall, (r, err, calls) = best(lambda: compiler.run(cfg, "synthetic", inst, name,
                                                          cls._fastmm_hot, False,
                                                          inst.param_values()))
        assert err is None, err
        same = "same orders" if r.outbound_sha256 == cpp.outbound_sha256 else "different orders"
        print(f"{cls.__name__:18s} {wall:8.3f} {r.md_events / wall / 1e6:9.2f}M {calls:11,d} "
              f"{(wall - cpp_wall) / calls * 1e9:17.1f}  ({same})")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sections", nargs="*", default=["hook", "compile", "backtest"])
    ap.add_argument("--build", type=Path, default=REPO / "build" / "release")
    ap.add_argument("--iters", type=int, default=1_000_000)
    ap.add_argument("--runs", type=int, default=7)
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--seconds", type=int, default=3600)
    ap.add_argument("--repeat", type=int, default=5)
    args = ap.parse_args()
    print(f"fastmm {fastmm.__version__}, numba {compiler.numba.__version__}, "
          f"{compiler.llvm.get_host_cpu_name()}")
    if "hook" in args.sections:
        section_hook(args.build, args.iters, args.runs, args.rounds)
    if "compile" in args.sections:
        section_compile()
    if "backtest" in args.sections:
        section_backtest(args.seconds, args.repeat)


if __name__ == "__main__":
    main()
