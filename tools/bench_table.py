#!/usr/bin/env python3
"""Render Google Benchmark JSON files (aggregates) into the markdown table used by bench/README.md.

usage: bench_table.py results/*.json --template bench/README.tmpl.md --preset release-native --cpu 2
The template's `{{TABLE}}`, `{{MACHINE}}`, `{{DATE}}` placeholders are substituted.
"""
import argparse
import datetime as dt
import json
import os
import platform
import re
import subprocess
import sys


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
    if v < 1e3:
        return f"{v:,.1f} ns"
    if v < 1e6:
        return f"{v / 1e3:,.2f} µs"
    return f"{v / 1e6:,.2f} ms"


def load(paths):
    rows = {}
    for p in paths:
        with open(p) as f:
            data = json.load(f)
        for b in data.get("benchmarks", []):
            name = b["name"]
            m = re.match(r"^(.*?)_(mean|median|stddev|cv|min|max)$", name)
            if not m:
                continue  # non-aggregate line
            base, agg = m.group(1), m.group(2)
            unit = b.get("time_unit", "ns")
            scale = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}[unit]
            r = rows.setdefault(base, {"counters": {}})
            r[agg] = b["real_time"] * scale
            # user-defined counters (e.g. p50/p99/items_per_second) appear as extra keys
            for k, v in b.items():
                if k in ("name", "run_name", "run_type", "repetitions", "repetition_index", "threads",
                         "iterations", "real_time", "cpu_time", "time_unit", "aggregate_name",
                         "aggregate_unit", "family_index", "per_family_instance_index", "label"):
                    continue
                if isinstance(v, (int, float)) and agg == "median":
                    r["counters"][k] = v
    return rows


def render(rows):
    out = ["| Benchmark | median | stddev | p50 / p99 (if measured) | throughput |", "|---|---:|---:|---:|---:|"]
    for name in sorted(rows):
        r = rows[name]
        med = fmt_ns(r.get("median", float("nan")))
        sd = fmt_ns(r.get("stddev", 0.0))
        c = r["counters"]
        pct = ""
        if "p50" in c or "p99" in c:
            pct = f"{fmt_ns(c.get('p50', 0))} / {fmt_ns(c.get('p99', 0))}"
        thr = ""
        if "items_per_second" in c:
            thr = f"{c['items_per_second'] / 1e6:,.1f} Mops/s"
        elif "bytes_per_second" in c:
            thr = f"{c['bytes_per_second'] / 1e9:,.2f} GB/s"
        out.append(f"| `{name}` | {med} | {sd} | {pct} | {thr} |")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json", nargs="+")
    ap.add_argument("--template", required=True)
    ap.add_argument("--preset", default="release-native")
    ap.add_argument("--cpu", default="?")
    a = ap.parse_args()
    rows = load(a.json)
    machine = (f"{machine()} | {compiler()} | preset `{a.preset}` "
               f"| pinned to CPU {a.cpu} | 5 repetitions, median reported")
    tmpl = open(a.template).read()
    sys.stdout.write(tmpl.replace("{{TABLE}}", render(rows)).replace("{{MACHINE}}", machine)
                     .replace("{{DATE}}", dt.date.today().isoformat()))


if __name__ == "__main__":
    main()
