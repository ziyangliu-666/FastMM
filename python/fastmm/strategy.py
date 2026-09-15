"""Python strategies (ADR-0012, section 7).

A strategy is a ``fastmm.Strategy`` subclass with the C++ hook names and argument order, and
``fastmm.Param`` descriptors for its parameters::

    class SkewMM(fastmm.Strategy):
        half_spread_bps = fastmm.Param(5.0, min=0.0, max=10_000.0, doc="half spread, bps")

        def on_book(self, ctx, inst, book):
            ...

    result = fastmm.run_backtest(cfg, data="synthetic", strategy=SkewMM, params={...})

Only the hooks a class defines are called. The hooks run inside the C++ engine
(``Engine<PyStrategy, SimClock, SimTransport, InlineFeed>``), so risk checks, the OMS, the quote
manager, the journal and the outbound hash are the ones C++ strategies use.
"""

from __future__ import annotations

import inspect
import math
import re
import warnings
from decimal import Decimal
from typing import (
    TYPE_CHECKING,
    Any,
    Dict,
    Generic,
    Mapping,
    Optional,
    Tuple,
    Type,
    TypeVar,
    Union,
    overload,
)

from . import _core
from ._hot import decl as _hot_decl

if TYPE_CHECKING:  # pragma: no cover
    from ._core import BacktestConfig, BacktestResult

__all__ = [
    "BUY",
    "SELL",
    "LIQUIDITY_UNKNOWN",
    "MAKER",
    "TAKER",
    "HOOKS",
    "OrderRejected",
    "Param",
    "StaleViewError",
    "Strategy",
    "StrategyError",
    "run_backtest",
]

BUY = 0
"""Side of a bid / buy order (the ``side`` columns of results use the same numbers)."""
SELL = 1
"""Side of an ask / sell order."""
LIQUIDITY_UNKNOWN = 0
MAKER = 1
TAKER = 2

StaleViewError = _core.StaleViewError
OrderRejected = _core.OrderRejected

# Hook name -> positional parameters after self (the C++ table in strategies/hooks.hpp).
HOOKS: Dict[str, Tuple[str, ...]] = {
    "on_start": ("ctx",),
    "on_stop": ("ctx",),
    "on_book": ("ctx", "inst", "book"),
    "on_book_ticker": ("ctx", "inst", "msg"),
    "on_trade": ("ctx", "inst", "trade"),
    "on_option_ticker": ("ctx", "inst", "msg"),
    "on_fill": ("ctx", "fill"),
    "on_order_update": ("ctx", "update"),
    "on_timer": ("ctx", "timer_id", "tag"),
    "on_connection": ("ctx", "msg"),
    "on_quoting": ("ctx", "enabled"),
}

# Likely misspellings, as in the C++ checker: a warning, because a helper may use such a name.
_NEAR_MISSES: Dict[str, str] = {
    "on_fills": "on_fill",
    "on_execution": "on_fill",
    "on_trades": "on_trade",
    "on_order_book": "on_book",
    "on_orderbook": "on_book",
    "on_book_update": "on_book",
    "on_depth": "on_book",
    "on_tick": "on_book",
    "on_bbo": "on_book_ticker",
    "on_ticker": "on_book_ticker",
    "on_options_ticker": "on_option_ticker",
    "on_order": "on_order_update",
    "on_order_event": "on_order_update",
    "on_timer_fired": "on_timer",
    "on_connection_state": "on_connection",
    "on_disconnect": "on_connection",
    "on_init": "on_start",
    "on_shutdown": "on_stop",
    "on_quote": "on_quoting",
    "onBook": "on_book",
    "OnBook": "on_book",
    "onTrade": "on_trade",
    "onFill": "on_fill",
}


class StrategyError(RuntimeError):
    """A strategy hook raised. The original exception (with its traceback) is ``__cause__``;
    ``result`` is the partial BacktestResult of the run, which stopped after the failing event."""

    def __init__(self, message: str, result: "Optional[BacktestResult]" = None,
                 hook: str = "", now_ns: int = 0, events: int = 0, status: int = 0,
                 fail_code: int = 0, kill_reason: str = "", slow_failure: str = "") -> None:
        super().__init__(message)
        self.result = result
        self.hook = hook
        self.now_ns = now_ns
        self.events = events
        # Hot hooks: the hook status (1 exception, 2 ctx.fail, 3 bad float level), the code passed
        # to ctx.fail() and the kill reason.
        self.status = status
        self.fail_code = fail_code
        self.kill_reason = kill_reason
        # Slow methods: "exception" or "fills overflow" when the slow tier stopped the run.
        self.slow_failure = slow_failure


