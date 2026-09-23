"""pandas views of backtest results.

The native result columns are raw int64 fixed point with a 1e-8 scale (prices, quantities,
fees, PnL) and int64 nanosecond timestamps. The helpers here convert them to floats and
``datetime64[ns]``. pandas is imported lazily so the core package only needs numpy.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Dict, Iterable, Sequence, Tuple

import numpy as np

if TYPE_CHECKING:  # pragma: no cover
    import pandas as pd

    from ._core import BacktestResult

FIXED_SCALE = 1e-8
"""Multiply a raw fixed-point value by this to get natural units."""

SIDE_NAMES = {0: "buy", 1: "sell", -1: ""}
LIQUIDITY_NAMES = {0: "unknown", 1: "maker", 2: "taker"}
ORDER_KIND_NAMES = {0: "new", 1: "cancel", 2: "replace"}
ORDER_TYPE_NAMES = {0: "limit", 1: "market", 2: "post_only"}

# Stats columns sweep_frame() adds next to the grid parameters.
SWEEP_STATS: Tuple[str, ...] = (
    "net_pnl",
    "realized_pnl",
    "fees",
    "sharpe_bar",  # per bar; sharpe_annualized is NaN for runs shorter than a day
    "max_drawdown",
    "fills",
    "fill_ratio",
    "spread_captured_bps",
    "realized_spread_bps",
    "inventory_abs_mean",
    "quote_uptime",
)

MARKOUT_PREFIX = "markout_mid_"
"""Fill columns holding the venue mid at ts + horizon: ``markout_mid_<ns>ns``."""


def _pandas():
    try:
        import pandas as pd
    except ImportError as exc:  # pragma: no cover - exercised only without pandas
        raise ImportError(
            "fastmm.to_pandas() needs pandas; install it with `pip install 'pandas>=2.0'`"
        ) from exc
    return pd


def _fixed(raw: np.ndarray) -> np.ndarray:
    return raw.astype(np.float64) * FIXED_SCALE


def _time(ns: np.ndarray) -> np.ndarray:
    return ns.astype("datetime64[ns]")


def _time_or_nat(ns: np.ndarray) -> np.ndarray:
    """Like _time, with 0 (unknown / dropped) mapped to NaT."""
    return np.where(ns == 0, np.datetime64("NaT", "ns"), _time(ns))


def _categorical(pd, codes: np.ndarray, names: Dict[int, str]):
    return pd.Categorical.from_codes(
        np.searchsorted(sorted(names), codes), categories=[names[k] for k in sorted(names)]
    )


def fills_frame(result: BacktestResult) -> pd.DataFrame:
    """Fills as a DataFrame indexed by execution time."""
    pd = _pandas()
    f = result.fills
    df = pd.DataFrame(
        {
            "instrument": f["instrument"].astype(np.int64),
            "side": _categorical(pd, f["side"].astype(np.int64), SIDE_NAMES),
            "price": _fixed(f["price"]),
            "qty": _fixed(f["qty"]),
            "fee": _fixed(f["fee"]),
            "liquidity": _categorical(pd, f["liquidity"].astype(np.int64), LIQUIDITY_NAMES),
            "cl_ord_id": f["cl_ord_id"].copy(),
            "mid": _fixed(f["mid"]),
            "best_bid": _fixed(f["best_bid"]),
            "best_ask": _fixed(f["best_ask"]),
            # NaN where the fill model does not track a queue position (raw -1).
            "queue_ahead": np.where(
                f["queue_ahead"] < 0, np.nan, f["queue_ahead"].astype(np.float64) * FIXED_SCALE
            ),
        },
        index=pd.DatetimeIndex(_time(f["ts"]), name="ts"),
    )
    df["notional"] = df["price"] * df["qty"]
    sign = np.where(f["side"] == 0, 1.0, -1.0)
    df["signed_qty"] = sign * df["qty"]
    # Spread capture: what the quote earned against the mid it was filled at.
    df["capture"] = df["signed_qty"] * (df["mid"] - df["price"])
    for key in sorted(k for k in f if k.startswith(MARKOUT_PREFIX)):
        ns = int(key[len(MARKOUT_PREFIX) : -2])
        mid_h = f[key]
        # 0 means the run ended before the horizon: excluded, not marked at the last mid.
        marked = np.where(mid_h > 0, mid_h.astype(np.float64) * FIXED_SCALE, np.nan)
        df[f"mid_{_horizon_label(ns)}"] = marked
        df[f"markout_{_horizon_label(ns)}"] = df["signed_qty"] * (marked - df["price"])
    return df


def _horizon_label(ns: int) -> str:
    if ns % 60_000_000_000 == 0:
        return f"{ns // 60_000_000_000}m"
    if ns % 1_000_000_000 == 0:
        return f"{ns // 1_000_000_000}s"
    if ns % 1_000_000 == 0:
        return f"{ns // 1_000_000}ms"
    return f"{ns // 1_000}us"


def markout_frame(result: BacktestResult) -> pd.DataFrame:
    """One row per (horizon, bucket) of ``result.markouts()``: markout and spread capture in
    quote currency and in bps of notional, plus the adverse selection between them."""
    pd = _pandas()
    rows = []
    for h in result.markouts():
        buckets = [(k, h[k]) for k in ("total", "buy", "sell", "maker", "taker")]
        buckets += [(f"instrument_{i}", b) for i, b in enumerate(h["instrument"])]
        for name, b in buckets:
            if name != "total" and b["fills"] == 0:
                continue
            rows.append(
                {
                    "horizon": h["label"],
                    "horizon_ns": h["horizon_ns"],
                    "bucket": name,
                    "excluded_fills": h["excluded_fills"] if name == "total" else 0,
                    **b,
                }
            )
    return pd.DataFrame(rows)


def equity_frame(result: BacktestResult) -> pd.DataFrame:
    """Equity bars (quote currency) indexed by bar end time; ``equity`` = realized +
    unrealized - fees."""
    pd = _pandas()
    e = result.equity
    realized = _fixed(e["realized"])
    unrealized = _fixed(e["unrealized"])
    fees = _fixed(e["fees"])
    return pd.DataFrame(
        {
            "equity": (e["realized"] + e["unrealized"] - e["fees"]).astype(np.float64)
            * FIXED_SCALE,
            "realized": realized,
            "unrealized": unrealized,
            "fees": fees,
            "position": _fixed(e["position"]),
            "mid": _fixed(e["mid"]),
            "bid_quoted": (e["quoted"] & 1).astype(bool),
            "ask_quoted": (e["quoted"] & 2).astype(bool),
        },
        index=pd.DatetimeIndex(_time(e["ts"]), name="ts"),
    )


def orders_frame(result: BacktestResult) -> pd.DataFrame:
    """Outbound orders indexed by engine send time; ``venue_ts`` is NaT when the latency
    model dropped the message."""
    pd = _pandas()
    o = result.orders
    return pd.DataFrame(
        {
            "venue_ts": _time_or_nat(o["venue_ts"]),
            "trigger_ts": _time_or_nat(o["trigger_ts"]),
            "cl_ord_id": o["cl_ord_id"].copy(),
            "instrument": o["instrument"].astype(np.int64),
            "side": _categorical(pd, o["side"].astype(np.int64), SIDE_NAMES),
            "price": _fixed(o["price"]),
            "qty": _fixed(o["qty"]),
            "kind": _categorical(pd, o["kind"].astype(np.int64), ORDER_KIND_NAMES),
            "type": _categorical(pd, o["type"].astype(np.int64), ORDER_TYPE_NAMES),
        },
        index=pd.DatetimeIndex(_time(o["ts"]), name="ts"),
    )


def to_pandas(result: BacktestResult) -> Dict[str, pd.DataFrame]:
    """``{"fills", "equity", "orders", "markouts"}`` DataFrames with float prices / quantities /
    PnL and ``datetime64[ns]`` timestamps."""
    return {
        "fills": fills_frame(result),
        "equity": equity_frame(result),
        "orders": orders_frame(result),
        "markouts": markout_frame(result),
    }


def sweep_frame(
    points: Iterable[Tuple[Dict[str, Any], BacktestResult]],
    stats: Sequence[str] = SWEEP_STATS,
) -> pd.DataFrame:
    """One row per sweep point: the grid parameters, the chosen ``stats()`` columns and the
    outbound hash. Row order follows the sweep (grid order)."""
    pd = _pandas()
    rows = []
    for params, result in points:
        s = result.stats()
        row = dict(params)
        for name in stats:
            row[name] = s[name]
        row["outbound_sha256"] = result.outbound_sha256
        rows.append(row)
    return pd.DataFrame(rows)
