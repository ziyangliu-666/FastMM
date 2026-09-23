import numpy as np
import pytest

import fastmm

pd = pytest.importorskip("pandas")


def test_to_pandas_round_trip(example_config):
    cfg = example_config.copy()
    cfg.duration_s = 10
    r = fastmm.run_backtest(cfg, data="synthetic")
    frames = r.to_pandas()
    assert set(frames) == {"fills", "equity", "orders", "markouts"}
    fills, equity, orders = frames["fills"], frames["equity"], frames["orders"]

    assert len(fills) == len(r.fills["ts"]) > 0
    assert fills.index.dtype == "datetime64[ns]"
    assert fills.index.asi8.tolist() == r.fills["ts"].tolist()
    np.testing.assert_array_equal(
        np.round(fills["price"].to_numpy() * 1e8).astype(np.int64), r.fills["price"]
    )
    np.testing.assert_array_equal(
        np.round(fills["qty"].to_numpy() * 1e8).astype(np.int64), r.fills["qty"]
    )
    assert set(fills["side"].astype(str)) <= {"buy", "sell"}

    assert len(equity) == len(r.equity["ts"])
    assert equity["equity"].iloc[-1] == pytest.approx(r.stats()["net_pnl"], abs=1e-6)
    assert len(orders) == len(r.orders["ts"])
    assert set(orders["kind"].astype(str)) <= {"new", "cancel", "replace"}

    assert fastmm.to_pandas(r)["fills"].equals(fills)


def test_sweep_frame(example_config):
    cfg = example_config.copy()
    cfg.duration_s = 5
    points = fastmm.sweep(cfg, {"half_spread_bps": [0.01, 0.02]}, data="synthetic", threads=2)
    df = fastmm.sweep_frame(points)
    assert list(df["half_spread_bps"]) == [0.01, 0.02]
    assert {"net_pnl", "fills", "sharpe_bar", "outbound_sha256"} <= set(df.columns)
    assert df["net_pnl"].tolist() == [p[1].stats()["net_pnl"] for p in points]
