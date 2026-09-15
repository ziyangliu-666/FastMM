"""Declarations of hot strategies: @fastmm.hot, fastmm.State and the checks at class creation.

Nothing here imports numba, so defining a class stays cheap; compiler.py compiles the hooks when a
run starts.
"""

from __future__ import annotations

import ast
import importlib.util
import inspect
import math
import textwrap
from decimal import ROUND_HALF_UP, Decimal, InvalidOperation
from typing import Any, Callable, Dict, List, Mapping, Optional, Tuple

import numpy as np

from .._slow import decl as _slow_decl
from . import abi

EVENT_HOOKS: Tuple[str, ...] = ("on_book", "on_fill", "on_quoting", "on_connection", "on_params")
MAX_PARAMS = 32  # a ParamUpdate's fields and the journal's parameter table
# Names that Strategy.publish(inst=None, **values) needs for itself.
RESERVED_NAMES = frozenset({"publish", "inst"})
# Methods on Numba records (compiler.py). A record field with one of these names is unreachable.
CTX_METHODS = frozenset({"quote", "quote_raw", "bid", "ask", "bid_raw", "ask_raw", "clear", "pull",
                         "uncross", "keep_passive", "fail"})

NUMBA_MISSING = 'fastmm: hot hooks need numba; install it with: pip install "fastmm-engine[hot]"'

_UNIT_NS = {
    "ns": 1,
    "us": 1_000,
    "ms": 1_000_000,
    "s": 1_000_000_000,
    "m": 60_000_000_000,
    "h": 3_600_000_000_000,
}
_SCALE = 100_000_000


class HotCompileError(TypeError):
    """A hot hook does not compile in Numba's nopython mode or fails the IR check."""


def parse_period(every: Any) -> int:
    """'100ms' -> 100_000_000 ns. Units: ns, us, ms, s, m, h."""
    if not isinstance(every, str):
        raise TypeError(f"fastmm: every= takes a string such as '100ms', got {type(every).__name__}")
    text = every.strip()
    for unit in sorted(_UNIT_NS, key=len, reverse=True):
        if text.endswith(unit):
            number = text[: -len(unit)].strip()
            try:
                ns = Decimal(number) * _UNIT_NS[unit]
            except InvalidOperation:
                break
            if ns != ns.to_integral_value() or ns <= 0:
                break
            return int(ns)
    raise ValueError(f"fastmm: every={every!r} is not a positive period such as '100ms' or '1s' "
                     "(units ns, us, ms, s, m, h)")


class HotHook:
    """A method marked with @fastmm.hot; the engine thread calls its compiled form."""

    def __init__(self, fn: Callable[..., Any], every: Optional[str]) -> None:
        if not inspect.isfunction(fn):
            raise TypeError("fastmm: @fastmm.hot decorates a function defined with def")
        self.fn = fn
        self.every = every
        self.period_ns = None if every is None else parse_period(every)
        self.name = fn.__name__
        self.__doc__ = fn.__doc__
        self.__wrapped__ = fn

    def __set_name__(self, owner: type, name: str) -> None:
        self.name = name

    def __get__(self, obj: Any, objtype: Any = None) -> "HotHook":
        return self

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        raise TypeError(f"fastmm: {self.name} is a hot hook; the engine calls its compiled form")

    def __repr__(self) -> str:
        every = "" if self.every is None else f"(every={self.every!r})"
        return f"<fastmm.hot{every} {self.fn.__qualname__}>"


def hot(fn: Optional[Callable[..., Any]] = None, *, every: Optional[str] = None) -> Any:
    """Marks a strategy method as a hot hook: ``@fastmm.hot`` for on_book, on_fill, on_quoting and
    on_connection, ``@fastmm.hot(every="100ms")`` for a timer hook. Every hot hook takes
    ``(self, ctx, book)``."""
    if fn is None:
        if every is not None:
            parse_period(every)  # report a bad period at the decorator
        return lambda f: HotHook(f, every)
    if every is not None:
        raise TypeError("fastmm: use @fastmm.hot(every=...) with the period as a keyword")
    return HotHook(fn, None)


class State:
    """A per-instrument value that hot hooks read and write and that persists between calls. The
    type comes from the default: bool, int (int64) or float."""

    def __init__(self, default: Any, *, doc: str = "") -> None:
        if isinstance(default, (bool, np.bool_)):
            self.code = "?"
            default = bool(default)
        elif isinstance(default, (int, np.integer)):
            self.code = "i8"
            default = int(default)
            if not -(2**63) <= default < 2**63:
                raise ValueError(f"fastmm: State default {default} does not fit in int64")
        elif isinstance(default, (float, np.floating)):
            self.code = "f8"
            default = float(default)
        else:
            raise TypeError(f"fastmm: State default must be bool, int or float, got "
                            f"{type(default).__name__}")
        self.default = default
        self.doc = doc
        self.name = ""

    def __set_name__(self, owner: type, name: str) -> None:
        self.name = name

    def __get__(self, obj: Any, objtype: Any = None) -> "State":
        return self

    def __repr__(self) -> str:
        return f"State({self.default!r}, doc={self.doc!r})"


