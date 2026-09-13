"""
FastMM core bindings: backtests, parameter sweeps, order book research helpers
"""
from __future__ import annotations
import numpy
import typing
__all__: list[str] = ['BacktestConfig', 'BacktestResult', 'ConfigError', 'OrderBook', 'build_info', 'disable_logging', 'enable_logging', 'inspect_journal', 'run_backtest', 'strategies', 'sweep']
class BacktestConfig:
    """
    Everything one backtest needs: engine, instruments, strategy and parameters, simulated venue (fill model, latency, fees) and the synthetic market. Build one with from_toml() or single_instrument().
    """
    @staticmethod
    def from_toml(path: typing.Any) -> BacktestConfig:
        """
        Load a FastMM TOML config (sections [engine] [[instruments]] [strategy] [risk] [venues.<x>.fees] [sim] [backtest]). Raises ConfigError.
        """
    @staticmethod
    def from_toml_string(text: str) -> BacktestConfig:
        """
        Parse a TOML document held in a string. Raises ConfigError.
        """
    @staticmethod
    def single_instrument(symbol: str, tick: str, lot: str) -> BacktestConfig:
        """
        One instrument on venue 0 with exact decimal tick/lot (e.g. '0.01', '0.00001'); synthetic market, matching fill model, no strategy selected.
        """
    def __copy__(self) -> BacktestConfig:
        ...
    def __deepcopy__(self, memo: typing.Any) -> BacktestConfig:
        ...
    def __repr__(self) -> str:
        ...
    def clear_params(self) -> None:
        """
        Remove every strategy parameter (use before switching strategy).
        """
    def copy(self) -> BacktestConfig:
        ...
    def set_param(self, key: str, value: typing.Any) -> None:
        """
        Set one strategy parameter (str / int / float / bool).
        """
    @property
    def cancel_rate_per_order_s(self) -> float:
        """
        Synthetic per-order cancel hazard per second.
        """
    @cancel_rate_per_order_s.setter
    def cancel_rate_per_order_s(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def duration_s(self) -> float:
        """
        Synthetic horizon in seconds (data files run to their end).
        """
    @duration_s.setter
    def duration_s(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def engine_seed(self) -> int:
        """
        Engine RNG seed ([engine] rng_seed).
        """
    @engine_seed.setter
    def engine_seed(self, arg1: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def equity_bar_s(self) -> float:
        """
        Equity bar length in seconds.
        """
    @equity_bar_s.setter
    def equity_bar_s(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def fill_model(self) -> str:
        """
        'matching' (orders rest in the simulated book) or 'l2_queue' (queue position on L2 data).
        """
    @fill_model.setter
    def fill_model(self, arg1: str) -> None:
        ...
    @property
    def initial_capital(self) -> float:
        """
        Quote currency; only used for max_drawdown_pct.
        """
    @initial_capital.setter
    def initial_capital(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def journal_out(self) -> str:
        """
        Record the session to this .fmj ('' = no journal).
        """
    @journal_out.setter
    def journal_out(self, arg0: str) -> None:
        ...
    @property
    def latency_fixed_us(self) -> int:
        """
        Fixed order and ack latency, microseconds.
        """
    @latency_fixed_us.setter
    def latency_fixed_us(self, arg1: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def latency_jitter_us(self) -> int:
        """
        Mean lognormal order and ack latency jitter, microseconds.
        """
    @latency_jitter_us.setter
    def latency_jitter_us(self, arg1: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def latency_md_jitter_us(self) -> int:
        """
        Mean market-data latency jitter, microseconds.
        """
    @latency_md_jitter_us.setter
    def latency_md_jitter_us(self, arg1: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def latency_md_us(self) -> int:
        """
        Fixed market-data latency, microseconds.
        """
    @latency_md_us.setter
    def latency_md_us(self, arg1: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def limit_rate_per_s(self) -> float:
        """
        Synthetic limit order arrivals per second.
        """
    @limit_rate_per_s.setter
    def limit_rate_per_s(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def maker_fee_bps(self) -> float:
        """
        Maker fee in bps (negative = rebate), 0.01 bps resolution.
        """
    @maker_fee_bps.setter
    def maker_fee_bps(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def market_rate_per_s(self) -> float:
        """
        Synthetic market order arrivals per second.
        """
    @market_rate_per_s.setter
    def market_rate_per_s(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def measure_wall_clock(self) -> bool:
        """
        Measure wall-clock tick-to-order per engine step.
        """
    @measure_wall_clock.setter
    def measure_wall_clock(self, arg0: bool) -> None:
        ...
    @property
    def mid_step_rate_per_s(self) -> float:
        """
        Synthetic latent mid +-1 tick steps per second.
        """
    @mid_step_rate_per_s.setter
    def mid_step_rate_per_s(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def output_dir(self) -> str:
        """
        Where the apps write results; run_backtest() never writes (see BacktestResult.write_all).
        """
    @output_dir.setter
    def output_dir(self, arg0: str) -> None:
        ...
    @property
    def p_drop(self) -> float:
        """
        Probability an outbound order message is lost.
        """
    @p_drop.setter
    def p_drop(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def params(self) -> dict:
        """
        Strategy parameters as {name: str} (a copy; assign a dict or use set_param()).
        """
    @params.setter
    def params(self, arg1: dict) -> None:
        ...
    @property
    def path(self) -> str:
        """
        Data file for source journal/csv.
        """
    @path.setter
    def path(self, arg0: str) -> None:
        ...
    @property
    def queue_conservatism(self) -> float:
        """
        L2 queue model: 0 = cancels ahead always help, 1 = never.
        """
    @queue_conservatism.setter
    def queue_conservatism(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def seed(self) -> int:
        """
        Seed of the synthetic market and the latency model.
        """
    @seed.setter
    def seed(self, arg1: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def source(self) -> str:
        """
        'synthetic' | 'journal' | 'csv' | '' (used when run_backtest(data=None)).
        """
    @source.setter
    def source(self, arg0: str) -> None:
        ...
    @property
    def start_mid(self) -> str:
        """
        Synthetic market initial mid (exact decimal string; float accepted).
        """
    @start_mid.setter
    def start_mid(self, arg1: typing.Any) -> None:
        ...
    @property
    def start_ns(self) -> int:
        """
        Synthetic market start time (ns since the epoch).
        """
    @start_ns.setter
    def start_ns(self, arg1: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def strategy(self) -> str:
        """
        Registered strategy name.
        """
    @strategy.setter
    def strategy(self, arg0: str) -> None:
        ...
    @property
    def supports_replace(self) -> bool:
        """
        Venue and engine use native replace instead of cancel-then-new.
        """
    @supports_replace.setter
    def supports_replace(self, arg1: bool) -> None:
        ...
    @property
    def taker_fee_bps(self) -> float:
        """
        Taker fee in bps, 0.01 bps resolution.
        """
    @taker_fee_bps.setter
    def taker_fee_bps(self, arg1: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class BacktestResult:
    """
    Outcome of one backtest. fills / equity / orders are dicts of read-only numpy views over the C++ vectors (prices, quantities, fees and PnL are raw int64 with a 1e-8 scale); to_pandas() converts them.
    """
    def __repr__(self) -> str:
        ...
    def engine_stats(self) -> dict:
        """
        The engine's own counters (PnL converted to float).
        """
    def stats(self) -> dict:
        """
        Every summary metric: PnL / volume / inventory in quote or base units (float), counts and latency percentiles (int, ns).
        """
    def summary_json(self) -> str:
        """
        Metrics as a JSON document.
        """
    def summary_table(self) -> str:
        """
        Human-readable metrics table.
        """
    def to_pandas(self) -> typing.Any:
        """
        fills / equity / orders as pandas DataFrames with float prices and datetime64 timestamps (see fastmm.results.to_pandas).
        """
    def transport_stats(self) -> dict:
        """
        Simulated venue / transport counters.
        """
    def write_all(self, dir: typing.Any) -> None:
        """
        Write equity.csv, fills.csv, orders.csv and summary.json into dir (created).
        """
    @property
    def end_ts(self) -> int:
        """
        Last event time, ns.
        """
    @property
    def engine_steps(self) -> int:
        ...
    @property
    def equity(self) -> dict:
        """
        Equity bar columns (zero-copy numpy views).
        """
    @property
    def fills(self) -> dict:
        """
        Fill columns (zero-copy numpy views).
        """
    @property
    def md_events(self) -> int:
        """
        Market-data messages delivered to the engine.
        """
    @property
    def orders(self) -> dict:
        """
        Order columns (zero-copy numpy views).
        """
    @property
    def outbound_messages(self) -> int:
        ...
    @property
    def outbound_sha256(self) -> str:
        """
        SHA-256 of every message the engine sent (determinism fingerprint).
        """
    @property
    def params(self) -> dict:
        """
        Strategy parameters of the run as {name: str}.
        """
    @property
    def seed(self) -> int:
        ...
    @property
    def start_ts(self) -> int:
        """
        First event time, ns.
        """
    @property
    def strategy(self) -> str:
        ...
    @property
    def wall_seconds(self) -> float:
        ...
class ConfigError(ValueError):
    pass
class OrderBook:
    """
    Price-aggregated L2 book (up to 256 levels per side) for research. Prices and quantities are floats, stored in 1e-8 fixed point; best-first level order.
    """
    def __init__(self) -> None:
        ...
    def __repr__(self) -> str:
        ...
    def apply_delta(self, bids: typing.Any = None, asks: typing.Any = None) -> None:
        """
        Set/update levels; qty 0 deletes the level.
        """
    def apply_snapshot(self, bids: typing.Any, asks: typing.Any) -> None:
        """
        Replace the book. bids/asks: (n, 2) float arrays or sequences of (price, qty).
        """
    def asks(self, n: typing.SupportsInt | typing.SupportsIndex | None = None) -> numpy.ndarray:
        """
        Top n ask levels (all if None) as an (k, 2) float64 array, best first.
        """
    def best_ask(self) -> typing.Any:
        """
        (price, qty) of the best ask, or None.
        """
    def best_bid(self) -> typing.Any:
        """
        (price, qty) of the best bid, or None.
        """
    def bids(self, n: typing.SupportsInt | typing.SupportsIndex | None = None) -> numpy.ndarray:
        """
        Top n bid levels (all if None) as an (k, 2) float64 array, best first.
        """
    def clear(self) -> None:
        ...
    def imbalance(self, levels: typing.SupportsInt | typing.SupportsIndex = 1) -> float:
        """
        (bid qty - ask qty) / (bid qty + ask qty) over the top `levels`, in [-1, 1].
        """
    def microprice(self) -> typing.Any:
        """
        Top-of-book quantity-weighted mid, or None if a side is empty.
        """
    def mid(self) -> typing.Any:
        """
        (best bid + best ask) / 2, or None if a side is empty.
        """
    def spread(self) -> typing.Any:
        """
        best ask - best bid, or None if a side is empty.
        """
    def weighted_mid(self, levels: typing.SupportsInt | typing.SupportsIndex = 5) -> typing.Any:
        """
        Mid of the per-side quantity-weighted prices over the top `levels`.
        """
    @property
    def ask_depth(self) -> int:
        ...
    @property
    def bid_depth(self) -> int:
        ...
    @property
    def crossed(self) -> bool:
        ...
    @property
    def truncated(self) -> bool:
        """
        A side overflowed 256 levels since the last snapshot.
        """
def build_info() -> str:
    """
    Compiler, flags and build type of the native module.
    """
def disable_logging() -> None:
    """
    Flush pending log records, stop the logger thread and close the log file.
    """
def enable_logging(level: str = 'warn', path: typing.Any = None) -> None:
    """
    Start writing C++ log records at `level` or above to `path` (appending) or to stderr. Calling it again restarts the logger with the new settings.
    """
def inspect_journal(path: typing.Any) -> dict:
    """
    Header and message counts of an .fmj journal.
    """
def run_backtest(config: BacktestConfig, data: typing.Any = None, strategy: str | None = None) -> BacktestResult:
    """
    Run one backtest with the GIL released.
    
    data: None (config.source / config.path), 'synthetic', a .fmj or .csv path, or a dict of numpy arrays {ts: int64, type: uint8, inst: uint32, side: int8, price: int64 (raw 1e-8) | float64, qty: int64 | float64, seq: uint64 (optional)} used without copying.
    strategy: registry name; defaults to config.strategy.
    """
def strategies() -> dict:
    """
    Registered strategies and their parameter schemas: {name: [{'name', 'type', 'default', 'min', 'max', 'doc'}, ...]}.
    """
def sweep(config: BacktestConfig, grid: dict, data: typing.Any = None, strategy: str | None = None, threads: typing.SupportsInt | typing.SupportsIndex = 0) -> list:
    """
    Cartesian parameter sweep on a thread pool (GIL released). grid: {param: [values]}. Returns [(params, BacktestResult)] in grid order, first parameter varying slowest. Every worker opens its own cursor over `data` (same forms as run_backtest). threads <= 0 uses every hardware thread.
    """
__version__: str = '0.1.0'