# ---- parameters ----------------------------------------------------------------------------------

T = TypeVar("T", bool, int, float)

_INT_RE = re.compile(r"[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?\Z")
_FLOAT_RE = re.compile(r"[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?\Z")
_TRUE = ("true", "1", "yes", "on")
_FALSE = ("false", "0", "no", "off")


def _format_double(x: float) -> str:
    """std::to_chars(double) shortest form: fixed or scientific, whichever is shorter."""
    if x == 0.0:
        return "-0" if math.copysign(1.0, x) < 0 else "0"
    sign, digits, exp = Decimal(repr(x)).as_tuple()
    ds = "".join(str(d) for d in digits).rstrip("0") or "0"
    exp = int(exp) + (len(digits) - len(ds))  # value == int(ds) * 10**exp
    n = len(ds)
    if exp >= 0:
        fixed = ds + "0" * exp
    elif n + exp > 0:
        fixed = ds[: n + exp] + "." + ds[n + exp:]
    else:
        fixed = "0." + "0" * (-(n + exp)) + ds
    e10 = exp + n - 1
    sci = ds[0] + ("." + ds[1:] if n > 1 else "") + "e" + ("-" if e10 < 0 else "+")
    sci += f"{abs(e10):02d}"
    out = fixed if len(fixed) <= len(sci) else sci
    return ("-" if sign else "") + out


def _value_string(value: Any) -> str:
    """The string form the C++ parameter parser receives from Python (repr for floats)."""
    if isinstance(value, str):
        return value
    try:
        import numpy as np  # numpy scalars behave like the builtins they wrap

        if isinstance(value, np.bool_):
            value = bool(value)
        elif isinstance(value, np.integer):
            value = int(value)
        elif isinstance(value, np.floating):
            value = float(value)
    except ImportError:  # pragma: no cover
        pass
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    raise TypeError(f"parameter values must be str, int, float or bool, got {type(value).__name__}")


def _parse_int(s: str) -> Optional[int]:
    """detail::parse_scaled_decimal(s, 0): '3', '3.0', '2e3'; no fractional part may remain."""
    if not _INT_RE.match(s):
        return None
    d = Decimal(s)
    if d != d.to_integral_value():
        return None
    v = int(d)
    return v if -(2**63) <= v < 2**63 else None


def _parse_double(s: str) -> Optional[float]:
    """std::from_chars after dropping one leading '+'; finite values only."""
    if not _FLOAT_RE.match(s):
        return None
    v = float(s)
    return v if math.isfinite(v) else None


