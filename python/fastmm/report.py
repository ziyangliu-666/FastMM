"""A single-file HTML report for one FastMM run.

    fastmm report runs/backtest              # -> runs/backtest/report.html
    python -m fastmm report runs/sim/session.fmj -o session.html

The input is what a run leaves behind: a backtest output directory (``summary.json``,
``equity.csv``, ``fills.csv``, written by ``fastmm-backtest --out`` and
``BacktestResult::write_all``), an in-memory :class:`fastmm.BacktestResult`, or the session
journal of a live run. A session journal has no venue mid at every fill, so the panels that
need one (markouts, spread capture, fill quality) are left out of a session report rather than
filled with substitutes.

The output is one HTML file with the CSS and the charts inlined: no network, no JavaScript, no
fonts to fetch. Charts are SVG (time series) and CSS bars (everything else), so the text in them
is real text that scales, prints and can be selected.
"""

from __future__ import annotations

import csv
import html
import json
import math
import os
import re
import sys
from datetime import datetime, timedelta, timezone
from typing import Any, Dict, List, Optional, Sequence, Tuple, Union

__all__ = ["Run", "read_run_dir", "read_journal", "from_result", "render_html", "write_report"]

PathLike = Union[str, "os.PathLike[str]"]
Rows = List[Tuple[str, str]]

_MAX_POINTS = 600  # time-series points kept after decimation (the wide chart is 760 units)


# --------------------------------------------------------------------------------------------
# the run
# --------------------------------------------------------------------------------------------


class Run:
    """Everything a report draws, in natural units, from whichever source produced it.

    ``metrics`` holds the summary numbers under the names ``summary.json`` uses; a key that is
    missing (a live session has no markouts) makes the panels that need it disappear instead of
    showing a substitute.
    """

    def __init__(self, kind: str, strategy: str, source: str) -> None:
        self.kind = kind  # "backtest" or "session"
        self.strategy = strategy
        self.source = source
        self.quote_asset = ""
        self.base_asset = ""
        self.symbol = ""
        self.metrics: Dict[str, Any] = {}
        self.decomposition: Optional[Dict[str, float]] = None
        self.markouts: List[Dict[str, Any]] = []
        self.equity: Dict[str, List[float]] = {}
        self.fills: Dict[str, List[Any]] = {}
        self.params: Rows = []
        self.settings: Rows = []
        self.config_toml = ""
        self.notes: List[str] = []

    def get(self, key: str) -> Optional[float]:
        v = self.metrics.get(key)
        return None if v is None or (isinstance(v, float) and not math.isfinite(v)) else v


# --------------------------------------------------------------------------------------------
# sources
# --------------------------------------------------------------------------------------------


def read_run_dir(path: PathLike, config: Optional[PathLike] = None) -> Run:
    """Read a backtest output directory (``summary.json`` and the CSVs beside it)."""
    d = str(path)
    summary_path = os.path.join(d, "summary.json")
    if not os.path.exists(summary_path):
        raise FileNotFoundError(f"{summary_path}: not a backtest output directory")
    with open(summary_path, encoding="utf-8") as fh:
        summary = json.load(fh)
    run = Run("backtest", str(summary.get("strategy") or "backtest"), os.path.abspath(d))
    run.metrics = {k: v for k, v in summary.items() if not isinstance(v, (dict, list))}
    run.decomposition = summary.get("pnl_decomposition")
    run.markouts = summary.get("markouts") or []
    run.params = [(k, str(v)) for k, v in (summary.get("params") or {}).items()]
    run.equity = _read_equity_csv(os.path.join(d, "equity.csv"))
    run.fills = _read_fills_csv(os.path.join(d, "fills.csv"))
    _apply_config(run, config if config is not None else _find_config(d))
    run.settings = _backtest_settings(run)
    return run


def from_result(result: Any, source: str = "in-memory result") -> Run:
    """Build a report from a :class:`fastmm.BacktestResult` without writing files first."""
    stats = result.stats()
    run = Run("backtest", str(result.strategy), source)
    run.metrics = dict(stats)
    run.metrics.setdefault("seed", getattr(result, "seed", None))
    run.metrics["outbound_sha256"] = result.outbound_sha256
    # stats() carries the decomposition as flat keys; summary.json nests it under one object.
    run.decomposition = {
        "spread_capture": stats["spread_capture"],
        "spread_capture_bps": stats["spread_capture_bps"],
        "mid_drift": stats["mid_drift"],
        "fees_paid": stats["fees_paid"],
        "rebates_received": stats["rebates_received"],
        "net": stats["decomposition_net"],
        "residual": stats["decomposition_residual"],
        "notional": stats["traded_notional"],
        "capture_notional": stats["capture_notional"],
        "capture_fills": stats["capture_fills"],
        "fills": stats["fills"],
    }
    run.markouts = list(result.markouts())
    run.params = [(k, str(v)) for k, v in dict(result.params).items()]
    scale = 1e-8
    e = result.equity
    run.equity = {
        "ts": [int(t) for t in e["ts"]],
        "equity": [(r + u - f) * scale
                   for r, u, f in zip(e["realized"], e["unrealized"], e["fees"])],
        "position": [p * scale for p in e["position"]],
        "mid": [m * scale for m in e["mid"]],
        "quoted": [int(q) for q in e["quoted"]],
    }
    f = result.fills
    run.fills = {
        "ts": [int(t) for t in f["ts"]],
        "side": ["buy" if s == 0 else "sell" for s in f["side"]],
        "qty": [q * scale for q in f["qty"]],
        "fee": [v * scale for v in f["fee"]],
        "liquidity": ["maker" if v == 1 else ("taker" if v == 2 else "unknown")
                      for v in f["liquidity"]],
    }
    run.settings = _backtest_settings(run)
    return run


def read_journal(path: PathLike, config: Optional[PathLike] = None) -> Run:
    """Read a live session journal (``.fmj``) recorded by ``fastmm-live``."""
    from . import _fmj

    s = _fmj.read_session(str(path))
    header = s["header"]
    run = Run("session", header["strategy"] or "session", os.path.abspath(str(path)))
    run.config_toml = header["config"]
    instruments = s["instruments"]
    if instruments:
        run.symbol = instruments[0]["symbol"]
    _apply_config(run, config, header["config"] or None)
    _session_metrics(run, s)
    if run.metrics.get("fills"):
        run.notes.append(
            "A live session records its fills, not the venue book at each fill, so markouts, "
            "spread capture and fill quality are not in this report; replay the journal through "
            "a backtest for those.")
    return run


