"""FastMM - ultra-low-latency market-making engine (Python research bindings).

    import fastmm
    cfg = fastmm.BacktestConfig.from_toml("configs/backtest-example.toml")
    result = fastmm.run_backtest(cfg, data="synthetic")
    print(result.summary_table())
    frames = result.to_pandas()          # fills / equity / orders DataFrames
"""

from ._core import (
    BacktestConfig,
    BacktestResult,
    ConfigError,
    OrderBook,
    __version__,
    build_info,
    inspect_journal,
    run_backtest,
    strategies,
    sweep,
)
from .data import load_csv
from .results import FIXED_SCALE, sweep_frame, to_pandas

__all__ = [
    "FIXED_SCALE",
    "BacktestConfig",
    "BacktestResult",
    "ConfigError",
    "OrderBook",
    "__version__",
    "build_info",
    "inspect_journal",
    "load_csv",
    "run_backtest",
    "strategies",
    "sweep",
    "sweep_frame",
    "to_pandas",
]