class HotSpec:
    """What class creation found: hooks, timer hooks, parameters, State fields, @fastmm.every methods
    and on_start / on_stop, in declaration order (bases first)."""

    def __init__(self, qualname: str, hooks: Dict[str, HotHook], timers: Dict[str, HotHook],
                 params: Dict[str, Any], states: Dict[str, State],
                 slow: Optional[Dict[str, Any]] = None,
                 lifecycle: Optional[Dict[str, Any]] = None) -> None:
        self.qualname = qualname
        self.hooks = hooks
        self.timers = timers
        self.params = params
        self.states = states
        self.slow = slow or {}
        self.lifecycle = lifecycle or {}

    @property
    def has_slow(self) -> bool:
        """Whether the class has slow methods (@fastmm.every, on_start or on_stop)."""
        return bool(self.slow or self.lifecycle)

    def periods_ns(self) -> List[int]:
        """The periods of the @fastmm.every methods."""
        out = []
        for fn in self.slow.values():
            mark = _slow_decl.every_of(fn)
            if mark is not None:
                out.append(mark.period_ns)
        return out


    def fields(self) -> List[Tuple[str, str]]:
        """The `self` record: parameters (float ones with a `_raw` int64 twin), then State."""
        out: List[Tuple[str, str]] = []
        for name, p in self.params.items():
            if p.type is bool:
                out.append((name, "?"))
            elif p.type is int:
                out.append((name, "i8"))
            else:
                out.append((name, "f8"))
                out.append((name + "_raw", "i8"))
        out.extend((name, s.code) for name, s in self.states.items())
        return out

    def record_dtype(self) -> Tuple[np.dtype, int]:
        """(dtype, parameter block bytes). The parameter block is every byte before the first State
        field."""
        fields = self.fields()
        if not fields:
            fields = [("_fastmm_empty", "u1")]
        dt = np.dtype(fields, align=True)
        if self.states:
            first = next(iter(self.states))
            param_bytes = dt.fields[first][1]
        elif self.params:
            param_bytes = dt.itemsize
        else:
            param_bytes = 0
        return dt, param_bytes

    def initial_record(self, instance: Any) -> bytes:
        """One record with the instance's parameter values and the State defaults."""
        dt, _ = self.record_dtype()
        rec = np.zeros(1, dt)
        for name, p in self.params.items():
            value = getattr(instance, name)
            rec[name] = value
            if p.type is float:
                rec[name + "_raw"] = fixed_raw(value, name)
        for name, s in self.states.items():
            rec[name] = s.default
        return rec.tobytes()


def fixed_raw(value: float, name: str) -> int:
    """A float as 1e-8 fixed point, rounded to the nearest raw unit (half away from zero) from its
    shortest decimal form, so 0.002 is exactly 200000."""
    if not math.isfinite(value):
        raise ValueError(f"fastmm: parameter '{name}' is not finite")
    raw = int((Decimal(repr(float(value))) * _SCALE).to_integral_value(rounding=ROUND_HALF_UP))
    if not -(2**63) <= raw < 2**63:
        raise ValueError(f"fastmm: parameter '{name}' = {value!r} does not fit in 1e-8 fixed point")
    return raw


def _positional(fn: Callable[..., Any]) -> Optional[List[str]]:
    try:
        sig = inspect.signature(fn)
    except (TypeError, ValueError):  # pragma: no cover
        return None
    names = []
    for p in sig.parameters.values():
        if p.kind not in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD) or p.default is not p.empty:
            return None
        names.append(p.name)
    return names


def _lint_parameter_writes(qualname: str, hook: HotHook, params: Mapping[str, Any],
                           raw_names: Mapping[str, str]) -> None:
    """TypeError for `self.<parameter> = ...` in a hook: the engine copies the parameters into
    `self` before every call, so the assignment would not persist."""
    try:
        lines, first = inspect.getsourcelines(hook.fn)
    except (OSError, TypeError):  # no source (exec, REPL): nothing to lint
        return
    try:
        tree = ast.parse(textwrap.dedent("".join(lines)))
    except SyntaxError:  # pragma: no cover
        return
    fdef = next((n for n in tree.body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))),
                None)
    if fdef is None or not fdef.args.args:
        return
    self_name = fdef.args.args[0].arg
    for node in ast.walk(fdef):
        if (isinstance(node, ast.Attribute) and isinstance(node.ctx, ast.Store)
                and isinstance(node.value, ast.Name) and node.value.id == self_name):
            param = node.attr if node.attr in params else raw_names.get(node.attr)
            if param is not None:
                line = first + node.lineno - 1
                raise TypeError(
                    f"fastmm: {qualname}.{hook.name} assigns parameter '{node.attr}' (line "
                    f"{line}); the engine copies the parameters into self before every call, so "
                    "the value would not persist. Use a fastmm.State field.")