def _session_metrics(run: Run, s: Dict[str, Any]) -> None:
    """Position, PnL and the marked equity of a session, from its fills and mark prices."""
    fills = s["fills"]
    marks = s["marks"]
    header = s["header"]
    counts = s["counts"]
    events: List[Tuple[int, int, Optional[Dict[str, Any]], Optional[float]]] = []
    for f in fills:
        events.append((f["ts"], 0, f, None))
    for ts, mid in marks:
        events.append((ts, 1, None, mid))
    events.sort(key=lambda e: (e[0], e[1]))

    position = fees = realized = 0.0
    avg_px = 0.0
    volume_base = volume_quote = 0.0
    maker = taker = 0
    other_fees = 0
    inventory_sum = inventory_abs = inventory_max = 0.0
    ts_col: List[int] = []
    eq_col: List[float] = []
    pos_col: List[float] = []
    mid_col: List[float] = []
    mark = marks[0][1] if marks else None
    fill_ts: List[int] = []
    fill_side: List[str] = []
    fill_qty: List[float] = []
    fill_fee: List[float] = []
    fill_liq: List[str] = []
    for ts, _kind, fill, mid in events:
        if mid is not None:
            mark = mid
        if fill is not None:
            qty = fill["qty"]
            px = fill["price"]
            signed = qty if fill["side"] == "buy" else -qty
            fee = fill["fee"]
            if fill["fee_asset"] == "quote":
                fees += fee
            elif fill["fee_asset"] == "base":
                fees += fee * px
                signed -= fee if fill["side"] == "buy" else -fee
            else:
                other_fees += 1
            # Average cost: a trade that reduces the position books the difference as realized.
            if position == 0 or (position > 0) == (signed > 0):
                total = position + signed
                avg_px = (avg_px * position + px * signed) / total if total else 0.0
            else:
                closed = min(abs(position), abs(signed)) * (1 if position > 0 else -1)
                realized += closed * (px - avg_px)
                if abs(signed) > abs(position):
                    avg_px = px
            position += signed
            volume_base += qty
            volume_quote += qty * px
            maker += fill["liquidity"] == "maker"
            taker += fill["liquidity"] == "taker"
            fill_ts.append(ts)
            fill_side.append(fill["side"])
            fill_qty.append(qty)
            fill_fee.append(fee if fill["fee_asset"] == "quote" else fee * px)
            fill_liq.append(fill["liquidity"])
        if mark is None:
            continue
        unrealized = position * (mark - avg_px)
        ts_col.append(ts)
        eq_col.append(realized + unrealized - fees)
        pos_col.append(position)
        mid_col.append(mark)
        inventory_sum += position
        inventory_abs += abs(position)
        inventory_max = max(inventory_max, abs(position))
    run.equity = {"ts": ts_col, "equity": eq_col, "position": pos_col, "mid": mid_col,
                  "quoted": []}
    run.fills = {"ts": fill_ts, "side": fill_side, "qty": fill_qty, "fee": fill_fee,
                 "liquidity": fill_liq}
    n = max(len(ts_col), 1)
    unrealized = position * (mark - avg_px) if mark is not None else 0.0
    start, end = s["first_ts"], s["last_ts"]
    run.metrics.update({
        "net_pnl": realized + unrealized - fees,
        "realized_pnl": realized,
        "unrealized_pnl": unrealized,
        "fees": fees,
        "final_position": position,
        "fills": len(fills),
        "maker_fills": maker,
        "taker_fills": taker,
        "orders": counts["orders"],
        "cancels": counts["cancels"],
        "replaces": counts["replaces"],
        "rejects": counts["rejects"],
        "cancel_rejects": counts["cancel_rejects"],
        "dropped_outbound": counts["dropped"],
        "volume_base": volume_base,
        "volume_quote": volume_quote,
        "inventory_mean": inventory_sum / n,
        "inventory_abs_mean": inventory_abs / n,
        "inventory_max": inventory_max,
        "md_events": counts["events"],
        "seed": header["rng_seed"],
        "start_ts": start,
        "end_ts": end,
        "duration_s": (end - start) / 1e9 if end > start else 0.0,
        "max_drawdown": _max_drawdown(eq_col),
    })
    if eq_col:
        run.metrics["fill_ratio"] = (len(fills) / counts["orders"]) if counts["orders"] else None
    run.settings = [
        ("session id", str(header["session_id"])),
        ("started", _fmt_ts(start or header["start_ts"])),
        ("duration", _fmt_duration(run.metrics["duration_s"])),
        ("journal format", f"v{header['version']}"),
        ("rng seed", str(header["rng_seed"])),
        ("quoting", "enabled" if header["quoting_enabled"] else "dry run (no orders)"),
        ("mark price", "venue book ticker, else the last trade print"),
    ]
    if s["latency"]:
        run.metrics["latency"] = s["latency"]
    if s["reject_reasons"]:
        run.metrics["reject_reasons"] = s["reject_reasons"]
    if other_fees:
        run.notes.append(
            f"{other_fees} fills paid their fee in a third asset (for example BNB); those fees "
            "are counted nowhere in this report, as the engine does not convert them.")


def _max_drawdown(equity: Sequence[float]) -> float:
    peak = float("-inf")
    worst = 0.0
    for v in equity:
        peak = max(peak, v)
        worst = min(worst, v - peak)
    return -worst


def _read_equity_csv(path: str) -> Dict[str, List[float]]:
    cols: Dict[str, List[float]] = {"ts": [], "equity": [], "position": [], "mid": [],
                                    "quoted": []}
    if not os.path.exists(path):
        return cols
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            cols["ts"].append(int(row["ts_ns"]))
            cols["equity"].append(float(row["equity"]))
            cols["position"].append(float(row["position"]))
            cols["mid"].append(float(row["mid"]))
            cols["quoted"].append(int(row["quoted"]))
    return cols


def _read_fills_csv(path: str) -> Dict[str, List[Any]]:
    cols: Dict[str, List[Any]] = {"ts": [], "side": [], "qty": [], "fee": [], "liquidity": []}
    if not os.path.exists(path):
        return cols
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            cols["ts"].append(int(row["ts_ns"]))
            cols["side"].append("buy" if row["side"] == "B" else "sell")
            cols["qty"].append(float(row["qty"]))
            cols["fee"].append(float(row["fee"]))
            cols["liquidity"].append({"M": "maker", "T": "taker"}.get(row["liquidity"], "unknown"))
    return cols


def _find_config(run_dir: str) -> Optional[str]:
    for name in ("config.toml", "engine.toml", "backtest.toml"):
        p = os.path.join(run_dir, name)
        if os.path.exists(p):
            return p
    candidates = sorted(f for f in os.listdir(run_dir) if f.endswith(".toml"))
    return os.path.join(run_dir, candidates[0]) if candidates else None


_TOML_INSTRUMENT = re.compile(r"\[\[instruments\]\](.*?)(?=^\[|\Z)", re.S | re.M)


def _toml_table(text: str, name: str) -> Rows:
    """The ``key = value`` lines of one table of a TOML file, with the quotes stripped.

    Enough for the report's two needs (``[strategy.params]`` and ``[[instruments]]``): the
    configuration the engine embeds in a journal is one flat table per section.
    """
    m = re.search(rf"^\s*\[{re.escape(name)}\]\s*$(.*?)(?=^\s*\[|\Z)", text, re.S | re.M)
    rows: Rows = []
    for line in (m.group(1).splitlines() if m else ()):
        key, sep, value = line.strip().partition("=")
        if sep and key.strip() and not key.startswith("#"):
            rows.append((key.strip(), value.strip().strip("'\"")))
    return rows


def _apply_config(run: Run, path: Optional[PathLike], text: Optional[str] = None) -> None:
    """Take the instrument, the strategy parameters and the text itself from a configuration."""
    if text is None and path is not None and os.path.exists(str(path)):
        with open(str(path), encoding="utf-8") as fh:
            text = fh.read()
        run.settings.append(("configuration", os.path.abspath(str(path))))
    if not text:
        return
    run.config_toml = text
    m = _TOML_INSTRUMENT.search(text)
    block = m.group(1) if m else ""
    for key, attr in (("symbol", "symbol"), ("base", "base_asset"), ("quote", "quote_asset")):
        found = re.search(rf"""^\s*{key}\s*=\s*['"]([^'"]*)['"]""", block, re.M)
        if found:
            setattr(run, attr, found.group(1))
    if not run.params:
        run.params = _toml_table(text, "strategy.params")


