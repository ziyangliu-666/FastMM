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
    "sharpe_annualized",
    "max_drawdown",
    "fills",
    "fill_ratio",
    "spread_captured_bps",
    "inventory_abs_mean",
    "quote_uptime",
)


def _pandas():
    try:
        import pandas as pd
    except ImportError as exc:  # pragma: no cover - exercised only without pandas
        raise ImportError(
            "fastmm.to_pandas() needs pandas; install it with `pip install fastmm[pandas]`"
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
        },
        index=pd.DatetimeIndex(_time(f["ts"]), name="ts"),
    )
    df["notional"] = df["price"] * df["qty"]
    return df


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
    """``{"fills", "equity", "orders"}`` DataFrames with float prices / quantities / PnL and
    ``datetime64[ns]`` timestamps."""
    return {
        "fills": fills_frame(result),
        "equity": equity_frame(result),
        "orders": orders_frame(result),
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