class Param(Generic[T]):
    """A strategy parameter: typed from its default (bool, int or float), with an optional range.

    Values from ``cfg.params`` (strings) and ``run_backtest(params=...)`` (Python scalars) are
    parsed like FASTMM_PARAM values in C++, with the same error messages. Read it as a plain
    attribute: ``self.levels``.
    """

    def __init__(self, default: T, *, min: Optional[T] = None, max: Optional[T] = None,
                 doc: str = "") -> None:
        if isinstance(default, bool):
            self.type_name = "bool"
            self.type: type = bool
        elif isinstance(default, int):
            self.type_name, self.type = "int", int
        elif isinstance(default, float):
            self.type_name, self.type = "double", float
        else:
            raise TypeError(
                f"Param default must be bool, int or float, got {type(default).__name__}")
        if self.type is bool:
            min = False if min is None else min
            max = True if max is None else max
        self.default = default
        self.min = None if min is None else self.type(min)
        self.max = None if max is None else self.type(max)
        self.doc = doc
        self.name = ""
        if self.type is float and not math.isfinite(default):
            raise ValueError("Param default must be finite")
        if not self._in_range(default):
            raise ValueError(f"Param default {default!r} outside [{self._bound(self.min, '-inf')}, "
                             f"{self._bound(self.max, 'inf')}]")

    def __set_name__(self, owner: type, name: str) -> None:
        self.name = name

    @overload
    def __get__(self, obj: None, objtype: Optional[type] = None) -> "Param[T]": ...
    @overload
    def __get__(self, obj: object, objtype: Optional[type] = None) -> T: ...

    def __get__(self, obj: Any, objtype: Any = None) -> Any:
        if obj is None:
            return self
        return obj.__dict__.get(self.name, self.default)

    def __set__(self, obj: Any, value: Any) -> None:
        err, typed = self.parse(value)
        if err is not None:
            raise ValueError(f"parameter '{self.name}': {err}")
        obj.__dict__[self.name] = typed

    def _format(self, v: Any) -> str:
        if self.type is bool:
            return "true" if v else "false"
        if self.type is int:
            return str(v)
        return _format_double(v)

    def _in_range(self, v: Any) -> bool:
        below = self.min is not None and v < self.min
        above = self.max is not None and self.max < v
        return not (below or above)

    def _bound(self, v: Any, missing: str) -> str:
        return missing if v is None else self._format(v)

    def parse(self, value: Any) -> Tuple[Optional[str], Any]:
        """(error, typed value): error is the C++ message without the parameter name."""
        s = _value_string(value)
        if self.type is bool:
            typed: Any = True if s in _TRUE else False if s in _FALSE else None
            hint = " (true or false)"
        elif self.type is int:
            typed = _parse_int(s)
            hint = " (a whole number)"
        else:
            typed = _parse_double(s[1:] if s.startswith("+") else s)
            hint = " (a finite number)"
        if typed is None:
            return f"cannot parse '{s}' as {self.type_name}{hint}", None
        if not self._in_range(typed):
            return (f"value {s} outside [{self._bound(self.min, '-inf')}, "
                    f"{self._bound(self.max, 'inf')}]"), None
        return None, typed

    def format(self, value: Any) -> str:
        """The value in a form parse() accepts (the C++ formatter)."""
        return self._format(value)

    def describe(self) -> Dict[str, Any]:
        return {"name": self.name, "type": self.type_name, "default": self.default,
                "min": self.min, "max": self.max, "doc": self.doc}

    def __repr__(self) -> str:
        return f"Param({self.default!r}, min={self.min!r}, max={self.max!r}, doc={self.doc!r})"


# ---- strategy ------------------------------------------------------------------------------------

