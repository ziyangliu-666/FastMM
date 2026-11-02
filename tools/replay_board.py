#!/usr/bin/env python3
"""How far a backtest over a live journal is from the live session, per time of day.

    python3 tools/replay_board.py j/session.fmj runs/r --start 2026-10-29T02:30:00Z \
        --buckets 'night=20-4,pre=4-9.5,reg=9.5-16,post=16-20' --tz America/New_York \
        --rebate-bps 0.8 --data ./build/release/bin/fastmm-data

`runs/r` is the output directory of `fastmm-backtest --data journal:j/session.fmj,strip_own=1`. The
session's fills come from its journal (`fastmm-data events`), the run's from its fills.csv, and both
are marked against the same recorded mid (`fastmm-data mark`), so the rows differ only by the fills.

The columns, live and run: maker fills and notional, buy share, edge at the fill, markouts at 60 s
and 300 s in bps of notional, and what the markout at 300 s and the rebate are worth. `money` marks
everything the fills and the start positions earn on the bar's mid; `long` is the mean of the sum of
long positions; `|pos|` the mean sum of absolute differences of the two runs' positions, in USD.
--positions gives both runs the positions at --start.

The standard library is enough; it reads every file once.
"""
import argparse
import bisect
import csv
import datetime as dt
import json
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from zoneinfo import ZoneInfo

HORIZONS = (0, 60, 300)
HEADER = (
    "bucket", "who", "hours", "fills", "notional", "buy%", "edge", "mo60", "mo300", "mm", "money", "long", "|pos|",
)


def parse_buckets(text):
    """'night=20-4,pre=4-9.5' -> [(name, from_hour, to_hour)]."""
    out = []
    for part in text.split(","):
        name, span = part.split("=")
        a, b = span.split("-")
        out.append((name, float(a), float(b)))
    return out


def bucket_of(t_ns, buckets, tz):
    t = dt.datetime.fromtimestamp(t_ns / 1e9, tz)
    h = t.hour + t.minute / 60
    for name, a, b in buckets:
        if (a <= h < b) if a < b else (h >= a or h < b):
            return name
    return "other"


def read_live(journals, data):
    """The session's fills from its journal: t_ns, symbol, side (+1 bought), price, qty, maker, fee."""
    with tempfile.NamedTemporaryFile("r", suffix=".csv") as f:
        subprocess.run([data, "events", *journals, "--out", f.name], check=True)
        rows = []
        seen = set()
        for r in csv.DictReader(f):
            if r["type"] != "fill":
                continue
            # A venue's execution comes once by stream and may again by reconciliation, flagged
            # as replayed: some only ever come that way. Each counts once, by its exec id.
            key = (r["venue"], r["symbol"], r["d"])
            if r["d"] and key in seen:
                continue
            seen.add(key)
            t = int(r["venue_time"] or 0) or int(r["time"])
            rows.append((t, r["symbol"], 1 if r["side"] == "B" else -1, float(r["price"]), float(r["qty"]), r["a"] == "1", float(r["b"] or 0)))
    return rows


def read_run(run):
    names = [str(i).split(":")[-1] for i in json.loads((run / "summary.json").read_text())["instruments"]]
    rows = []
    with open(run / "fills.csv") as f:
        for r in csv.DictReader(f):
            rows.append((int(r["ts_ns"]), names[int(r["inst"])], 1 if r["side"] == "B" else -1, float(r["price"]), float(r["qty"]), r["liquidity"] == "M", float(r["fee"])))
    return rows


def mark(journal, tables, data, bars):
    """The mids of each table's rows at HORIZONS, and the bars: {symbol: ([bar_ns], [mid])}."""
    with tempfile.TemporaryDirectory() as d:
        paths = []
        for k, rows in enumerate(tables):
            p = Path(d) / f"{k}.csv"
            with open(p, "w") as f:
                f.write("t_ns,symbol\n")
                f.writelines(f"{r[0]},{r[1]}\n" for r in rows)
            paths.append(str(p))
        b = Path(d) / "bars.csv"
        horizons = ",".join(map(str, HORIZONS))
        subprocess.run([data, "mark", journal, *paths, "--horizons-s", horizons, "--bars", str(b), "--bar-s", str(bars)], check=True)
        marks = []
        for p in paths:
            with open(p + ".marks") as f:
                marks.append([[float(x) if x else None for x in line.rstrip("\n").split(",")] for line in f])
        series = defaultdict(lambda: ([], []))
        with open(b) as f:
            for r in csv.DictReader(f):
                s = series[r["symbol"]]
                s[0].append(int(r["bar_ns"]))
                s[1].append(float(r["mid"]))
    return marks, series


def mid_at(series, symbol, t):
    ts, mids = series.get(symbol, ((), ()))
    k = bisect.bisect_right(ts, t) - 1
    return mids[k] if k >= 0 else None