def _backtest_settings(run: Run) -> Rows:
    rows: Rows = list(run.settings)
    start, end = run.metrics.get("start_ts"), run.metrics.get("end_ts")
    if start and end:
        tail = datetime.fromtimestamp(float(end) / 1e9, timezone.utc)
        same_day = tail.date() == datetime.fromtimestamp(float(start) / 1e9, timezone.utc).date()
        rows.append(("window", f"{_fmt_ts(start)} to "
                               f"{tail.strftime('%H:%M:%S' if same_day else '%Y-%m-%d %H:%M:%S')}"))
    rows.append(("simulated duration", _fmt_duration(run.get("duration_s") or 0.0)))
    if run.get("bars"):
        rows.append(("equity bars", f"{int(run.metrics['bars'])}"))
    if run.metrics.get("seed") is not None:
        rows.append(("rng seed", str(int(run.metrics["seed"]))))
    if run.markouts:
        rows.append(("markout horizons", ", ".join(h["label"] for h in run.markouts)))
    if run.get("md_events") is not None:
        rows.append(("market-data events", _fmt_int(run.metrics["md_events"])))
    if run.get("wall_seconds"):
        rows.append(("wall time", f"{run.metrics['wall_seconds']:.2f} s"))
    if run.metrics.get("outbound_sha256"):
        rows.append(("outbound sha256", str(run.metrics["outbound_sha256"])))
    return rows


# --------------------------------------------------------------------------------------------
# formatting
# --------------------------------------------------------------------------------------------


def _esc(v: Any) -> str:
    return html.escape(str(v), quote=True)


def _fmt_num(v: Optional[float], dp: int = 2, plus: bool = False) -> str:
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "&mdash;"
    s = f"{v:+,.{dp}f}" if plus else f"{v:,.{dp}f}"
    return s.replace("-", "−")  # a minus sign, not a hyphen


def _fmt_money(v: Optional[float]) -> str:
    if v is None:
        return "&mdash;"
    return _fmt_num(v, 2 if abs(v) >= 1 else 4)


def _fmt_qty(v: Optional[float]) -> str:
    if v is None:
        return "&mdash;"
    return _fmt_num(v, 5 if abs(v) < 1000 else 2)


def _fmt_int(v: Optional[float]) -> str:
    return "&mdash;" if v is None else f"{int(v):,}"


def _fmt_pct(v: Optional[float], dp: int = 1) -> str:
    return "&mdash;" if v is None else f"{v * 100:.{dp}f}%"


def _fmt_ns(ns: Optional[float]) -> str:
    if ns is None:
        return "&mdash;"
    ns = float(ns)
    if ns >= 1e9:
        return f"{ns / 1e9:,.1f} s"
    if ns >= 1e6:
        return f"{ns / 1e6:,.1f} ms"
    if ns >= 1e3:
        return f"{ns / 1e3:,.0f} µs"
    return f"{ns:,.0f} ns"