class Strategy:
    """Base class of Python strategies.

    Define any subset of the hooks (``on_start(self, ctx)``, ``on_book(self, ctx, inst, book)``,
    ``on_fill(self, ctx, fill)``, ... see ``HOOKS``); undefined hooks are never called. Declare
    parameters with ``Param``; override ``validate()`` for cross-field checks.
    """

    def __init__(self) -> None:
        self._fastmm_used = False

    # The HotSpec of a class with @fastmm.hot methods (set at class creation), else None.
    _fastmm_hot: "Optional[_hot_decl.HotSpec]" = None

    def __init_subclass__(cls, **kwargs: Any) -> None:
        super().__init_subclass__(**kwargs)
        cls._fastmm_hot = _hot_decl.check_class(cls, HOOKS, Param)

    # Silences the "did you mean" warning for helper methods named like a misspelt hook.
    fastmm_allow_near_miss_names = False

    @classmethod
    def strategy_name(cls) -> str:
        """Name in results and journals: ``py:<QualName>``."""
        return "py:" + cls.__qualname__

    @classmethod
    def params(cls) -> Dict[str, Param]:
        """Every Param of the class and its bases, in declaration order (bases first)."""
        out: Dict[str, Param] = {}
        for klass in reversed(cls.__mro__):
            for name, attr in vars(klass).items():
                if isinstance(attr, Param):
                    out.pop(name, None)
                    out[name] = attr
        return out

    @classmethod
    def schema(cls) -> list:
        """Parameter schema like fastmm.strategies(): [{'name', 'type', 'default', 'min', 'max',
        'doc'}, ...]; an unbounded side is None."""
        return [p.describe() for p in cls.params().values()]

    @classmethod
    def hooks(cls) -> Tuple[str, ...]:
        """The hooks this class defines, in table order. Raises TypeError for a hook with the wrong
        signature and warns about likely misspellings."""
        found = []
        for name, expected in HOOKS.items():
            attr = inspect.getattr_static(cls, name, None)
            if attr is None:
                continue
            fn = getattr(cls, name)
            want = f"{name}(self, {', '.join(expected)})"
            if not callable(fn) or isinstance(attr, (staticmethod, classmethod)):
                raise TypeError(
                    f"fastmm: {cls.__qualname__}.{name} must be a method; expected {want}")
            if not _accepts(fn, 1 + len(expected)):
                raise TypeError(
                    f"fastmm: {cls.__qualname__}.{name} has the wrong signature; expected {want}")
            found.append(name)
        if not getattr(cls, "fastmm_allow_near_miss_names", False):
            for miss, hook in _NEAR_MISSES.items():
                if inspect.getattr_static(cls, miss, None) is not None:
                    warnings.warn(f"fastmm: {cls.__qualname__}.{miss} is not a hook; did you mean "
                                  f"{hook}?", stacklevel=3)
        return tuple(found)

    def validate(self) -> Optional[str]:
        """Optional cross-field check after parameters are applied; return an error message."""
        return None

    def configure(self, params: Mapping[str, Any]) -> None:
        """Apply parameters (like C++ StrategyBase::configure): keys in sorted order, the first
        error raises ValueError and leaves the previous values."""
        declared = self.params()
        before = {name: getattr(self, name) for name in declared}
        try:
            for key in sorted(params):
                p = declared.get(key)
                if p is None:
                    raise ValueError(f"unknown parameter '{key}'")
                err, typed = p.parse(params[key])
                if err is not None:
                    raise ValueError(f"parameter '{key}': {err}")
                self.__dict__[key] = typed
            err = self.validate()
            if err:
                raise ValueError(str(err))
        except BaseException:
            for name, value in before.items():
                self.__dict__[name] = value
            raise

    def param_values(self) -> Dict[str, str]:
        """Effective parameters as strings (BacktestResult.params)."""
        return {name: p.format(getattr(self, name)) for name, p in self.params().items()}

    def publish(self, inst: Any = None, **values: Any) -> bool:
        """New parameter values for a running strategy with slow methods, from any thread: the same
        checks and effect as ctx.publish(). False when no session of this instance runs or the
        session did not take the update."""
        publisher = self.__dict__.get("_fastmm_publisher")
        if publisher is None:
            return False
        return bool(publisher.publish(inst, values))


def _accepts(fn: Any, nargs: int) -> bool:
    try:
        sig = inspect.signature(fn)
    except (TypeError, ValueError):  # builtins without a signature: trust them
        return True
    required = total = 0
    for p in sig.parameters.values():
        if p.kind == p.VAR_POSITIONAL:
            return required <= nargs
        if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD):
            total += 1
            if p.default is p.empty:
                required += 1
        elif p.kind == p.KEYWORD_ONLY and p.default is p.empty:
            return False
    return required <= nargs <= total


# ---- running -------------------------------------------------------------------------------------

StrategyArg = Union[None, str, Type[Strategy], Strategy]


def _instance(strategy: Any) -> Strategy:
    if isinstance(strategy, type) and issubclass(strategy, Strategy):
        return strategy()
    if isinstance(strategy, Strategy):
        if getattr(strategy, "_fastmm_used", False):
            raise ValueError("fastmm: this Strategy instance has already run; pass the class to "
                             "get a fresh instance per run")
        return strategy
    raise TypeError("strategy must be a registered strategy name, a fastmm.Strategy subclass or "
                    f"an unused instance, got {type(strategy).__name__}")


