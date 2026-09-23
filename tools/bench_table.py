#!/usr/bin/env python3
"""Render Google Benchmark JSON files into the markdown table used by bench/README.md.

usage: bench_table.py results/*.json --template bench/README.tmpl.md --preset release-native --cpu 2

Reads the raw per-repetition rows (scripts/bench.sh does not pass
--benchmark_report_aggregates_only), not Google Benchmark's own aggregates: its "median" is the
median of the repetition means and its "stddev" the standard deviation of those means, which is a
measure of how stable the machine was, not of how the operation's own time varies. What this script
reports instead, per benchmark:

  * time per iteration, fastest run: the smallest per-iteration mean over all repetitions of all
    rounds. Interference only ever makes a benchmark slower, so on a machine shared with other work
    the fastest repetition is the least disturbed estimate.
  * CPU: the process CPU time of that same repetition. It should nearly equal the wall figure next
    to it for anything that does not wait; where it does not, even the fastest run was descheduled
    and the row cannot be trusted.
  * median and spread: the median over all repetitions and the smallest-to-largest range, which is
    what the machine's load did to this benchmark while it was measured.
  * p50 / p99: per-operation percentiles, only for the benchmarks that record every operation in a
    LogLinearHistogram and export them as counters (p50/p99, or p50_ns/p99_ns for the codec
    benchmarks, which time batches). Counters are shown at their median over repetitions.

Google Benchmark reports one mean per repetition, so no per-iteration dispersion exists for the
benchmarks without a histogram; the table leaves those cells empty rather than printing a zero.

The template's `{{TABLE}}`, `{{MACHINE}}`, `{{DATE}}` placeholders are substituted.
"""
import argparse
import datetime as dt
import json
import os
import platform
import re
import statistics
import subprocess
import sys

_SCALE = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}
_META = {"name", "run_name", "run_type", "repetitions", "repetition_index", "threads", "iterations",
         "real_time", "cpu_time", "time_unit", "aggregate_name", "aggregate_unit", "family_index",
         "per_family_instance_index", "label", "error_occurred", "error_message"}


def machine() -> str:
    """Core count, architecture and OS family; no CPU model or kernel version."""
    wsl = " (WSL2)" if "microsoft" in platform.release().lower() else ""
    return f"{os.cpu_count()}-core {platform.machine()} | {platform.system()}{wsl}"


def compiler() -> str:
    """Compiler name and major version."""
    for c in ("g++", "clang++"):
        try:
            out = subprocess.check_output([c, "--version"], text=True)
        except Exception:
            continue
        m = re.search(r"(\d+)\.\d+", out)
        return f"{c} {m.group(1)}" if m else c
    return "unknown"


def fmt_ns(v: float) -> str:
    if v != v:  # NaN
        return "?"
    if v < 1e3:
        return f"{v:,.1f} ns"
    if v < 1e6:
        return f"{v / 1e3:,.2f} µs"
    return f"{v / 1e6:,.2f} ms"


def load(paths):
    """name -> {median, min, max, reps, iterations, counters}; times in ns per iteration.

    `median`, `min` and `max` are over the repetitions of one benchmark; each repetition
    contributes its own mean time per iteration. Errored repetitions are skipped.
    """
    raw = {}
    for p in paths:
        with open(p) as f:
            data = json.load(f)
        for b in data.get("benchmarks", []):
            if b.get("run_type") != "iteration" or b.get("error_occurred"):
                continue
            name = b.get("run_name", b["name"])
            scale = _SCALE[b.get("time_unit", "ns")]
            r = raw.setdefault(name, {"times": [], "cpu": [], "counters": {}, "iterations": []})
            r["times"].append(b["real_time"] * scale)
            r["cpu"].append(b.get("cpu_time", float("nan")) * scale)
            r["iterations"].append(b.get("iterations", 0))
            for k, v in b.items():
                if k in _META or not isinstance(v, (int, float)):
                    continue
                r["counters"].setdefault(k, []).append(v)
    rows = {}
    for name, r in raw.items():
        if not r["times"]:
            continue
        best = min(range(len(r["times"])), key=lambda i: r["times"][i])
        rows[name] = {
            "min": r["times"][best],
            "cpu": r["cpu"][best],
            "median": statistics.median(r["times"]),
            "max": max(r["times"]),
            "reps": len(r["times"]),
            "iterations": min(r["iterations"]) if r["iterations"] else 0,
            "counters": {k: statistics.median(v) for k, v in r["counters"].items()},
        }
    return rows


def _spread(r) -> str:
    """The median over the repetitions, then the min-to-max range and how wide it is."""
    lo, hi = r["min"], r["max"]
    if r["reps"] < 2 or lo <= 0:
        return fmt_ns(r["median"])
    return f"{fmt_ns(r['median'])} ({fmt_ns(lo)} – {fmt_ns(hi)}, +{(hi - lo) / lo * 100.0:.0f} %)"


def _percentiles(c) -> str:
    """Per-operation p50 / p99 from a benchmark's own histogram, in ns."""
    if "p50" in c or "p99" in c:
        return f"{fmt_ns(c.get('p50', float('nan')))} / {fmt_ns(c.get('p99', float('nan')))}"
    if "p50_ns" in c or "p99_ns" in c:
        return f"{fmt_ns(c.get('p50_ns', float('nan')))} / {fmt_ns(c.get('p99_ns', float('nan')))}"
    return ""


def render(rows):
    out = ["| Benchmark | per iteration, fastest run | CPU there | median over runs (min – max) | per-op p50 / p99 | throughput |",
           "|---|---:|---:|---:|---:|---:|"]
    for name in sorted(rows):
        r = rows[name]
        c = r["counters"]
        thr = ""
        if "items_per_second" in c:
            thr = f"{c['items_per_second'] / 1e6:,.1f} Mops/s"
        elif "bytes_per_second" in c:
            thr = f"{c['bytes_per_second'] / 1e9:,.2f} GB/s"
        out.append(f"| `{name}` | {fmt_ns(r['min'])} | {fmt_ns(r['cpu'])} | {_spread(r)} "
                   f"| {_percentiles(c)} | {thr} |")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json", nargs="+")
    ap.add_argument("--template", required=True)
    ap.add_argument("--preset", default="release-native")
    ap.add_argument("--cpu", default="?")
    a = ap.parse_args()
    rows = load(a.json)
    reps = max((r["reps"] for r in rows.values()), default=0)
    setup = (f"{machine()} | {compiler()} | preset `{a.preset}` | pinned to CPU {a.cpu} "
             f"(benchmarks that declare they need more cores run unpinned) | "
             f"{reps} repetitions pooled over all rounds")
    tmpl = open(a.template).read()
    sys.stdout.write(tmpl.replace("{{TABLE}}", render(rows)).replace("{{MACHINE}}", setup)
                     .replace("{{DATE}}", dt.date.today().isoformat()))


if __name__ == "__main__":
    main()