def _fmt_ts(ns: Optional[float]) -> str:
    if not ns:
        return "&mdash;"
    return datetime.fromtimestamp(float(ns) / 1e9, timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def _fmt_clock(ns: float, span_ns: float) -> str:
    dt = datetime.fromtimestamp(ns / 1e9, timezone.utc)
    if span_ns > 2 * 86_400e9:
        return dt.strftime("%d %b")
    if span_ns > 3600e9:
        return dt.strftime("%H:%M")
    return dt.strftime("%H:%M:%S")


def _fmt_duration(seconds: float) -> str:
    if seconds <= 0:
        return "&mdash;"
    if seconds < 90:
        return f"{seconds:.0f} s"
    td = timedelta(seconds=round(seconds))
    hours, rest = divmod(int(td.total_seconds()), 3600)
    minutes, secs = divmod(rest, 60)
    if hours:
        return f"{hours} h {minutes:02d} m"
    return f"{minutes} m {secs:02d} s"


# --------------------------------------------------------------------------------------------
# charts
# --------------------------------------------------------------------------------------------


def _decimate(xs: Sequence[float], ys: Sequence[float],
              limit: int = _MAX_POINTS) -> Tuple[List[float], List[float]]:
    """Keep at most ``limit`` points, and the extreme of every bucket: a spike survives."""
    n = len(xs)
    if n <= limit:
        return list(xs), list(ys)
    step = n / (limit / 2)
    out_x: List[float] = []
    out_y: List[float] = []
    i = 0.0
    while int(i) < n:
        lo, hi = int(i), min(int(i + step), n)
        window = range(lo, max(hi, lo + 1))
        a = min(window, key=lambda k: ys[k])
        b = max(window, key=lambda k: ys[k])
        for k in sorted({lo, a, b}):
            out_x.append(xs[k])
            out_y.append(ys[k])
        i += step
    out_x.append(xs[-1])
    out_y.append(ys[-1])
    return out_x, out_y


def _nice_ticks(lo: float, hi: float, count: int = 4) -> List[float]:
    if not math.isfinite(lo) or not math.isfinite(hi) or hi <= lo:
        return [lo]
    raw = (hi - lo) / max(count, 1)
    mag = 10 ** math.floor(math.log10(raw))
    step = next((m * mag for m in (1, 2, 2.5, 5, 10) if m * mag >= raw), 10 * mag)
    first = math.ceil(lo / step) * step
    ticks = []
    v = first
    while v <= hi + step * 1e-9:
        ticks.append(0.0 if abs(v) < step * 1e-9 else v)
        v += step
    return ticks


class _Plot:
    """A linear x/y plot in SVG user units; one instance draws one chart."""

    def __init__(self, uid: str, width: int, height: int, x_lo: float, x_hi: float,
                 y_lo: float, y_hi: float, left: int = 56, right: int = 10) -> None:
        self.uid = uid
        self.w, self.h = width, height
        self.left, self.right, self.top, self.bottom = left, right, 10, 20
        self.x_lo, self.x_hi = x_lo, x_hi if x_hi > x_lo else x_lo + 1
        pad = (y_hi - y_lo) * 0.08 or (abs(y_hi) or 1) * 0.08
        self.y_lo, self.y_hi = y_lo - pad, y_hi + pad

    def x(self, v: float) -> float:
        span = self.w - self.left - self.right
        return self.left + (v - self.x_lo) / (self.x_hi - self.x_lo) * span

    def y(self, v: float) -> float:
        span = self.h - self.top - self.bottom
        return self.top + (self.y_hi - v) / (self.y_hi - self.y_lo) * span

    def grid(self, fmt) -> str:
        out = []
        for t in _nice_ticks(self.y_lo, self.y_hi, 3):
            y = round(self.y(t), 2)
            cls = "zero" if t == 0 else "grid"
            out.append(f'<line class="{cls}" x1="{self.left}" x2="{self.w - self.right}" '
                       f'y1="{y}" y2="{y}"/>')
            out.append(f'<text class="tick-y" x="{self.left - 6}" y="{y + 3.5}">{fmt(t)}</text>')
        return "".join(out)

    def x_axis(self, span_ns: float, ticks: int = 4) -> str:
        out = [f'<line class="axis" x1="{self.left}" x2="{self.w - self.right}" '
               f'y1="{self.h - self.bottom}" y2="{self.h - self.bottom}"/>']
        for i in range(ticks + 1):
            v = self.x_lo + (self.x_hi - self.x_lo) * i / ticks
            anchor = "start" if i == 0 else ("end" if i == ticks else "middle")
            out.append(f'<text class="tick-x" text-anchor="{anchor}" x="{round(self.x(v), 2)}" '
                       f'y="{self.h - 6}">{_fmt_clock(v, span_ns)}</text>')
        return "".join(out)

    def line(self, xs: Sequence[float], ys: Sequence[float], cls: str, step: bool = False) -> str:
        return f'<path class="{cls}" d="{self._d(xs, ys, step)}"/>'

    def area(self, xs: Sequence[float], ys: Sequence[float], base: float, cls: str,
             step: bool = False, clip: str = "") -> str:
        d = (f"{self._d(xs, ys, step)} L{round(self.x(xs[-1]), 2)},{round(self.y(base), 2)} "
             f"L{round(self.x(xs[0]), 2)},{round(self.y(base), 2)} Z")
        clip_attr = f' clip-path="url(#{clip})"' if clip else ""
        return f'<path class="{cls}" d="{d}"{clip_attr}/>'

    def _d(self, xs: Sequence[float], ys: Sequence[float], step: bool) -> str:
        pts = []
        prev_y = None
        for i, (vx, vy) in enumerate(zip(xs, ys)):
            px, py = round(self.x(vx), 2), round(self.y(vy), 2)
            if i == 0:
                pts.append(f"M{px},{py}")
            elif step and prev_y is not None:
                pts.append(f"L{px},{prev_y} L{px},{py}")
            else:
                pts.append(f"L{px},{py}")
            prev_y = py
        return " ".join(pts)

    def clip(self, y_from: float, y_to: float) -> str:
        top, bottom = sorted((self.y(y_from), self.y(y_to)))
        return (f'<clipPath id="{self.uid}"><rect x="{self.left}" y="{round(top, 2)}" '
                f'width="{self.w - self.left - self.right}" '
                f'height="{round(bottom - top, 2)}"/></clipPath>')

    def dot(self, vx: float, vy: float, cls: str) -> str:
        return (f'<circle class="{cls}" cx="{round(self.x(vx), 2)}" '
                f'cy="{round(self.y(vy), 2)}" r="4"/>')

    def label(self, vx: float, vy: float, text: str, anchor: str = "end", dy: float = -9) -> str:
        return (f'<text class="mark-label" text-anchor="{anchor}" x="{round(self.x(vx), 2)}" '
                f'y="{round(self.y(vy) + dy, 2)}">{text}</text>')

    def svg(self, body: str, title: str) -> str:
        return (f'<svg viewBox="0 0 {self.w} {self.h}" role="img" aria-label="{_esc(title)}" '
                f'preserveAspectRatio="xMidYMid meet">{body}</svg>')


def _equity_svg(run: Run, width: int, height: int, uid: str) -> str:
    ts, eq = _decimate(run.equity["ts"], run.equity["equity"])
    lo, hi = min(min(eq), 0.0), max(max(eq), 0.0)
    p = _Plot(uid, width, height, ts[0], ts[-1], lo, hi)
    span = ts[-1] - ts[0]
    body = [p.grid(lambda v: _fmt_num(v, 2 if abs(hi - lo) >= 1 else 4)),
            p.clip(0.0, p.y_lo),
            p.area(ts, eq, 0.0, "area-loss", clip=uid),
            p.line(ts, eq, "line-equity"),
            p.x_axis(span, 4 if width >= 600 else 2)]
    # The trough of the worst drawdown, direct-labelled: the one point the eye should find.
    full_ts, full_eq = run.equity["ts"], run.equity["equity"]
    peak = float("-inf")
    worst = 0.0
    worst_i = -1
    for i, v in enumerate(full_eq):
        peak = max(peak, v)
        if v - peak < worst:
            worst, worst_i = v - peak, i
    if worst < 0 and 0 < worst_i < len(full_eq) - 1:
        body.append(p.dot(full_ts[worst_i], full_eq[worst_i], "dot-mark"))
        body.append(p.label(full_ts[worst_i], full_eq[worst_i],
                            f"drawdown {_fmt_num(worst, 2)}", "middle", 17))
    body.append(p.dot(ts[-1], eq[-1], "dot-end"))
    body.append(p.label(ts[-1], eq[-1], _fmt_num(eq[-1], 2), "end", -10))
    return p.svg("".join(body), "equity over the run")


def _position_svg(run: Run, width: int, height: int, uid: str, limit: Optional[float]) -> str:
    ts, pos = _decimate(run.equity["ts"], run.equity["position"])
    lo, hi = min(min(pos), 0.0), max(max(pos), 0.0)
    if limit:
        lo, hi = min(lo, -limit), max(hi, limit)
    p = _Plot(uid, width, height, ts[0], ts[-1], lo, hi)
    dp = 5 if max(abs(lo), abs(hi)) < 10 else 2
    body = [p.grid(lambda v: _fmt_num(v, dp)),
            f'<clipPath id="{uid}-long"><rect x="{p.left}" y="{round(p.top, 2)}" '
            f'width="{p.w - p.left - p.right}" height="{round(p.y(0) - p.top, 2)}"/></clipPath>',
            f'<clipPath id="{uid}-short"><rect x="{p.left}" y="{round(p.y(0), 2)}" '
            f'width="{p.w - p.left - p.right}" '
            f'height="{round(p.h - p.bottom - p.y(0), 2)}"/></clipPath>',
            p.area(ts, pos, 0.0, "area-long", step=True, clip=f"{uid}-long"),
            p.area(ts, pos, 0.0, "area-short", step=True, clip=f"{uid}-short"),
            p.line(ts, pos, "line-position", step=True)]
    if limit:
        for side in (limit, -limit):
            y = round(p.y(side), 2)
            body.append(f'<line class="limit" x1="{p.left}" x2="{p.w - p.right}" y1="{y}" '
                        f'y2="{y}"/>')
        body.append(f'<text class="mark-label" text-anchor="start" x="{p.left + 4}" '
                    f'y="{round(p.y(limit) - 5, 2)}">inventory limit '
                    f'{_fmt_num(limit, dp)}</text>')
    body.append(p.x_axis(ts[-1] - ts[0], 4 if width >= 600 else 2))
    return p.svg("".join(body), "position over the run")


def _chart(run: Run, kind: str, limit: Optional[float] = None) -> str:
    """The wide and the narrow rendering of one chart; CSS shows the one that fits."""
    out = []
    for cls, width, height in (("wide", 760, 190), ("narrow", 360, 200)):
        uid = f"{kind}-{cls}"
        svg = (_equity_svg(run, width, height, uid) if kind == "equity"
               else _position_svg(run, width, height, uid, limit))
        out.append(f'<div class="chart only-{cls}">{svg}</div>')
    return "".join(out)


def _bar_rows(rows: Sequence[Tuple[str, float, str]], signed: bool = False,
              tones: Optional[Sequence[str]] = None, total_last: bool = False) -> str:
    """A label / bar / value table. ``rows`` is (label, value, formatted value).

    ``signed`` puts zero in the middle and grows the bars either way; the last row can be a
    total, which is a sum rather than a contribution and so carries no direction colour.
    """
    peak = max((abs(v) for _, v, _ in rows), default=0.0) or 1.0
    out = ['<div class="bars">']
    for i, (label, value, text) in enumerate(rows):
        width = abs(value) / peak * (50 if signed else 100)
        left = 50 - width if (signed and value < 0) else (50 if signed else 0)
        total = total_last and i == len(rows) - 1
        if total:
            tone = "bar-total"
        elif tones is not None:
            tone = tones[i % len(tones)]
        else:
            tone = "bar-neg" if (signed and value < 0) else "bar-pos"
        zero = '<span class="bar-zero"></span>' if signed else ""
        out.append(
            f'<div class="bar-row{" total" if total else ""}">'
            f'<span class="bar-label">{_esc(label)}</span>'
            f'<span class="bar-track">{zero}'
            f'<span class="bar {tone}" style="left:{left:.4g}%;width:{width:.4g}%"></span></span>'
            f'<span class="bar-value">{text}</span></div>')
    out.append("</div>")
    return "".join(out)


def _stacked_bar(parts: Sequence[Tuple[str, float, str]]) -> str:
    """One 100 % bar: (label, share 0..1, tone class), with the shares labelled underneath."""
    segments = []
    legend = []
    for label, share, tone in parts:
        segments.append(f'<span class="seg {tone}" style="width:{share * 100:.4g}%"></span>')
        legend.append(f'<span class="key"><span class="swatch {tone}"></span>{_esc(label)} '
                      f'<b>{_fmt_pct(share)}</b></span>')
    return (f'<div class="stack">{"".join(segments)}</div>'
            f'<div class="legend">{"".join(legend)}</div>')


def _columns(values: Sequence[float], labels: Sequence[str], caption: str) -> str:
    """A column chart in CSS: one column per bucket, tallest normalised to the row height."""
    peak = max(values, default=0.0) or 1.0
    cols = "".join(
        f'<span class="col" style="height:{v / peak * 100:.4g}%" title="{_esc(labels[i])}"></span>'
        for i, v in enumerate(values))
    return (f'<div class="cols">{cols}</div>'
            f'<div class="cols-foot"><span>{_esc(labels[0])}</span>'
            f'<span class="muted">{_esc(caption)}</span>'
            f'<span>{_esc(labels[-1])}</span></div>')


# --------------------------------------------------------------------------------------------
# page
# --------------------------------------------------------------------------------------------


_CSS = """
:root{color-scheme:light dark;
--plane:#f4f4f1;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;
--rule:rgba(11,11,11,.11);--grid:#e1e0d9;--axis:#c3c2b7;
--s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;--neg:#d03b3b;--wash:rgba(42,120,214,.10);
--negwash:rgba(208,59,59,.10)}
@media (prefers-color-scheme:dark){:root{
--plane:#0d0d0d;--surface:#1a1a19;--ink:#fff;--ink2:#c3c2b7;--muted:#898781;
--rule:rgba(255,255,255,.12);--grid:#2c2c2a;--axis:#383835;
--s1:#3987e5;--s2:#d95926;--s3:#199e70;--neg:#e66767;--wash:rgba(57,135,229,.14);
--negwash:rgba(230,103,103,.14)}}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--plane);color:var(--ink);
font:15px/1.55 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
font-variant-numeric:tabular-nums}
main{max-width:64rem;margin:0 auto;padding:2.5rem 1.25rem 4rem}
h1{font-size:1.75rem;font-weight:600;letter-spacing:-.015em;margin:.1rem 0 .35rem}
h2{font-size:1rem;font-weight:600;letter-spacing:-.005em;margin:0 0 .15rem}
p{margin:0}
a{color:inherit}
.eyebrow{font-size:.7rem;letter-spacing:.09em;text-transform:uppercase;color:var(--muted)}
.sub{color:var(--ink2);font-size:.86rem}
.muted{color:var(--muted)}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.8rem;
word-break:break-all}
header.head{display:flex;flex-wrap:wrap;gap:1.5rem;align-items:flex-end;
justify-content:space-between;padding-bottom:1.25rem;border-bottom:1px solid var(--rule)}
.hero{text-align:right}
.hero .value{font-size:2.75rem;line-height:1;font-weight:600;letter-spacing:-.02em;
font-variant-numeric:proportional-nums}
.hero .label{font-size:.78rem;color:var(--ink2);margin-top:.35rem}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(9.5rem,1fr));
background:var(--surface);border:1px solid var(--rule);border-radius:8px;overflow:hidden;
margin:1.25rem 0}
.tile{padding:.8rem .9rem;border-left:1px solid var(--rule);border-top:1px solid var(--rule);
margin:-1px 0 0 -1px}
.tile .k{font-size:.68rem;letter-spacing:.07em;text-transform:uppercase;color:var(--muted)}
.tile .v{font-size:1.3rem;font-weight:600;margin-top:.15rem;
font-variant-numeric:proportional-nums}
.tile .n{font-size:.75rem;color:var(--ink2);margin-top:.1rem}
section{background:var(--surface);border:1px solid var(--rule);border-radius:8px;
padding:1.15rem 1.15rem 1.25rem;margin:1rem 0}
section .lede{color:var(--ink2);font-size:.83rem;margin-bottom:.9rem;max-width:46rem}
.grid2{display:grid;grid-template-columns:repeat(auto-fit,minmax(17rem,1fr));gap:1.4rem}
.chart svg{display:block;width:100%;height:auto}
.chart{margin:.2rem 0 .1rem}
.only-narrow{display:none}
.chart-title{font-size:.72rem;letter-spacing:.07em;text-transform:uppercase;color:var(--muted);
margin:.9rem 0 .1rem}
svg .grid{stroke:var(--grid);stroke-width:1}
svg .zero,svg .axis{stroke:var(--axis);stroke-width:1}
svg .limit{stroke:var(--muted);stroke-width:1;stroke-dasharray:3 3}
svg text{fill:var(--muted);font:11px system-ui,-apple-system,sans-serif;
font-variant-numeric:tabular-nums}
svg .mark-label{fill:var(--ink2);font-size:11px}
svg .tick-y{text-anchor:end}
svg .line-equity{fill:none;stroke:var(--s1);stroke-width:2;stroke-linejoin:round;
stroke-linecap:round}
svg .line-position{fill:none;stroke:var(--s1);stroke-width:1.5;stroke-linejoin:round}
svg .area-loss{fill:var(--negwash);stroke:none}
svg .area-long{fill:var(--wash);stroke:none}
svg .area-short{fill:var(--negwash);stroke:none}
svg .dot-end{fill:var(--s1);stroke:var(--surface);stroke-width:2}
svg .dot-mark{fill:var(--neg);stroke:var(--surface);stroke-width:2}
table{width:100%;border-collapse:collapse;font-size:.83rem}
caption{text-align:left;font-size:.72rem;letter-spacing:.07em;text-transform:uppercase;
color:var(--muted);padding-bottom:.4rem}
th,td{text-align:right;padding:.36rem .5rem;border-bottom:1px solid var(--rule);
white-space:nowrap}
th:first-child,td:first-child{text-align:left;white-space:normal}
table.kv td{white-space:normal;overflow-wrap:anywhere}
table.kv td:first-child{color:var(--ink2);width:40%}
thead th{font-weight:500;color:var(--muted);font-size:.72rem;letter-spacing:.04em}
tbody tr:last-child td{border-bottom:none}
tr.total td{font-weight:600;border-top:1px solid var(--axis)}
.scroll{overflow-x:auto}
.bars{margin:.2rem 0 .4rem}
.bar-row{display:grid;grid-template-columns:minmax(6rem,11rem) 1fr minmax(5rem,auto);
align-items:center;gap:.7rem;padding:.22rem 0;font-size:.83rem}
.bar-label{color:var(--ink2)}
.bar-track{position:relative;height:14px}
.bar{position:absolute;top:0;height:14px;border-radius:0 3px 3px 0;min-width:1px}
.bar-neg{border-radius:3px 0 0 3px}
.bar-pos{background:var(--s1)}
.bar-neg{background:var(--neg)}
.bar-total{background:var(--ink2)}
.bar-row.total{border-top:1px solid var(--rule);margin-top:.25rem;padding-top:.35rem;
font-weight:600}
.bar-zero{position:absolute;left:50%;top:-2px;bottom:-2px;width:1px;background:var(--axis)}
.bar-value{text-align:right;font-variant-numeric:tabular-nums}
.stack{display:flex;height:16px;border-radius:3px;overflow:hidden;gap:2px;margin:.3rem 0 .5rem}
.seg{display:block;height:100%}
.seg:first-child{border-radius:3px 0 0 3px}
.seg:last-child{border-radius:0 3px 3px 0}
.tone1{background:var(--s1)}
.tone2{background:var(--s2)}
.tone3{background:var(--s3)}
.legend{display:flex;flex-wrap:wrap;gap:.25rem 1.1rem;font-size:.78rem;color:var(--ink2)}
.key{display:inline-flex;align-items:center;gap:.4rem}
.swatch{width:9px;height:9px;border-radius:2px;display:inline-block}
.cols{display:flex;align-items:flex-end;gap:1px;height:64px;margin-top:.4rem}
.col{flex:1;background:var(--s1);border-radius:2px 2px 0 0;opacity:.85}
.cols.strip{height:12px;gap:0}
.cols.strip .col{border-radius:0}
.cols-foot{display:flex;justify-content:space-between;font-size:.72rem;color:var(--muted);
padding-top:.3rem;border-top:1px solid var(--rule)}
.legend+.scroll{margin-top:.9rem}
details{margin-top:.9rem;font-size:.83rem}
summary{cursor:pointer;color:var(--ink2)}
pre{background:var(--plane);border:1px solid var(--rule);border-radius:6px;padding:.8rem;
overflow-x:auto;font:.76rem/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.note{font-size:.8rem;color:var(--ink2);margin-top:.8rem;padding-left:.7rem;
border-left:2px solid var(--axis)}
footer{margin-top:2rem;padding-top:1rem;border-top:1px solid var(--rule);font-size:.78rem;
color:var(--muted);display:flex;flex-wrap:wrap;gap:.4rem 1.5rem;justify-content:space-between}
@media (max-width:640px){
main{padding:1.5rem .9rem 3rem}
.only-wide{display:none}
.only-narrow{display:block}
header.head{align-items:flex-start}
.hero{text-align:left}
.hero .value{font-size:2.2rem}
.bar-row{grid-template-columns:minmax(5rem,7rem) 1fr auto;gap:.5rem;font-size:.78rem}
}
@media print{
:root{--plane:#fff;--surface:#fff;--ink:#000;--ink2:#333;--rule:rgba(0,0,0,.2)}
body{background:#fff}
main{max-width:none;padding:0}
section{break-inside:avoid;page-break-inside:avoid;border:none;border-top:1px solid var(--rule);
border-radius:0;padding-left:0;padding-right:0}
.tiles{border:none}
details{display:none}
}
"""


def _tile(label: str, value: str, note: str = "") -> str:
    note_html = f'<div class="n">{note}</div>' if note else ""
    return (f'<div class="tile"><div class="k">{_esc(label)}</div>'
            f'<div class="v">{value}</div>{note_html}</div>')


def _table(headers: Sequence[str], rows: Sequence[Sequence[str]], caption: str = "",
           total_last: bool = False, cls: str = "") -> str:
    head = "".join(f"<th>{_esc(h)}</th>" for h in headers)
    body = []
    for i, row in enumerate(rows):
        tr = ' class="total"' if total_last and i == len(rows) - 1 else ""
        body.append(f"<tr{tr}>" + "".join(f"<td>{c}</td>" for c in row) + "</tr>")
    cap = f"<caption>{_esc(caption)}</caption>" if caption else ""
    head_html = f"<thead><tr>{head}</tr></thead>" if any(headers) else ""
    return (f'<div class="scroll"><table class="{cls}">{cap}{head_html}'
            f'<tbody>{"".join(body)}</tbody></table></div>')


def _kv_table(rows: Rows, caption: str = "") -> str:
    return _table(["", ""], [(_esc(k), _esc(v)) for k, v in rows], caption, cls="kv")


def _head(run: Run, generated: datetime) -> str:
    net = run.get("net_pnl")
    quote = run.quote_asset or "quote currency"
    facts = [run.kind, run.symbol or "", _fmt_duration(run.get("duration_s") or 0.0)]
    start = run.metrics.get("start_ts")
    if start:
        facts.append(_fmt_ts(start))
    sub = " · ".join(f for f in facts if f and f != "&mdash;")
    return (f'<header class="head"><div>'
            f'<p class="eyebrow">FastMM run report</p>'
            f'<h1>{_esc(run.strategy)}</h1>'
            f'<p class="sub">{_esc(sub)}</p></div>'
            f'<div class="hero"><div class="value">{_fmt_money(net)}</div>'
            f'<div class="label">net PnL, {_esc(quote)}</div></div></header>')


def _tiles(run: Run) -> str:
    tiles = [
        _tile("fills", _fmt_int(run.get("fills")),
              f"{_fmt_int(run.get('maker_fills'))} maker / "
              f"{_fmt_int(run.get('taker_fills'))} taker"),
        _tile("traded notional", _fmt_num(run.get("volume_quote"), 0),
              f"{_fmt_qty(run.get('volume_base'))} {_esc(run.base_asset or 'base')}"),
        _tile("fees paid", _fmt_money(run.get("fees")), "already in the net PnL"),
    ]
    if run.get("quote_uptime") is not None:
        tiles.append(_tile("quote uptime", _fmt_pct(run.get("quote_uptime")),
                           "both sides resting"))
    tiles.append(_tile("max drawdown", _fmt_money(run.get("max_drawdown")),
                       _fmt_pct(run.get("max_drawdown_pct"), 2) + " of capital"
                       if run.get("max_drawdown_pct") is not None else "peak to trough"))
    if run.get("sharpe_bar") is not None:
        tiles.append(_tile("sharpe (per bar)", _fmt_num(run.get("sharpe_bar"), 3),
                           _fmt_num(run.get("sharpe_annualized"), 2) + " annualized"
                           if run.get("sharpe_annualized") is not None else "run under a day"))
    tiles.append(_tile("final position", _fmt_qty(run.get("final_position")),
                       f"mean |inventory| {_fmt_qty(run.get('inventory_abs_mean'))}"))
    return f'<div class="tiles">{"".join(tiles[:6])}</div>'


def _equity_section(run: Run) -> str:
    if len(run.equity.get("ts") or ()) < 2:
        return ""
    limit = None
    for key, value in run.params:
        if key in ("max_inventory", "max_position"):
            try:
                limit = abs(float(value))
            except ValueError:
                limit = None
    marked = ("marked at the venue mid" if run.kind == "backtest"
              else "marked at the last price the venue published")
    quoted = _quoted_strip(run)
    return (
        "<section>"
        "<h2>Equity and inventory</h2>"
        f'<p class="lede">Equity is realized plus unrealized PnL minus fees, {marked}. '
        "Inventory is the net position; the two charts share one time axis.</p>"
        f'<p class="chart-title">equity, {_esc(run.quote_asset or "quote currency")}</p>'
        f'{_chart(run, "equity")}'
        f'<p class="chart-title">position, {_esc(run.base_asset or "base currency")}</p>'
        f'{_chart(run, "position", limit)}'
        f"{quoted}"
        "</section>")


def _quoted_strip(run: Run) -> str:
    quoted = run.equity.get("quoted") or []
    if not quoted:
        return ""
    buckets = min(120, len(quoted))
    size = len(quoted) / buckets
    cells = []
    for i in range(buckets):
        window = quoted[int(i * size):max(int((i + 1) * size), int(i * size) + 1)]
        share = sum(1 for q in window if q == 3) / len(window)
        cells.append(f'<span class="col" style="height:100%;'
                     f'opacity:{0.12 + share * 0.78:.2f}"></span>')
    return ('<p class="chart-title">both sides quoted</p>'
            f'<div class="cols strip">{"".join(cells)}</div>'
            '<div class="cols-foot"><span class="muted">darker is more of the bar spent with a '
            'bid and an ask resting</span></div>')


def _pnl_section(run: Run) -> str:
    d = run.decomposition
    quote = run.quote_asset or "quote"
    if not d:
        rows = [("realized", run.get("realized_pnl")), ("unrealized", run.get("unrealized_pnl")),
                ("fees", -(run.get("fees") or 0.0)), ("net", run.get("net_pnl"))]
        bars = _bar_rows([(k, v or 0.0, _fmt_money(v)) for k, v in rows], signed=True,
                         total_last=True)
        return ("<section><h2>Where the PnL came from</h2>"
                f'<p class="lede">Realized and unrealized PnL and the fees paid, in {_esc(quote)}. '
                "A session journal carries no venue mid at each fill, so the report cannot split "
                "the spread captured from the drift that followed.</p>"
                f"{bars}</section>")
    parts = [
        ("gross spread capture", d.get("spread_capture", 0.0)),
        ("mid drift after the fills", d.get("mid_drift", 0.0)),
        ("fees paid", -abs(d.get("fees_paid", 0.0))),
        ("rebates received", d.get("rebates_received", 0.0)),
        ("net", d.get("net", 0.0)),
    ]
    bars = _bar_rows([(k, v, _fmt_money(v)) for k, v in parts], signed=True, total_last=True)
    notional = d.get("notional", 0.0)
    capture_fills = int(d.get("capture_fills", 0))
    fills = int(d.get("fills", 0))
    where = ("the mid at every fill" if capture_fills == fills
             else f"the {capture_fills} of {fills} fills that had a venue mid")
    residual = d.get("residual", 0.0)
    rows = [
        ("gross spread capture", _fmt_money(d.get("spread_capture")),
         _fmt_num(d.get("spread_capture_bps"), 3, plus=True)),
        ("mid drift after the fills", _fmt_money(d.get("mid_drift")), "&mdash;"),
        ("fees paid", _fmt_money(-abs(d.get("fees_paid", 0.0))), "&mdash;"),
        ("rebates received", _fmt_money(d.get("rebates_received")), "&mdash;"),
        ("net", _fmt_money(d.get("net")), "&mdash;"),
    ]
    unexplained = ("" if abs(residual) < 5e-5 else
                   f'<p class="note">Unexplained: {_fmt_money(residual)} &mdash; fixed-point '
                   "rounding, or something this split does not model.</p>")
    return (
        "<section><h2>Where the PnL came from</h2>"
        f'<p class="lede">The net PnL split into what the quotes earned against the mid, what the '
        f'mid did afterwards, and what the venue charged. {_fmt_num(notional, 2)} {_esc(quote)} '
        f"traded; the capture is measured against {where}.</p>"
        f"{bars}"
        f'{_table(["", quote, "bps of notional"], rows, "the same numbers", total_last=True)}'
        f"{unexplained}</section>")


def _markout_section(run: Run) -> str:
    if not run.markouts:
        return ""
    rows = []
    bars = []
    tones = []
    for h in run.markouts:
        total = h["total"]
        if not total["fills"]:
            continue
        bars.append((f"{h['label']} capture", total["capture_bps"],
                     _fmt_num(total["capture_bps"], 3, plus=True)))
        bars.append((f"{h['label']} markout", total["markout_bps"],
                     _fmt_num(total["markout_bps"], 3, plus=True)))
        tones += ["tone2", "tone1"]
        rows.append((
            _esc(h["label"]),
            _fmt_num(total["markout_bps"], 3, plus=True),
            _fmt_num(total["capture_bps"], 3, plus=True),
            _fmt_num(total["adverse_selection_bps"], 3, plus=True),
            _fmt_num(h["buy"]["markout_bps"], 3, plus=True),
            _fmt_num(h["sell"]["markout_bps"], 3, plus=True),
            _fmt_int(total["fills"]),
            _fmt_int(h["excluded_fills"]),
        ))
    if not rows:
        return ""
    table = _table(
        ["horizon", "markout", "capture", "adverse selection", "buys", "sells", "fills",
         "excluded"], rows,
        "bps of notional, per fill")
    return (
        "<section><h2>Markouts</h2>"
        '<p class="lede">The venue mid a while after each fill, against the price that was '
        "paid. A capture that stays positive while the markout turns negative is adverse "
        "selection, not edge. Fills whose horizon runs past the end of the data are excluded, "
        "never marked at a substitute price.</p>"
        f'{_bar_rows(bars, signed=True, tones=tones)}'
        '<div class="legend"><span class="key"><span class="swatch tone2"></span>spread captured '
        'at the fill</span><span class="key"><span class="swatch tone1"></span>markout at the '
        "horizon</span></div>"
        f"{table}</section>")


def _fill_quality_section(run: Run) -> str:
    if run.get("time_to_fill_p50_ns") is None and run.get("at_touch_share") is None:
        return ""
    at_touch = run.get("at_touch_share") or 0.0
    behind = run.get("behind_touch_share") or 0.0
    through = run.get("through_touch_share") or 0.0
    left = [
        '<p class="chart-title">where the quote was when it filled</p>',
        _stacked_bar([("at the touch", at_touch, "tone1"), ("behind it", behind, "tone2"),
                      ("through it", through, "tone3")]),
        '<p class="chart-title">time from quote to fill</p>',
        _bar_rows([("p50", float(run.get("time_to_fill_p50_ns") or 0),
                    _fmt_ns(run.get("time_to_fill_p50_ns"))),
                   ("p90", float(run.get("time_to_fill_p90_ns") or 0),
                    _fmt_ns(run.get("time_to_fill_p90_ns"))),
                   ("p99", float(run.get("time_to_fill_p99_ns") or 0),
                    _fmt_ns(run.get("time_to_fill_p99_ns")))]),
    ]
    rows: List[Tuple[str, str]] = [
        ("realized spread", f"{_fmt_num(run.get('realized_spread_bps'), 3)} bps of notional"),
        ("quotes placed", _fmt_int(run.get("quotes_placed"))),
        ("quotes filled", f"{_fmt_int(run.get('quotes_filled'))} "
                          f"({_fmt_pct(run.get('fill_rate_per_quote'))})"),
        ("fill ratio", _fmt_num(run.get("fill_ratio"), 3)),
        ("spread captured", f"{_fmt_num(run.get('spread_captured_bps'), 3)} bps"),
    ]
    if run.get("queue_ahead_p50") is not None:
        rows.append(("queue ahead at fill p50 / p90",
                     f"{_fmt_qty(run.get('queue_ahead_p50'))} / "
                     f"{_fmt_qty(run.get('queue_ahead_p90'))}"))
    else:
        rows.append(("queue ahead at fill", "not tracked by this fill model"))
    return (
        "<section><h2>Fill quality</h2>"
        '<p class="lede">Whether the fills came from resting at the touch or from the market '
        "coming through the quote, and how long a quote waited for them.</p>"
        f'<div class="grid2"><div>{"".join(left)}</div><div>{_kv_table(rows)}</div></div>'
        "</section>")


def _activity_section(run: Run) -> str:
    rows = [
        ("orders sent", _fmt_int(run.get("orders"))),
        ("cancels", _fmt_int(run.get("cancels"))),
        ("replaces", _fmt_int(run.get("replaces"))),
        ("rejects", _fmt_int(run.get("rejects"))),
        ("fills", f"{_fmt_int(run.get('fills'))} "
                  f"({_fmt_int(run.get('maker_fills'))} maker, "
                  f"{_fmt_int(run.get('taker_fills'))} taker)"),
    ]
    if run.get("cancel_rejects"):
        rows.insert(4, ("cancel rejects", _fmt_int(run.get("cancel_rejects"))))
    if run.get("dropped_outbound"):
        rows.append(("outbound messages dropped", _fmt_int(run.get("dropped_outbound"))))
    if run.get("outbound_messages") is not None:
        rows.append(("outbound messages", _fmt_int(run.get("outbound_messages"))))
    if run.get("md_events") is not None:
        rows.append(("market-data events", _fmt_int(run.get("md_events"))))
    if run.get("virtual_tick_to_order_p50_ns") is not None:
        rows.append(("tick to order, simulated p50 / p99",
                     f"{_fmt_ns(run.get('virtual_tick_to_order_p50_ns'))} / "
                     f"{_fmt_ns(run.get('virtual_tick_to_order_p99_ns'))}"))
    latency = run.metrics.get("latency")
    if latency and 5 in latency:
        count, p50, p99 = latency[5]
        rows.append(("tick to trade p50 / p99", f"{_fmt_ns(p50)} / {_fmt_ns(p99)}"))
    hist = _fill_histogram(run)
    return ("<section><h2>Quotes, rejects and fills</h2>"
            '<p class="lede">What the engine sent, what the venue refused and when the fills '
            "arrived.</p>"
            f'<div class="grid2"><div>{_kv_table(rows)}</div><div>{hist}</div></div></section>')


def _fill_histogram(run: Run) -> str:
    ts = run.fills.get("ts") or []
    if len(ts) < 4:
        return ""
    start, end = ts[0], ts[-1]
    if end <= start:
        return ""
    buckets = 48
    counts = [0.0] * buckets
    for t in ts:
        idx = min(int((t - start) / (end - start) * buckets), buckets - 1)
        counts[idx] += 1
    labels = [_fmt_clock(start + (end - start) * i / buckets, end - start) for i in range(buckets)]
    return ('<p class="chart-title">fills over the run</p>'
            + _columns(counts, labels, f"{_fmt_int(len(ts))} fills"))


def _config_section(run: Run) -> str:
    params = _kv_table(run.params, "strategy parameters") if run.params else ""
    settings = _kv_table(run.settings, "run") if run.settings else ""
    toml = ""
    if run.config_toml:
        toml = ('<details><summary>the effective configuration</summary>'
                f"<pre>{_esc(run.config_toml.strip())}</pre></details>")
    if not (params or settings or toml):
        return ""
    return ("<section><h2>Configuration</h2>"
            '<p class="lede">What produced the numbers above.</p>'
            f'<div class="grid2"><div>{params}</div><div>{settings}</div></div>{toml}</section>')


def render_html(run: Run, generated: Optional[datetime] = None) -> str:
    """The whole report as one self-contained HTML document."""
    generated = generated or datetime.now(timezone.utc)
    notes = "".join(f'<p class="note">{_esc(n)}</p>' for n in run.notes)
    sections = "".join(s for s in (
        _equity_section(run),
        _pnl_section(run),
        _markout_section(run),
        _fill_quality_section(run),
        _activity_section(run),
        _config_section(run),
    ) if s)
    title = f"FastMM {run.kind} report — {run.strategy}"
    return (
        "<!doctype html>\n"
        '<html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<link rel="icon" href="data:,">'  # self-contained: not even a favicon request
        f"<title>{_esc(title)}</title>"
        f"<style>{_CSS}</style></head><body><main>"
        f"{_head(run, generated)}"
        f"{_tiles(run)}"
        f"{notes}"
        f"{sections}"
        f'<footer><span>{_esc(run.source)}</span>'
        f'<span>generated {generated.strftime("%Y-%m-%d %H:%M:%S UTC")}</span></footer>'
        "</main></body></html>\n")


def write_report(source: Any, out: Optional[PathLike] = None,
                 config: Optional[PathLike] = None) -> str:
    """Write the report for a run directory, a journal or a result, and return the path.

    ``source`` is a path to a backtest output directory or a ``.fmj`` journal, or a
    :class:`fastmm.BacktestResult`. ``out`` defaults to ``report.html`` beside the input.
    """
    if hasattr(source, "stats") and hasattr(source, "equity"):
        run = from_result(source)
        if out is None:
            raise ValueError("write_report(result) needs an output path")
    else:
        path = str(source)
        if os.path.isdir(path):
            run = read_run_dir(path, config)
            out = out or os.path.join(path, "report.html")
        else:
            run = read_journal(path, config)
            out = out or os.path.join(os.path.dirname(os.path.abspath(path)), "report.html")
    text = render_html(run)
    with open(str(out), "w", encoding="utf-8") as fh:
        fh.write(text)
    return os.path.abspath(str(out))


COMMAND_HELP = "write the HTML report of a run"
COMMAND_DESCRIPTION = (
    "Write a self-contained HTML report -- equity, inventory, PnL decomposition, markouts, fill "
    "quality, counts and the configuration -- for a backtest output directory or a live session "
    "journal, and print its path.")


def add_arguments(parser):
    """The arguments of `fastmm report`, shared by the subcommand and tools/report.py."""
    parser.add_argument("run", metavar="run-dir|journal.fmj",
                        help="a directory written by fastmm-backtest --out, or a .fmj journal")
    parser.add_argument("-o", "--out", metavar="file.html",
                        help="where to write it (default: report.html beside the input)")
    parser.add_argument("--config", metavar="file.toml",
                        help="the run's configuration, for the instrument and the config table")
    return parser


def run_command(args) -> int:
    try:
        print(write_report(args.run, args.out, args.config))
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print(f"fastmm report: {e}", file=sys.stderr)
        return 1
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    """``python3 tools/report.py <run-dir | journal.fmj> [-o out.html] [--config file.toml]``."""
    import argparse

    parser = argparse.ArgumentParser(prog="tools/report.py", description=COMMAND_DESCRIPTION)
    return run_command(add_arguments(parser).parse_args(argv))
