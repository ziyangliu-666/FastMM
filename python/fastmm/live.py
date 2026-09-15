"""fastmm.run_live: a strategy with hot hooks against live venues, in this process (ADR-0013).

The session is the one ``fastmm-live`` runs (venue threads, engine thread, journal, status file,
kill switch), started by the fastmm_live extension with the GIL released. It needs the live
runtime: ``pip install "fastmm-engine[live]"``.
"""

from __future__ import annotations

import os
import sys
from typing import Any, Dict, Mapping, Optional, Union

from ._hot.decl import HotCompileError, parse_period

__all__ = ["LIVE_MISSING", "run_live"]

LIVE_MISSING = ('fastmm.run_live needs the live runtime (fastmm_live); install it with: '
                'pip install "fastmm-engine[live]"')

EXIT_USAGE = 2
EXIT_CONFIG = 3


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


def run_live(strategy: Any, config: Union[str, "os.PathLike[str]"],
             params: Optional[Mapping[str, Any]] = None, *,
             duration: Union[None, str, int, float] = None, dry_run: bool = False,
             journal: Optional[str] = None, no_journal: bool = False,
             status: Optional[str] = None, no_status: bool = False, log: Optional[str] = None,
             record_raw: Optional[str] = None, allow_inline_secrets: bool = False) -> int:
    """Run a strategy with ``@fastmm.hot`` methods against the venues in ``config`` until SIGINT,
    SIGTERM, ``duration`` or a kill switch; returns the ``fastmm-live`` exit code.

    strategy: a fastmm.Strategy subclass or an unused instance. Parameters come from the
    configuration's ``[strategy.params]`` when ``[strategy] name`` names the class (``py:Class``,
    ``module:Class`` or ``Class``), then from ``params``.

    The keyword arguments are the ``fastmm-live`` options: ``duration`` in seconds or as ``"60s"``,
    ``journal``, ``status`` and ``log`` paths, ``record_raw`` directory.

    Configuration, parameter and compilation errors print a message and return 3. Raises
    ImportError without fastmm_live, TypeError for a strategy that is not a fastmm.Strategy and
    RuntimeError when a session already runs in this process.
    """
    try:
        import fastmm_live
    except ImportError as e:
        raise ImportError(LIVE_MISSING) from e
    from .strategy import _instance

    instance = _instance(strategy)
    cls = type(instance)
    name = cls.strategy_name()
    duration_ns = _duration_ns(duration)
    spec = cls._fastmm_hot
    if spec is None:
        _error(f"{name} has no @fastmm.hot methods; a strategy runs live only with hot hooks")
        return EXIT_CONFIG
    path = os.fspath(config)
    try:
        section = fastmm_live._live.load_config(path, allow_inline_secrets)
    except ValueError as e:
        _error(str(e))
        return EXIT_CONFIG
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
        return EXIT_CONFIG
    instance._fastmm_used = True

    from ._hot import compiler, meta  # imports numba

    try:
        # Never Numba's cache in a live session: a cached hook does not notice a change to FastMM's
        # own overloads.
        compiled = compiler.compiled(cls, spec, cache=False)
    except HotCompileError as e:
        _error(str(e))
        return EXIT_CONFIG
    program = compiled.program(instance)
    program["param_fields"] = meta.param_fields(spec)
    channel = fastmm_live._live.ParamChannel(program["param_fields"], program["param_bytes"],
                                             program["record"], len(section["instruments"]))
    # The publish handle of this session; publishes return False after it ends.
    instance._fastmm_live_params = channel
    options = {
        "duration_ns": duration_ns, "dry_run": dry_run, "journal": journal,
        "no_journal": no_journal, "status": status, "no_status": no_status, "log": log,
        "record_raw": record_raw, "allow_inline_secrets": allow_inline_secrets,
    }
    sys.stdout.flush()
    sys.stderr.flush()
    try:
        rc, error, _calls = fastmm_live._live.run(path, options, name, instance.param_values(),
                                                  program,
                                                  meta.format_meta(meta.session_meta(cls)),
                                                  channel)
    except BaseException:
        channel.close()  # no session consumes it
        raise
    if error is not None:
        hook = error["hook"] or compiled.timer_names[error["timer"]]
        what = compiler._WHAT.get(error["status"], f"returned status {error['status']}")
        if error["status"] == compiler.abi.FAILED:
            what = f"called ctx.fail({error['fail_code']})"
        _error(f"{name}.{hook} {what} (engine time {error['now_ns']} ns); kill switch tripped: "
               "StrategyError")
    return int(rc)