def score(rows, marks, series, t0, t1, bar_ns, buckets, tz, start, rebate):
    """{bucket: stats} of one run's fills from t0 to t1, and its position in USD at the end of each bar."""
    fills = sorted(((r, m) for r, m in zip(rows, marks) if t0 <= r[0] < t1), key=lambda x: x[0][0])
    b = defaultdict(lambda: defaultdict(float))
    pos = defaultdict(float, start)
    path = {}
    k = 0
    bar = t0
    symbols = sorted(set(series) | set(start))
    last = {s: mid_at(series, s, t0) for s in symbols}
    while bar < t1:
        d = b[bucket_of(bar, buckets, tz)]
        d["bars"] += 1
        end = bar + bar_ns
        for s in symbols:  # what the position held through the bar earned
            mid = mid_at(series, s, end)
            if mid is not None and last.get(s) is not None:
                d["money"] += pos[s] * (mid - last[s])
            last[s] = mid if mid is not None else last.get(s)
        while k < len(fills) and fills[k][0][0] < end:
            (t, sym, sd, px, qty, maker, fee), m = fills[k]
            k += 1
            pos[sym] += sd * qty
            at = mid_at(series, sym, end)
            if at is not None:
                d["money"] += sd * qty * (at - px)
            d["money"] -= fee
            if not maker:
                continue
            usd = qty * px
            f = b[bucket_of(t, buckets, tz)]
            f["fills"] += 1
            f["notional"] += usd
            f["buy"] += usd if sd > 0 else 0.0
            f["rebate"] += usd * rebate / 1e4
            d["money"] += usd * rebate / 1e4
            for h, mid in zip(HORIZONS, m):
                if mid is not None:
                    f[f"mo{h}"] += sd * qty * (mid - px)
                    f[f"usd{h}"] += usd
        snapshot = {s: pos[s] * last[s] for s in symbols if last.get(s) is not None}
        d["long"] += sum(v for v in snapshot.values() if v > 0)
        path[bar] = snapshot
        bar = end
    return b, path


def row(name, who, d, bar_s, posdiff):
    n = d["notional"]
    bp = lambda key, usd: 1e4 * d[key] / d[usd] if d[usd] else float("nan")
    bars = d["bars"] or 1
    return (name, who, f"{d['bars'] * bar_s / 3600:.1f}", f"{d['fills']:.0f}", f"{n:.0f}", f"{100 * d['buy'] / n if n else 0:.1f}", f"{bp('mo0', 'usd0'):.2f}", f"{bp('mo60', 'usd60'):.2f}", f"{bp('mo300', 'usd300'):.2f}", f"{d['mo300'] + d['rebate']:.1f}", f"{d['money']:.1f}", f"{d['long'] / bars:.0f}", posdiff)


def start_positions(positions_csv):
    if positions_csv is None:
        return {}
    with open(positions_csv) as f:
        return {r["symbol"]: float(r["qty"]) for r in csv.DictReader(f)}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("journal", help="the session's first part; later parts are found beside it")
    p.add_argument("run", type=Path, help="the backtest's output directory")
    p.add_argument("--start", help="score from this UTC time (default: the run's first fill)")
    p.add_argument("--buckets", default="all=0-24", help="name=from-to hours of the day, comma separated")
    p.add_argument("--tz", default="UTC")
    p.add_argument("--rebate-bps", type=float, default=0.0, help="maker rebate on every maker fill")
    p.add_argument("--bar-s", type=int, default=60)
    p.add_argument("--positions", help="CSV (symbol,qty) of the positions at --start")
    p.add_argument("--data", default="fastmm-data")
    a = p.parse_args(argv)
    parts = [a.journal]
    head = Path(a.journal)
    k = 1
    while (head.with_suffix(f".{k}.fmj")).exists():
        parts.append(str(head.with_suffix(f".{k}.fmj")))
        k += 1
    live = read_live(parts, a.data)
    run = read_run(a.run)
    (mlive, mrun), series = mark(a.journal, [live, run], a.data, a.bar_s)
    # The run's ts_ns is when it received the fill: for the edge take the mid its venue had at the fill.
    with open(a.run / "fills.csv") as f:
        for m, r in zip(mrun, csv.DictReader(f)):
            if float(r["mid"]) > 0:
                m[0] = float(r["mid"])
    t0 = int(dt.datetime.fromisoformat(a.start.replace("Z", "+00:00")).timestamp() * 1e9) if a.start else min(r[0] for r in run)
    t1 = int(json.loads((a.run / "summary.json").read_text())["end_ts"])
    bar_ns = a.bar_s * 1_000_000_000
    t0 -= t0 % bar_ns
    tz = ZoneInfo(a.tz)
    buckets = parse_buckets(a.buckets)
    start = start_positions(a.positions)
    bl, pl = score(live, mlive, series, t0, t1, bar_ns, buckets, tz, start, a.rebate_bps)
    br, pr = score(run, mrun, series, t0, t1, bar_ns, buckets, tz, start, a.rebate_bps)
    diff = defaultdict(float)
    count = defaultdict(int)
    for bar, snap in pl.items():
        name = bucket_of(bar, buckets, tz)
        other = pr.get(bar, {})
        diff[name] += sum(abs(snap.get(s, 0.0) - other.get(s, 0.0)) for s in set(snap) | set(other))
        count[name] += 1
    w = csv.writer(sys.stdout, delimiter="\t")
    w.writerow(HEADER)
    for name in sorted(set(bl) | set(br)):
        pd_ = f"{diff[name] / max(count[name], 1):.0f}"
        w.writerow(row(name, "live", bl[name], a.bar_s, ""))
        w.writerow(row(name, "run", br[name], a.bar_s, pd_))
    return 0


if __name__ == "__main__":
    sys.exit(main())