def run_backtest(config: "BacktestConfig", data: Any = None, strategy: StrategyArg = None,
                 params: Optional[Mapping[str, Any]] = None, *,
                 hot_cache: bool = True, slow_delay_ms: float = 0,
                 max_param_age_ms: Optional[int] = None, fills_capacity: Optional[int] = None,
                 recent_rows: int = 4096) -> "BacktestResult":
    """Run one backtest.

    data: None (config.source / config.path), 'synthetic', a .fmj or .csv path, or a dict of numpy
    columns (see docs/python.md).

    strategy: None (config.strategy), a registered C++ strategy name, a fastmm.Strategy subclass
    (a fresh instance per run) or an unused Strategy instance.

    params: overrides applied on top of config.params (str, int, float or bool values).

    hot_cache: for a strategy with @fastmm.hot methods, whether compiled hooks are kept in
    FastMM's Numba cache directory; False compiles every hook afresh.

    C++ strategies run with the GIL released. A Python strategy holds the GIL for the whole run;
    a hook that raises stops the run and raises StrategyError (original exception as __cause__,
    partial result as .result). KeyboardInterrupt and SystemExit from a hook propagate unchanged,
    with the partial result as .result. Hot hooks run with the GIL released; a failing hot hook
    trips the kill switch and raises StrategyError with .status, .fail_code and .kill_reason.

    Slow methods (on_start, on_stop and @fastmm.every methods of a class with hot hooks) run at
    simulated times. slow_delay_ms: simulated delay before their publishes take effect.
    max_param_age_ms: overrides [strategy] max_param_age_ms (0 disables); None keeps the
    configured value, or when that is 0 uses 3 x the shortest @fastmm.every period and at least
    1000 ms. fills_capacity: fills the ring holds (None: from the order rate limit). recent_rows:
    rows per instrument that ctx.recent() keeps. A slow method that raises, or a full fills ring,
    stops the run and raises StrategyError with .slow_failure.
    """
    if strategy is None or isinstance(strategy, str):
        if params:
            config = config.copy()
            for key, value in params.items():
                config.set_param(key, value)
        return _core.run_backtest(config, data, strategy)

    instance = _instance(strategy)
    cls = type(instance)
    name = cls.strategy_name()
    spec = cls._fastmm_hot
    hooks = () if spec is not None else cls.hooks()
    merged: Dict[str, Any] = dict(config.params)
    if params:
        merged.update(params)
    try:
        instance.configure(merged)
    except ValueError as e:
        raise ValueError(f"{name}: {e}") from None
    instance._fastmm_used = True
    if spec is not None:
        return _run_hot(config, data, instance, name, spec, hot_cache, slow_delay_ms,
                        max_param_age_ms, fills_capacity, recent_rows)
    result, error = _core._run_strategy(config, data, instance, name, list(hooks),
                                        instance.param_values())
    if error is None:
        return result
    exc, hook, now_ns, events = error
    if not isinstance(exc, Exception):  # KeyboardInterrupt, SystemExit
        try:
            exc.result = result
        except AttributeError:  # pragma: no cover
            pass
        raise exc
    raise StrategyError(
        f"{name}.{hook} raised {type(exc).__name__}: {exc} (engine time {now_ns} ns, "
        f"after {events} engine events)",
        result=result, hook=hook, now_ns=now_ns, events=events) from exc


def _max_param_age_ms(config: "BacktestConfig", spec: "_hot_decl.HotSpec",
                      override: Optional[int]) -> int:
    from ._slow import decl as slow_decl

    if override is not None:
        if override < 0:
            raise ValueError("max_param_age_ms must be >= 0 (0 disables)")
        return int(override)
    if config.max_param_age_ms > 0:
        return int(config.max_param_age_ms)
    periods = spec.periods_ns()
    return slow_decl.default_max_param_age_ms(periods) if periods else 0


