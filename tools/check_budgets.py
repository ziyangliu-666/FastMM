#!/usr/bin/env python3
"""Check measured medians against the p50 latency budgets in bench/ci_budget.toml.

usage: check_budgets.py bench/results/latest [--budget bench/ci_budget.toml] [--slack 0.25]
Exit 1 if any benchmark exceeds budget * (1 + slack). Benchmarks missing from results are reported, not failed.
"""
import argparse
import glob
import os
import sys

try:
    import tomllib  # py3.11+
except ImportError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

sys.path.insert(0, os.path.dirname(__file__))
from bench_table import load  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("--budget", default=os.path.join(os.path.dirname(__file__), "..", "bench", "ci_budget.toml"))
    ap.add_argument("--slack", type=float, default=0.25)
    a = ap.parse_args()
    budgets = tomllib.load(open(a.budget, "rb"))["p50_ns"]
    rows = load(glob.glob(os.path.join(a.results, "*.json")))
    bad = 0
    for name, budget in budgets.items():
        r = rows.get(name)
        if r is None or "median" not in r:
            print(f"{name:60} missing")
            continue
        med = r["median"]
        limit = budget * (1 + a.slack)
        status = "OK" if med <= limit else "OVER"
        bad += status == "OVER"
        print(f"{name:60} {med:10.1f} ns  budget {budget:8.0f} ns (+{a.slack * 100:.0f}% = {limit:8.0f})  {status}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
