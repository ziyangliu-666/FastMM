"""fastmm.run_live: a strategy with hot hooks against live venues, in this process (ADR-0013).

The session is the one ``fastmm-live`` runs (venue threads, engine thread, journal, status file,
kill switch), started by the fastmm_live extension with the GIL released. Slow methods run on one
Python thread beside it; the session's control thread watches that thread through the channel. It
needs the live runtime: ``pip install "fastmm-engine[live]"``.
"""

from __future__ import annotations

import os
import sys
import threading
import time
import traceback
import warnings
from typing import Any, Dict, Mapping, Optional, Tuple, Union

from ._hot.decl import HotCompileError, parse_period

__all__ = ["LIVE_MISSING", "run_live"]

LIVE_MISSING = ('fastmm.run_live needs the live runtime (fastmm_live); install it with: '
                'pip install "fastmm-engine[live]"')

EXIT_USAGE = 2
EXIT_CONFIG = 3
EXIT_SLOW_TIER = 7
SLOW_TIER_TIMEOUT_MS = 10_000
"""How long run_live waits for the slow thread after the session stops."""

# The publisher of the running session; a forked child disables it.
_session_publisher: Any = None


def _after_fork_in_child() -> None:
    publisher = _session_publisher
    if publisher is not None:
        publisher.disable()


if hasattr(os, "register_at_fork"):
    os.register_at_fork(after_in_child=_after_fork_in_child)


def _error(message: str) -> None:
    print(message if message.startswith("fastmm") else f"fastmm: {message}", file=sys.stderr,
          flush=True)


def _duration_ns(duration: Union[None, str, int, float]) -> int:
    if duration is None:
        return 0
    if isinstance(duration, str):
        return parse_period(duration)
    if isinstance(duration, bool) or not isinstance(duration, (int, float)):
        raise TypeError(f"fastmm: duration takes seconds or a string such as '60s', got "
                        f"{type(duration).__name__}")
    ns = int(round(duration * 1e9))
    if ns <= 0:
        raise ValueError(f"fastmm: duration must be positive, got {duration!r}")
    return ns


def _confine_calling_thread(reserved: Any) -> None:
    """Moves the calling thread off the CPUs the session pins, so threads it starts from here on (the
    slow thread, threads started in on_start, their BLAS pools) inherit that."""
    if not reserved or not hasattr(os, "sched_getaffinity"):
        return
    allowed = os.sched_getaffinity(0) - set(reserved)
    if allowed:
        os.sched_setaffinity(0, allowed)


def _thread_exists(tid: Optional[int]) -> bool:
    return tid is None or os.path.exists(f"/proc/self/task/{tid}")


class _SlowThread:
    """The slow thread of a session: runner.run_thread() until stop; the channel records failures."""

    def __init__(self, runner: Any, channel: Any, name: str) -> None:
        self.runner = runner
        self.channel = channel
        self.name = name
        self.stop = threading.Event()
        self.error: Optional[BaseException] = None
        self.thread = threading.Thread(target=self._main, name="fastmm-slow", daemon=True)

    def _main(self) -> None:
        from ._slow import abi
        from ._slow.runner import WallClock

        try:
            self.runner.run_thread(self.channel, WallClock(), self.stop, started=True)
        except BaseException as e:
            self.error = e
            where = self.runner.failed_method
            what = f"{self.name}.{where}" if where else f"{self.name}: the slow thread"
            print(f"fastmm: {what} raised {type(e).__name__}: {e}", file=sys.stderr, flush=True)
            traceback.print_exc()
            sys.stderr.flush()
        finally:
            if not self.stop.is_set():
                self.channel.fail(abi.FAILURE_THREAD_EXITED)

    def start(self) -> int:
        self.thread.start()
        return int(self.thread.native_id or 0)

    def finish(self, timeout_ms: int) -> bool:
        """Sets stop and waits for the thread (on_stop runs there); False when it did not return."""
        self.stop.set()
        if _thread_exists(self.thread.native_id):
            self.thread.join(timeout_ms / 1000.0)
        return not self.thread.is_alive() or not _thread_exists(self.thread.native_id)


