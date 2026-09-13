#!/usr/bin/env python3
"""Compare two directories of Google Benchmark JSON aggregates: bench_compare.py baseline/ latest/ [--threshold 0.15]"""
import argparse
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from bench_table import load  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline")
    ap.add_argument("latest")
    ap.add_argument("--threshold", type=float, default=0.15, help="regression ratio that fails (0.15 = 15%)")
    a = ap.parse_args()
    base = load(glob.glob(os.path.join(a.baseline, "*.json")))
    new = load(glob.glob(os.path.join(a.latest, "*.json")))
    worst = 0.0
    print(f"{'benchmark':60} {'baseline':>12} {'latest':>12} {'delta':>8}")
    for name in sorted(set(base) & set(new)):
        b, n = base[name].get("median"), new[name].get("median")
        if not b or not n:
            continue
        d = (n - b) / b
        worst = max(worst, d)
        flag = "  <-- REGRESSION" if d > a.threshold else ""
        print(f"{name:60} {b:12.1f} {n:12.1f} {d * 100:+7.1f}%{flag}")
    if worst > a.threshold:
        print(f"\nFAIL: worst regression {worst * 100:.1f}% > {a.threshold * 100:.0f}%")
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
