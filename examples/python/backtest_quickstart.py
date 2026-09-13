"""Backtest BasicMM on the synthetic market and print the summary.

Run from the repository root:

    .venv/bin/python examples/python/backtest_quickstart.py

Saves runs/quickstart_equity.png when matplotlib is installed (pip install fastmm[plot]).
"""

from __future__ import annotations

from pathlib import Path

import fastmm

REPO = Path(__file__).resolve().parents[2]


def main() -> None:
    cfg = fastmm.BacktestConfig.from_toml(REPO / "configs" / "backtest-example.toml")
    cfg.strategy = "basic_mm"
    cfg.fill_model = "matching"  # strategy orders rest in the same book as the synthetic flow
    cfg.duration_s = 300
    cfg.seed = 7

    result = fastmm.run_backtest(cfg, data="synthetic")
    print(result.summary_table())

    stats = result.stats()
    print(
        f"fills={stats['fills']}  net_pnl={stats['net_pnl']:.4f}  "
        f"sharpe={stats['sharpe_annualized']:.2f}  uptime={stats['quote_uptime']:.1%}"
    )

    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed: skipping the equity plot")
        return

    import numpy as np

    eq = result.equity
    t = (eq["ts"] - eq["ts"][0]) / 1e9
    equity = (eq["realized"] + eq["unrealized"] - eq["fees"]) * fastmm.FIXED_SCALE
    position = eq["position"] * fastmm.FIXED_SCALE

    fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True, figsize=(9, 5))
    ax1.plot(t, equity, lw=1.2)
    ax1.set_ylabel("equity (quote)")
    ax1.set_title(f"{result.strategy} on synthetic data, seed {result.seed}")
    ax2.step(t, position, where="post", lw=1.0)
    ax2.axhline(0.0, color="0.6", lw=0.8)
    ax2.set_ylabel("position (base)")
    ax2.set_xlabel("seconds")
    for ax in (ax1, ax2):
        ax.grid(alpha=0.3)
        ax.spines[["top", "right"]].set_visible(False)
    fig.tight_layout()
    out = REPO / "runs" / "quickstart_equity.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=120)
    print(f"equity plot: {out.relative_to(REPO)} ({np.size(t)} bars)")


if __name__ == "__main__":
    main()