def run_live(strategy: Any, config: Union[str, "os.PathLike[str]"],
             params: Optional[Mapping[str, Any]] = None, *,
             duration: Union[None, str, int, float] = None, dry_run: bool = False,
             journal: Optional[str] = None, no_journal: bool = False,
             status: Optional[str] = None, no_status: bool = False, log: Optional[str] = None,
             record_raw: Optional[str] = None, allow_inline_secrets: bool = False,
             fills_capacity: Optional[int] = None, recent_rows: int = 4096,
             slow_tier_timeout_ms: int = SLOW_TIER_TIMEOUT_MS) -> int:
    """Run a strategy with ``@fastmm.hot`` methods against the venues in ``config`` until SIGINT,
    SIGTERM, ``duration``, a kill switch or a slow-tier failure; returns the exit code.

    strategy: a fastmm.Strategy subclass or an unused instance. Parameters come from the
    configuration's ``[strategy.params]`` when ``[strategy] name`` names the class (``py:Class``,
    ``module:Class`` or ``Class``), then from ``params``.

    The keyword arguments up to ``allow_inline_secrets`` are the ``fastmm-live`` options:
    ``duration`` in seconds or as ``"60s"``, ``journal``, ``status`` and ``log`` paths,
    ``record_raw`` directory. ``fills_capacity`` and ``recent_rows`` size the slow methods' fills
    ring and recent rows as in run_backtest. ``slow_tier_timeout_ms``: how long to wait for the slow
    thread after the session stops; run_live warns and returns if it has not returned by then.

    Configuration, parameter and compilation errors and an exception in on_start print a message and
    return 3. Raises ImportError without fastmm_live, TypeError for a strategy that is not a
    fastmm.Strategy and RuntimeError when a session already runs in this process.
    """
    rc, _ = _run_live(strategy, config, params, duration=duration, dry_run=dry_run,
                      journal=journal, no_journal=no_journal, status=status, no_status=no_status,
                      log=log, record_raw=record_raw, allow_inline_secrets=allow_inline_secrets,
                      fills_capacity=fills_capacity, recent_rows=recent_rows,
                      slow_tier_timeout_ms=slow_tier_timeout_ms)
    return rc


