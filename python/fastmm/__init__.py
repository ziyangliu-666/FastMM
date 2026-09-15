"""FastMM - ultra-low-latency market-making engine (Python research bindings).

    import fastmm
    cfg = fastmm.BacktestConfig.from_toml("configs/backtest-example.toml")
    result = fastmm.run_backtest(cfg, data="synthetic")
    print(result.summary_table())
    frames = result.to_pandas()          # fills / equity / orders DataFrames

Python strategies subclass fastmm.Strategy (see fastmm.strategy and docs/reference/python-api.md):

    result = fastmm.run_backtest(cfg, data="synthetic", strategy=MyStrategy, params={...})

A strategy with @fastmm.hot methods also runs live (fastmm_live, pip install "fastmm-engine[live]"):

    fastmm.run_live(MyStrategy, "configs/sim-local.toml")
"""

from ._core import (
    BacktestConfig,
    BacktestResult,
    BookTickerView,
    BookView,
    ConfigError,
    ConnectionView,
    Context,
    FillView,
    Instrument,
    OptionTickerView,
    Order,
    OrderBook,
    OrderUpdateView,
    Portfolio,
    PositionView,
    TradeView,
    __version__,
    build_info,
    disable_logging,
    enable_logging,
    inspect_journal,
    strategies,
    sweep,
)
from ._hot.decl import HotCompileError, State, hot
from .data import load_csv
from .live import run_live
from .results import FIXED_SCALE, sweep_frame, to_pandas
from .strategy import (
    BUY,
    LIQUIDITY_UNKNOWN,
    MAKER,
    SELL,
    TAKER,
    OrderRejected,
    Param,
    StaleViewError,
    Strategy,
    StrategyError,
    run_backtest,
)

__all__ = [
    "BUY",
    "FIXED_SCALE",
    "HotCompileError",
    "LIQUIDITY_UNKNOWN",
    "MAKER",
    "SELL",
    "TAKER",
    "BacktestConfig",
    "BacktestResult",
    "BookTickerView",
    "BookView",
    "ConfigError",
    "ConnectionView",
    "Context",
    "FillView",
    "Instrument",
    "OptionTickerView",
    "Order",
    "OrderBook",
    "OrderRejected",
    "OrderUpdateView",
    "Param",
    "Portfolio",
    "PositionView",
    "StaleViewError",
    "State",
    "Strategy",
    "StrategyError",
    "TradeView",
    "__version__",
    "build_info",
    "disable_logging",
    "enable_logging",
    "hot",
    "inspect_journal",
    "load_csv",
    "run_backtest",
    "run_live",
    "strategies",
    "sweep",
    "sweep_frame",
    "to_pandas",
]


def __getattr__(name: str):  # noqa: ANN202
    # fastmm.fx imports numba, so it loads on first use.
    if name == "fx":
        import importlib

        return importlib.import_module(".fx", __name__)
    raise AttributeError(f"module 'fastmm' has no attribute {name!r}")
