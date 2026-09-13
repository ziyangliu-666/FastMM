"""Grid-search BasicMM's half spread and inventory skew on the synthetic market.

Run from the repository root:

    .venv/bin/python examples/python/sweep_spread.py

The sweep runs on a C++ thread pool with the GIL released; results come back in grid order.
Needs pandas (pip install fastmm[pandas]).
"""

from __future__ import annotations

from pathlib import Path

import pandas as pd

import fastmm

REPO = Path(__file__).resolve().parents[2]


def main() -> None:
    cfg = fastmm.BacktestConfig.from_toml(REPO / "configs" / "backtest-example.toml")
    cfg.fill_model = "matching"
    cfg.duration_s = 120
    cfg.seed = 11

    grid = {
        "half_spread_bps": [0.01, 0.02, 0.04, 0.08],
        "skew_bps_per_unit": [0.0, 0.01, 0.05],
    }
    points = fastmm.sweep(cfg, grid, data="synthetic")
    df = fastmm.sweep_frame(points)
    df = df.drop(columns="outbound_sha256").sort_values("net_pnl", ascending=False)

    with pd.option_context("display.width", 140, "display.max_columns", 20):
        print(f"{len(points)} runs, {cfg.duration_s:.0f} s each, seed {cfg.seed}")
        print(df.to_string(index=False, float_format=lambda v: f"{v:.4f}"))
    best = df.iloc[0]
    print(
        f"\nbest: half_spread_bps={best['half_spread_bps']} "
        f"skew_bps_per_unit={best['skew_bps_per_unit']} net_pnl={best['net_pnl']:.4f}"
    )


if __name__ == "__main__":
    main()