def _run_live(strategy: Any, config: Union[str, "os.PathLike[str]"],
              params: Optional[Mapping[str, Any]], *, duration: Union[None, str, int, float],
              dry_run: bool, journal: Optional[str], no_journal: bool, status: Optional[str],
              no_status: bool, log: Optional[str], record_raw: Optional[str],
              allow_inline_secrets: bool, fills_capacity: Optional[int], recent_rows: int,
              slow_tier_timeout_ms: int) -> Tuple[int, bool]:
    """run_live; also returns whether the slow thread was still running when it returned."""
    global _session_publisher
    try:
        import fastmm_live
    except ImportError as e:
        raise ImportError(LIVE_MISSING) from e
    from .strategy import _instance

    instance = _instance(strategy)
    cls = type(instance)
    name = cls.strategy_name()
    duration_ns = _duration_ns(duration)
    if fills_capacity is not None and (isinstance(fills_capacity, bool) or fills_capacity < 1):
        raise ValueError(f"fills_capacity must be >= 1, got {fills_capacity!r}")
    if isinstance(recent_rows, bool) or recent_rows < 1:
        raise ValueError(f"recent_rows must be >= 1, got {recent_rows!r}")
    if isinstance(slow_tier_timeout_ms, bool) or slow_tier_timeout_ms < 0:
        raise ValueError(f"slow_tier_timeout_ms must be >= 0, got {slow_tier_timeout_ms!r}")
    spec = cls._fastmm_hot
    if spec is None:
        _error(f"{name} has no @fastmm.hot methods; a strategy runs live only with hot hooks")
        return EXIT_CONFIG, False
    live = fastmm_live._live
    live._check_can_run()
    path = os.fspath(config)
    try:
        section = live.load_config(path, allow_inline_secrets)
    except ValueError as e:
        _error(str(e))
        return EXIT_CONFIG, False
    merged: Dict[str, Any] = {}
    if section["params"]:
        if section["strategy"] in (name, f"{cls.__module__}:{cls.__qualname__}", cls.__qualname__):
            merged.update(section["params"])
        else:
            _error(f"note: ignoring [strategy.params] of '{section['strategy']}' for {name}")
    if params:
        merged.update(params)
    try:
        instance.configure(merged)
    except ValueError as e:
        _error(f"{name}: {e}")
        return EXIT_CONFIG, False
    instance._fastmm_used = True

    from ._hot import compiler, meta  # imports numba
    from ._slow import abi, decl, replay, runner as slow_runner
    from ._slow.publish import Publisher

    try:
        # Never Numba's cache in a live session: a cached hook does not notice a change to FastMM's
        # own overloads.
        compiled = compiler.compiled(cls, spec, cache=False)
    except HotCompileError as e:
        _error(str(e))
        return EXIT_CONFIG, False
    program = compiled.program(instance)
    program["param_fields"] = meta.param_fields(spec)
    param_values = instance.param_values()

    slow = spec.has_slow
    periods = spec.periods_ns()
    age = int(section["max_param_age_ms"])
    if age == 0 and slow and periods:
        age = decl.default_max_param_age_ms(periods)
    symbols = list(section["instruments"])
    runner = None
    if slow:
        runner = slow_runner.SlowRunner(instance, spec)
        timeouts = [timeout for _, _, _, timeout in runner.methods]
        gap = max([min(periods), *timeouts]) if periods else slow_runner.IDLE_WAKE_NS
        capacity = fills_capacity or live.slow_fills_capacity(section["orders_per_sec"], gap)
        channel = live.SlowChannel(symbols, capacity, recent_rows,
                                   slow_runner.SNAPSHOT_INTERVAL_NS)
    else:  # parameters only: the engine does not feed the channel
        channel = live.SlowChannel(symbols, 2, 2, slow_runner.SNAPSHOT_INTERVAL_NS)

    _confine_calling_thread(section["reserved_cpus"])
    thread: Optional[_SlowThread] = None
    if runner is not None:
        try:
            runner.start(channel, time.time_ns())  # on_start: before any venue connection
        except Exception as e:
            channel.close()
            where = runner.failed_method or "on_start"
            _error(f"{name}.{where} raised {type(e).__name__}: {e}")
            traceback.print_exc()
            return EXIT_CONFIG, False
        publisher = runner.publisher
        thread = _SlowThread(runner, channel, name)
    else:
        publisher = Publisher(cls, spec, instance, channel)
        instance.__dict__["_fastmm_publisher"] = publisher
    _session_publisher = publisher

    options = {
        "duration_ns": duration_ns, "dry_run": dry_run, "journal": journal,
        "no_journal": no_journal, "status": status, "no_status": no_status, "log": log,
        "record_raw": record_raw, "allow_inline_secrets": allow_inline_secrets,
        "max_param_age_ms": age,
    }
    metadata = replay.journal_meta(cls, param_values, age)
    sys.stdout.flush()
    sys.stderr.flush()
    tid = thread.start() if thread is not None else 0
    try:
        rc, error, _calls = live.run(path, options, name, param_values, program, metadata,
                                     channel, slow, tid)
    except BaseException:
        channel.close()  # no session consumes it
        if thread is not None:
            thread.finish(slow_tier_timeout_ms)
        _session_publisher = None
        raise
    returned = thread.finish(slow_tier_timeout_ms) if thread is not None else True
    _session_publisher = None
    if error is not None:
        hook = error["hook"] or compiled.timer_names[error["timer"]]
        what = compiler._WHAT.get(error["status"], f"returned status {error['status']}")
        if error["status"] == compiler.abi.FAILED:
            what = f"called ctx.fail({error['fail_code']})"
        _error(f"{name}.{hook} {what} (engine time {error['now_ns']} ns); kill switch tripped: "
               "StrategyError")
    failure = int(channel.failure()) if slow else 0
    if rc == EXIT_SLOW_TIER or failure != 0:
        _report_slow_failure(name, failure, runner, channel, thread)
    if not returned:
        warnings.warn(f"fastmm: the slow thread of {name} did not return within "
                      f"{slow_tier_timeout_ms} ms after the session stopped", RuntimeWarning,
                      stacklevel=3)
    return int(rc), not returned


def _report_slow_failure(name: str, failure: int, runner: Any, channel: Any,
                         thread: Optional[_SlowThread]) -> None:
    from ._slow import abi

    if failure == abi.FAILURE_EXCEPTION:
        if thread is None or thread.error is None:
            _error(f"{name}: a slow method raised")
        return  # the slow thread printed the exception when it happened
    if failure == abi.FAILURE_TIMEOUT:
        method = (runner.current or runner.overran) if runner is not None else None
        timeout = ""
        for m, _, _, timeout_ns in runner.methods if runner is not None else ():
            if m == method:
                timeout = f" ({timeout_ns / 1e9:g} s)"
        what = f"{name}.{method}" if method else f"{name}: a slow method"
        _error(f"{what} ran past its timeout{timeout}; the slow tier stopped the session")
    elif failure == abi.FAILURE_FILLS_OVERFLOW:
        _error(f"{name}: the fills ring ({channel.fills_capacity} fills) was full; the slow tier "
               "stopped the session; pass a larger fills_capacity")
    elif failure == abi.FAILURE_THREAD_EXITED:
        _error(f"{name}: the slow thread ended while the session ran; the slow tier stopped the "
               "session")
