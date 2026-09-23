"""FastMM - market-making engine (Python bindings; docs/python.md).

A strategy with @fastmm.hot methods (compiled with Numba) runs in backtests, replays and live:

    import fastmm

    class MyMM(fastmm.Strategy):
        @fastmm.hot
        def on_book(self, ctx, book):
            ...

    cfg = fastmm.BacktestConfig.from_toml("configs/backtest-example.toml")
    result = fastmm.run_backtest(cfg, data="synthetic", strategy=MyMM)
    print(result.summary_table())
    frames = result.to_pandas()          # fills / equity / orders / markouts DataFrames
    fastmm.run_live(MyMM, "configs/sim-local.toml")    # needs fastmm-engine-live

A class with plain fastmm.Strategy hooks (no @fastmm.hot) runs in backtests only.
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
    convert_data,
    data_sources,
    disable_logging,
    enable_logging,
    inspect_journal,
    strategies,
    sweep,
)
from ._hot.decl import HotCompileError, State, hot
from ._slow.decl import every
from ._slow.replay import ReplayResult, replay
from .data import load_csv
from .live import run_live
from .results import FIXED_SCALE, markout_frame, sweep_frame, to_pandas
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
    "ReplayResult",
    "StaleViewError",
    "State",
    "Strategy",
    "StrategyError",
    "TradeView",
    "__version__",
    "build_info",
    "convert_data",
    "data_sources",
    "disable_logging",
    "enable_logging",
    "every",
    "hot",
    "inspect_journal",
    "load_csv",
    "replay",
    "run_backtest",
    "run_live",
    "strategies",
    "sweep",
    "sweep_frame",
    "markout_frame",
    "to_pandas",
]


def __getattr__(name: str):  # noqa: ANN202
    # fastmm.fx imports numba, so it loads on first use.
    if name == "fx":
        import importlib

        return importlib.import_module(".fx", __name__)
    raise AttributeError(f"module 'fastmm' has no attribute {name!r}")
