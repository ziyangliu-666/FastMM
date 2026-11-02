"""Shadow sweeps from Python: variants of a live session's strategy over its journal as it is written.

A C++ strategy runs by its registered name. A ``fastmm.Strategy`` class with ``@fastmm.hot`` hooks
compiles a program per point, as ``fastmm.sweep`` does, and its points run on threads of their own
with the GIL released.
"""
from __future__ import annotations

import json
from typing import Any, Dict, List, Mapping, Optional, Sequence, Union

from . import _core

Variants = Union[Mapping[str, Sequence[Any]], Sequence[Mapping[str, Any]], None]


def shadow(journal: str, variants: Variants = None, strategy: Any = None, *, threads: int = 1,
           window_s: int = 60, settle_s: int = 10, max_lag_s: int = 300, report: str = "",
           status: str = "") -> Dict[str, Any]:
    """Follows one session's journal to its end, or to Ctrl-C, and returns the report as a dict.

    journal: the session's first part, written now or finished. variants: {name: [values]}, every
    combination, or a list of points [{name: value}]; the live point always runs first. strategy:
    None or a registered C++ strategy's name runs the journal's strategy; a ``fastmm.Strategy``
    subclass with hot hooks runs that class. threads: threads that step the points, at most 128
    points each. report, status: files rewritten as each window closes, as ``fastmm-shadow`` writes
    them. The report's keys are in docs/how-to/operations/shadow-sweeps.md.
    """
    grid: Any = variants if variants is not None else []
    if strategy is None or isinstance(strategy, str):
        text, stats = _core._shadow(journal, grid, threads, window_s, settle_s, max_lag_s, report,
                                    status)
    else:
        text, stats = _shadow_hot(journal, grid, strategy, threads, window_s, settle_s, max_lag_s,
                                  report, status)
    out: Dict[str, Any] = json.loads(text) if text else {}
    out.update({"reader": stats})
    return out


def _shadow_hot(journal: str, grid: Any, cls: type, threads: int, window_s: int, settle_s: int,
                max_lag_s: int, report: str, status: str) -> Any:
    from ._hot import compiler  # imports numba
    from .strategy import Strategy

    spec = getattr(cls, "_fastmm_hot", None)
    if not (isinstance(cls, type) and issubclass(cls, Strategy)) or spec is None:
        raise TypeError("shadow: strategy must be a C++ strategy's name or a fastmm.Strategy "
                        "class with @fastmm.hot hooks")
    params = _core._journal_params(journal)
    points: List[Dict[str, Any]] = list(_core._expand_grid(grid)) if grid else []
    hot = compiler.compiled(cls, spec, True)
    programs = []
    for point in [{}] + points:
        instance = cls()
        merged: Dict[str, Any] = dict(params)
        merged.update(point)
        try:
            instance.configure(merged)
        except ValueError as e:
            raise ValueError(f"{cls.strategy_name()}: {e}") from None
        instance._fastmm_used = True
        programs.append(hot.program(instance))
    options = {"threads": threads, "window_s": window_s, "settle_s": settle_s,
               "max_lag_s": max_lag_s, "report": report, "status": status}
    return _core._shadow_hot_strategy(journal, points, programs, "", options)
