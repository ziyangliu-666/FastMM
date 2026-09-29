#!/usr/bin/env python3
"""Render benchmark results into bench/README.md, bench/full-results.md and the results region of
docs/explanation/benchmarks.md.

usage: bench_table.py bench/results/latest/*.json --template bench/README.tmpl.md \
           [--e2e bench/results/e2e] [--full bench/full-results.md] \
           [--docs docs/explanation/benchmarks.md] --preset release-native --cpu 2 > bench/README.md

Reads the raw per-repetition rows (scripts/bench.sh does not pass
--benchmark_report_aggregates_only), not Google Benchmark's own aggregates. Per benchmark:

  * p50 / p99 / p99.9: per-operation percentiles, for the benchmarks that time every operation
    with rdtsc into a LogLinearHistogram and export them as counters (p50/p99/p999, or
    p50_ns/p99_ns for the codec benchmarks, which time batches of 64). Counters are shown at their
    median over repetitions.
  * otherwise the median over all repetitions of Google Benchmark's mean time per iteration,
    divided by the operations one iteration performs (HOT_PATH below).

The full table (bench/full-results.md) also gives the fastest repetition, its CPU time and the
min-to-max range over every repetition of every round.

The end-to-end table reads the run<N>/sim.json and run<N>/live.json files scripts/bench-e2e.sh
writes (--out), and gives the median over the runs of each percentile.

The template's `{{HOT}}`, `{{E2E}}`, `{{MACHINE}}` and `{{DATE}}` placeholders are substituted; in
the docs page, the regions between `<!-- BEGIN bench-hot -->` / `<!-- END bench-hot -->` and
`<!-- BEGIN bench-e2e -->` / `<!-- END bench-e2e -->`.
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
from pathlib import Path

_SCALE = {"ns": 1, "us": 1e3, "ms": 1e6, "s": 1e9}
_META = {"name", "run_name", "run_type", "repetitions", "repetition_index", "threads", "iterations",
         "real_time", "cpu_time", "time_unit", "aggregate_name", "aggregate_unit", "family_index",
         "per_family_instance_index", "label", "error_occurred", "error_message"}

# Stage, operation, benchmark, operations per iteration (for the benchmarks without a histogram).
HOT_PATH = [
    ("Market data", "Binance book ticker, JSON decode", "BM_Json_BinanceBookTicker", 1),
    ("", "Nasdaq ITCH message into the L2 book", "BM_ItchL2Bridge_Message", 1),
    ("", "L2 book, apply a 20-level delta", "BM_L2_ApplyDelta/20", 1),
    ("Engine", "Tick to order, simulator", "BM_TickToOrder_Sim/manual_time", 1),
    ("", "Strategy: two quotes (`basic_mm`)", "BM_BasicMM_ComputeQuotes", 1),
    ("", "Quote diff against resting orders", "BM_QuoteManager_Reconcile_6Orders1Change", 1),
    ("", "Pre-trade risk check", "BM_Risk_CheckNew_Pass", 1),
    ("", "OMS: submit an order", "BM_Oms_Submit", 1),
    ("", "Journal: record a 128-byte event", "BM_Journal_Record/128", 1),
    ("Orders", "Binance `order.place`, HMAC-signed", "BM_Encode_BinanceOrderPlace", 1),
    ("", "Binance `order.place`, Ed25519 session", "BM_Encode_BinanceOrderPlace_Session", 1),
    ("", "OUCH 5.0 Enter Order", "BM_Ouch50_EncodeNewIds", 1),
]

BEGIN_END = re.compile(r"(<!-- BEGIN (bench-\w+) -->\n).*?(<!-- END \2 -->)", re.S)


def machine() -> str:
    """CPU model, core count and OS family; no kernel version."""
    model = ""
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    model = line.split(":", 1)[1].strip() + ", "
                    break
    except OSError:
        pass
    wsl = " under WSL2" if "microsoft" in platform.release().lower() else ""
    return f"{model}{os.cpu_count()} cores, {platform.system()}{wsl}"


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
    """name -> {median, min, max, cpu, reps, counters}; times in ns per iteration.

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
            r = raw.setdefault(name, {"times": [], "cpu": [], "counters": {}})
            r["times"].append(b["real_time"] * scale)
            r["cpu"].append(b.get("cpu_time", float("nan")) * scale)
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
            "counters": {k: statistics.median(v) for k, v in r["counters"].items()},
        }
    return rows


def _pct(c, key):
    """A per-operation percentile counter, in ns, or None."""
    for k in (key, f"{key}_ns"):
        if k in c:
            return c[k]
    return None


def render_hot(rows):
    """The HOT_PATH rows; the p99.9 column only when some row has one."""
    has999 = any(_pct(rows[n]["counters"], "p999") is not None for _, _, n, _ in HOT_PATH if n in rows)
    out = ["| Stage | Operation | p50 | p99 | p99.9 | Benchmark |" if has999 else
           "| Stage | Operation | p50 | p99 | Benchmark |",
           "|---|---|---:|---:|---:|---|" if has999 else "|---|---|---:|---:|---|"]
    for stage, op, name, per_iter in HOT_PATH:
        r = rows.get(name)
        if r is None:
            continue
        c = r["counters"]
        p50, p99, p999 = _pct(c, "p50"), _pct(c, "p99"), _pct(c, "p999")
        if p50 is None:
            p50 = r["median"] / per_iter
        cells = [fmt_ns(p50), fmt_ns(p99) if p99 is not None else ""]
        if has999:
            cells.append(fmt_ns(p999) if p999 is not None else "")
        bench = name.split("/manual_time")[0]
        out.append(f"| {stage} | {op} | {' | '.join(cells)} | `{bench}` |")
    return "\n".join(out)