def check_class(cls: type, hook_table: Mapping[str, Tuple[str, ...]],
                param_type: type) -> Optional[HotSpec]:
    """The HotSpec of a class with hot hooks, None for a class without. Raises TypeError for a class
    that mixes hot hooks with fastmm.Strategy hooks other than on_start and on_stop, a bad hot hook
    or slow method, slow methods without hot hooks, or a name used by two fields, and ImportError
    when numba is missing."""
    hooks: Dict[str, HotHook] = {}
    timers: Dict[str, HotHook] = {}
    params: Dict[str, Any] = {}
    states: Dict[str, State] = {}
    plain: Dict[str, type] = {}
    slow: Dict[str, Any] = {}
    lifecycle: Dict[str, Any] = {}
    kinds: Dict[str, str] = {}
    for klass in reversed(cls.__mro__):
        for name, attr in vars(klass).items():
            for table in (hooks, timers, params, states, plain, slow, lifecycle):
                table.pop(name, None)
            if isinstance(attr, HotHook):
                (hooks if attr.every is None else timers)[name] = attr
            elif isinstance(attr, param_type) or isinstance(attr, State):
                kind = "State" if isinstance(attr, State) else "Param"
                if kinds.get(name, kind) != kind:
                    raise TypeError(f"fastmm: {cls.__qualname__}.{name} is declared both as a Param "
                                    "and as a State; parameters and State share one namespace")
                kinds[name] = kind
                (states if kind == "State" else params)[name] = attr
            elif _slow_decl.every_of(attr) is not None:
                slow[name] = attr
            elif name in hook_table and not (klass.__module__ == "fastmm.strategy"
                                             and klass.__qualname__ == "Strategy"):
                if name in _slow_decl.LIFECYCLE:
                    lifecycle[name] = attr
                plain[name] = klass
    if not hooks and not timers:
        if slow:
            raise TypeError(f"fastmm: {cls.__qualname__} has @fastmm.every methods but no "
                            "@fastmm.hot hooks; slow methods run beside hot hooks")
        return None
    for name in lifecycle:
        plain.pop(name, None)

    qualname = cls.__qualname__
    if importlib.util.find_spec("numba") is None:
        raise ImportError(NUMBA_MISSING)
    base_hooks = {n for n, k in plain.items() if k.__qualname__ != "Strategy"}
    if base_hooks:
        names = ", ".join(sorted(base_hooks))
        raise TypeError(f"fastmm: {qualname} has @fastmm.hot methods and defines {names}; a class "
                        "with hot hooks cannot define the hooks of fastmm.Strategy")
    for name, h in hooks.items():
        if name not in EVENT_HOOKS:
            raise TypeError(f"fastmm: {qualname}.{name}: @fastmm.hot applies to "
                            f"{', '.join(EVENT_HOOKS)}; use @fastmm.hot(every=...) for a timer hook")
    for name in timers:
        if name in EVENT_HOOKS or name in hook_table:
            raise TypeError(f"fastmm: {qualname}.{name}: a timer hook needs a name that is not a "
                            "hook name")
    if len(timers) > abi.TIMER_LIMIT:
        raise TypeError(f"fastmm: {qualname} has {len(timers)} timer hooks; at most "
                        f"{abi.TIMER_LIMIT}")
    for name, h in {**hooks, **timers}.items():
        if _positional(h.fn) is None or len(_positional(h.fn) or ()) != 3:
            raise TypeError(f"fastmm: {qualname}.{name} has the wrong signature; a hot hook is "
                            f"{name}(self, ctx, book)")

    for name, fn in lifecycle.items():
        if not inspect.isfunction(fn) or len(_positional(fn) or ()) != 2:
            raise TypeError(f"fastmm: {qualname}.{name} has the wrong signature; expected "
                            f"{name}(self, ctx)")
    for name, fn in slow.items():
        if name in hook_table or name in EVENT_HOOKS:
            raise TypeError(f"fastmm: {qualname}.{name}: a @fastmm.every method needs a name that "
                            "is not a hook name")
        if len(_positional(fn) or ()) != 2:
            raise TypeError(f"fastmm: {qualname}.{name} has the wrong signature; a slow method is "
                            f"{name}(self, ctx)")
    if len(params) > MAX_PARAMS:
        raise TypeError(f"fastmm: {qualname} has {len(params)} parameters; a hot strategy has at "
                        f"most {MAX_PARAMS}")

    for name in (*params, *states):
        if name in CTX_METHODS:
            raise TypeError(f"fastmm: {qualname}.{name}: '{name}' is the name of a ctx method, "
                            "which Numba would resolve instead of the field; rename it")
        if name in RESERVED_NAMES:
            raise TypeError(f"fastmm: {qualname}.{name}: '{name}' is taken by "
                            "Strategy.publish(inst=None, **values); rename it")

    raw_names: Dict[str, str] = {}
    for name, p in params.items():
        if p.type is float:
            raw_names[name + "_raw"] = name
    for raw, name in raw_names.items():
        if raw in params or raw in states:
            raise TypeError(f"fastmm: {qualname}.{raw} clashes with the raw twin of parameter "
                            f"'{name}'; parameters and State share one namespace")
    for h in (*hooks.values(), *timers.values()):
        _lint_parameter_writes(qualname, h, params, raw_names)
    return HotSpec(qualname, hooks, timers, params, states, slow, lifecycle)