def _run_hot(config: "BacktestConfig", data: Any, instance: Strategy, name: str,
             spec: "_hot_decl.HotSpec", cache: bool, slow_delay_ms: float,
             max_param_age_ms: Optional[int], fills_capacity: Optional[int],
             recent_rows: int) -> "BacktestResult":
    from ._hot import compiler  # imports numba

    age = _max_param_age_ms(config, spec, max_param_age_ms)
    if age != config.max_param_age_ms:
        config = config.copy()
        config.max_param_age_ms = age
    param_values = instance.param_values()
    metadata = ""
    if config.journal_out:
        from ._slow import replay as slow_replay

        metadata = slow_replay.journal_metadata(type(instance), spec, param_values, age)

    slow: Optional[Dict[str, Any]] = None
    runner = None
    if spec.has_slow:
        from ._slow import runner as slow_runner

        if not (isinstance(slow_delay_ms, (int, float)) and math.isfinite(slow_delay_ms)
                and slow_delay_ms >= 0):
            raise ValueError(f"slow_delay_ms must be a finite number >= 0, got {slow_delay_ms!r}")
        if fills_capacity is not None and fills_capacity < 1:
            raise ValueError(f"fills_capacity must be >= 1, got {fills_capacity!r}")
        if recent_rows < 1:
            raise ValueError(f"recent_rows must be >= 1, got {recent_rows!r}")
        runner = slow_runner.SlowRunner(instance, spec)
        periods = spec.periods_ns()
        timeouts = [timeout for _, _, _, timeout in runner.methods]
        gap = max([min(periods), *timeouts]) if periods else slow_runner.IDLE_WAKE_NS
        slow = {"runner": runner, "delay_ns": int(round(slow_delay_ms * 1_000_000)),
                "fills_capacity": int(fills_capacity or 0), "longest_gap_ns": int(gap),
                "recent_rows": int(recent_rows),
                "snapshot_interval_ns": slow_runner.SNAPSHOT_INTERVAL_NS}

    try:
        result, error, _calls = compiler.run(config, data, instance, name, spec, cache,
                                             param_values, metadata, slow)
    except Exception as e:
        if runner is not None and runner.failed_method == "on_start":
            raise StrategyError(f"{name}.on_start raised {type(e).__name__}: {e}",
                                hook="on_start", slow_failure="exception") from e
        raise
    if runner is not None:
        result._set_slow_methods(runner.timing_rows())
    if error is not None:
        raise StrategyError(
            f"{name}.{error['hook']} {error['what']} (engine time {error['now_ns']} ns, after "
            f"{error['events']} engine events); kill switch tripped: {error['kill_reason']}",
            result=result, hook=error["hook"], now_ns=error["now_ns"], events=error["events"],
            status=error["status"], fail_code=error["fail_code"],
            kill_reason=error["kill_reason"])
    if slow is None or runner is None:
        return result

    events = slow["events"]
    exc = slow["error"]
    if exc is not None:
        method = runner.failed_method or "a slow method"
        if not isinstance(exc, Exception):  # KeyboardInterrupt, SystemExit
            try:
                exc.result = result
            except AttributeError:  # pragma: no cover
                pass
            raise exc
        raise StrategyError(
            f"{name}.{method} raised {type(exc).__name__}: {exc} (engine time {runner.now_ns} ns, "
            f"after {events} engine events); the slow tier stopped the run",
            result=result, hook=method, now_ns=runner.now_ns, events=events,
            slow_failure="exception") from exc
    if slow["failure"] != 0:
        from ._slow import abi as slow_abi

        what = slow_abi.FAILURE_NAMES.get(slow["failure"], str(slow["failure"]))
        detail = ""
        if slow["failure"] == slow_abi.FAILURE_FILLS_OVERFLOW:
            detail = (f": the fills ring ({runner.channel.fills_capacity} fills) was full; pass a "
                      "larger fills_capacity")
        raise StrategyError(
            f"{name}: the slow tier failed ({what}){detail}; the run stopped after {events} engine "
            "events", result=result, events=events, slow_failure=what)
    try:
        runner.stop(result.end_ts)
    except Exception as e:
        raise StrategyError(f"{name}.on_stop raised {type(e).__name__}: {e}", result=result,
                            hook="on_stop", slow_failure="exception") from e
    _warn_slow_delay(name, result, slow_delay_ms)
    return result


def _warn_slow_delay(name: str, result: "BacktestResult", slow_delay_ms: float) -> None:
    """A warning per @fastmm.every method whose median wall time is at least 1 ms and above
    slow_delay_ms: live, its publishes would take effect that much later than in the backtest."""
    for method, t in result.slow_methods.items():
        p50 = t["p50_ms"]
        if t["calls"] > 0 and p50 >= 1.0 and slow_delay_ms < p50:
            warnings.warn(
                f"fastmm: {name}.{method} takes {p50:.1f} ms of wall time (p50) but "
                f"slow_delay_ms={slow_delay_ms:g}, so its publishes take effect sooner than a live "
                f"session allows; pass slow_delay_ms={math.ceil(p50)} or more",
                RuntimeWarning, stacklevel=3)
