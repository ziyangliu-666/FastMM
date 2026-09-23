#!/usr/bin/env python3
"""Check measured times against the budgets in bench/ci_budget.toml.

usage: check_budgets.py bench/results/latest [--budget bench/ci_budget.toml] [--slack 0.10]
Exit 1 if any benchmark exceeds budget * (1 + slack), reported an error (SkipWithError), or has a
median counter below its floor in [min_counters]. Benchmarks missing from results are reported, not
failed.

The compared figure is the fastest repetition, not the median that bench/README.md publishes: the
machines these run on are shared, and interference only ever makes a benchmark slower, so the
fastest repetition is the least disturbed estimate and the one a budget can be tight around. The
median is printed next to it.

Budgets are set from a measurement on the reference machine named in bench/ci_budget.toml, so the
default slack is small: it covers that machine's own run-to-run spread, nothing else. CI runs this
on a hosted runner, whose hardware is slower and whose load is unknown, with an explicit larger
--slack; see .github/workflows/ci.yml.
"""
import argparse
import glob
import json
import os
import sys

try:
    import tomllib  # py3.11+
except ImportError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

sys.path.insert(0, os.path.dirname(__file__))
from bench_table import load  # noqa: E402


def errors(paths):
    """Benchmark run name -> error message for every repetition that called SkipWithError.

    Google Benchmark leaves errored repetitions out of the aggregates; when all of them errored the
    file holds no aggregates at all, so the benchmark would otherwise look merely missing.
    """
    out = {}
    for p in paths:
        with open(p) as f:
            data = json.load(f)
        for b in data.get("benchmarks", []):
            if b.get("error_occurred"):
                out[b.get("run_name", b["name"])] = b.get("error_message", "")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--budget", default=os.path.join(os.path.dirname(__file__), "..", "bench", "ci_budget.toml"))
    ap.add_argument("--slack", type=float, default=0.10,
                    help="allowance over the budget (0.10 = 10%%); use more on unknown hardware")
    a = ap.parse_args()
    cfg = tomllib.load(open(a.budget, "rb"))
    budgets = cfg["p50_ns"]
    floors = cfg.get("min_counters", {})
    paths = glob.glob(os.path.join(a.results, "*.json"))
    rows = load(paths)
    errs = errors(paths)
    bad = 0
    for name, budget in budgets.items():
        if name in errs:
            print(f"{name:60} ERROR  {errs[name]}")
            bad += 1
            continue
        r = rows.get(name)
        if r is None or "min" not in r:
            print(f"{name:60} missing")
            continue
        best = r["min"]
        limit = budget * (1 + a.slack)
        status = "OK" if best <= limit else "OVER"
        bad += status == "OVER"
        print(f"{name:60} {best:10.1f} ns (median {r['median']:9.1f})  budget {budget:8.0f} ns "
              f"(+{a.slack * 100:.0f}% = {limit:8.0f})  {status}")
    for name, counters in floors.items():
        if name in errs and name not in budgets:
            print(f"{name:60} ERROR  {errs[name]}")
            bad += 1
            continue
        r = rows.get(name)
        if r is None or "median" not in r:
            continue  # reported above if budgeted; an errored run is already counted
        for counter, floor in counters.items():
            v = r["counters"].get(counter)
            status = "OK" if v is not None and v >= floor else "LOW"
            bad += status == "LOW"
            shown = "absent" if v is None else f"{v:.3f}"
            print(f"{name:60} {counter} {shown}  floor {floor}  {status}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