def render_full(rows):
    out = ["| Benchmark | median | fastest | CPU of fastest | range | per-op p50 / p99 | throughput |",
           "|---|---:|---:|---:|---:|---:|---:|"]
    for name in sorted(rows):
        r = rows[name]
        c = r["counters"]
        thr = ""
        if "items_per_second" in c:
            thr = f"{c['items_per_second'] / 1e6:,.1f} Mops/s"
        elif "bytes_per_second" in c:
            thr = f"{c['bytes_per_second'] / 1e9:,.2f} GB/s"
        p50, p99 = _pct(c, "p50"), _pct(c, "p99")
        pct = f"{fmt_ns(p50)} / {fmt_ns(p99)}" if p50 is not None and p99 is not None else ""
        spread = ""
        if r["reps"] > 1 and r["min"] > 0:
            spread = f"+{(r['max'] - r['min']) / r['min'] * 100.0:.0f} %"
        out.append(f"| `{name}` | {fmt_ns(r['median'])} | {fmt_ns(r['min'])} | {fmt_ns(r['cpu'])} "
                   f"| {spread} | {pct} | {thr} |")
    return "\n".join(out)


def render_e2e(d: Path):
    runs = sorted(p for p in d.glob("run*") if (p / "sim.json").exists() and (p / "live.json").exists())
    if not runs:
        return ""
    hops = {"wire": [], "k2t0": [], "t0t5": [], "ouch": []}
    for run in runs:
        sim = json.load(open(run / "sim.json"))
        live = json.load(open(run / "live.json"))
        venue = live["venues"][0]
        w = sim["wire_to_wire_ns"]
        hops["wire"].append((w["count"], w["p50"], w["p99"], w["p999"]))
        for key, src in (("k2t0", venue["feed"]["kernel_to_t0"]),
                         ("t0t5", live["latency"]["tick_to_trade"]),
                         ("ouch", venue["wire_tick_to_trade"])):
            hops[key].append((src["count"], src["p50_ns"], src["p99_ns"], src["p999_ns"]))
    names = {
        "wire": "Wire to wire: market data sent to order received",
        "k2t0": "Kernel receive to T0 (network thread)",
        "t0t5": "T0 to T5: engine, tick to order in the ring",
        "ouch": "T0 to the order's `write` returning",
    }
    out = ["| Interval | samples | p50 | p99 | p99.9 |", "|---|---:|---:|---:|---:|"]
    for key, label in names.items():
        v = hops[key]
        n = sum(x[0] for x in v)
        med = [statistics.median(x[i] for x in v) for i in (1, 2, 3)]
        out.append(f"| {label} | {n:,} | " + " | ".join(fmt_ns(m) for m in med) + " |")
    return "\n".join(out) + f"\n\nMedian over {len(runs)} runs of each percentile."


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json", nargs="+")
    ap.add_argument("--template", required=True)
    ap.add_argument("--e2e", help="scripts/bench-e2e.sh output directory")
    ap.add_argument("--full", help="write the table of every benchmark here")
    ap.add_argument("--docs", help="update the generated regions of this page")
    ap.add_argument("--preset", default="release-native")
    ap.add_argument("--cpu", default="?")
    a = ap.parse_args()
    rows = load(a.json)
    reps = max((r["reps"] for r in rows.values()), default=0)
    setup = (f"{machine()}, {compiler()}, preset `{a.preset}`, pinned to CPU {a.cpu}, "
             f"{reps} repetitions")
    hot = render_hot(rows)
    e2e = render_e2e(Path(a.e2e)) if a.e2e else ""
    today = dt.date.today().isoformat()
    tmpl = open(a.template).read()
    sys.stdout.write(tmpl.replace("{{HOT}}", hot).replace("{{E2E}}", e2e)
                     .replace("{{MACHINE}}", setup).replace("{{DATE}}", today))
    if a.full:
        with open(a.full, "w") as f:
            f.write("# Every benchmark\n\nGenerated by `scripts/bench.sh` on "
                    f"{today}: {setup}. Times are per iteration; for the codec benchmarks an "
                    "iteration is a batch of 64 operations, for `BM_L2_ApplyDelta/20` a 20-level "
                    "delta, for `BM_EngineStep_Sim` a 32-event step. Method: "
                    "[Benchmarks](../docs/explanation/benchmarks.md).\n\n"
                    + render_full(rows) + "\n")
    if a.docs:
        page = Path(a.docs)
        regions = {"bench-hot": hot, "bench-e2e": e2e}

        def sub(m):
            body = regions.get(m.group(2))
            return m.group(0) if not body else f"{m.group(1)}{body}\n{m.group(3)}"

        page.write_text(BEGIN_END.sub(sub, page.read_text()))


if __name__ == "__main__":
    main()
