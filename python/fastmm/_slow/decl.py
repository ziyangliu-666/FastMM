"""@fastmm.every: the slow methods of a strategy with hot hooks (ADR-0013, section 1)."""

from __future__ import annotations

import inspect
import math
from typing import Any, Callable, Iterable, NamedTuple, Optional, TypeVar

F = TypeVar("F", bound=Callable[..., Any])

# Plain-Python methods of a hot strategy that the slow thread calls, besides @fastmm.every methods.
LIFECYCLE = ("on_start", "on_stop")
DEFAULT_TIMEOUT = "10s"


class Every(NamedTuple):
    """What @fastmm.every records on a method (``fn._fastmm_every``)."""

    period: str
    timeout: str
    period_ns: int
    timeout_ns: int


def every(period: str, *, timeout: str = DEFAULT_TIMEOUT) -> Callable[[F], F]:
    """Runs the decorated method, ``name(self, ctx)``, on the slow thread once per `period` of session
    time, first when the session starts. In a live session a call that runs longer than `timeout`
    stops the session. Units: ns, us, ms, s, m, h."""
    from .._hot.decl import parse_period

    if callable(period):
        raise TypeError("fastmm: use @fastmm.every('1s') with a period")
    period_ns = parse_period(period)
    try:
        timeout_ns = parse_period(timeout)
    except (TypeError, ValueError) as e:
        raise type(e)(str(e).replace("every=", "timeout=", 1)) from None

    def mark(fn: F) -> F:
        if not inspect.isfunction(fn):
            raise TypeError("fastmm: @fastmm.every decorates a function defined with def")
        fn._fastmm_every = Every(period, timeout, period_ns, timeout_ns)  # type: ignore[attr-defined]
        return fn

    return mark


def every_of(attr: Any) -> Optional[Every]:
    """The Every of a method marked with @fastmm.every, else None."""
    if not inspect.isfunction(attr):
        return None
    mark = getattr(attr, "_fastmm_every", None)
    return mark if isinstance(mark, Every) else None


def default_max_param_age_ms(periods_ns: Iterable[int]) -> int:
    """max_param_age_ms of a strategy with slow methods: 3 times the shortest period, at least
    1000 ms."""
    shortest = min(periods_ns)
    return max(1000, math.ceil(3 * shortest / 1_000_000))
