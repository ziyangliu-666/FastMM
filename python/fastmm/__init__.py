"""FastMM - ultra-low-latency market-making engine (Python research bindings).

    import fastmm
    cfg = fastmm.BacktestConfig.from_toml("configs/backtest-example.toml")
    result = fastmm.run_backtest(cfg, data="synthetic")
    print(result.summary_table())
    frames = result.to_pandas()          # fills / equity / orders DataFrames

Python strategies subclass fastmm.Strategy (see fastmm.strategy and docs/reference/python-api.md):

    result = fastmm.run_backtest(cfg, data="synthetic", strategy=MyStrategy, params={...})
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
from .data import load_csv
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
    "Strategy",
    "StrategyError",
    "TradeView",
    "__version__",
    "build_info",
    "disable_logging",
    "enable_logging",
    "inspect_journal",
    "load_csv",
    "run_backtest",
    "strategies",
    "sweep",
    "sweep_frame",
    "to_pandas",
]
