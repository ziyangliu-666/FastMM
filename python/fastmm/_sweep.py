"""fastmm.sweep and fastmm.walk_forward for registered C++ strategies and fastmm.Strategy classes.

A C++ strategy's points run on the C++ thread pool with the GIL released (``_core.sweep``). A
Python strategy needs the interpreter, so its points run in worker processes forked from this one:
each worker inherits the config, the data and the class (a class defined in ``__main__`` or a
notebook works), runs ``fastmm.run_backtest`` and sends the BacktestResult back pickled. Where
``fork`` is not available, or with ``threads=1``, the points run one after another in this process.
"""

from __future__ import annotations

import multiprocessing
import os
from typing import Any, Dict, List, Optional, Sequence, Tuple

from . import _core

# In a worker process: the work of its pool (config, data, class, [(params, window)]).
_JOB: Optional[Tuple[Any, Any, type, List[Tuple[Dict[str, Any], Any]]]] = None


def _set_job(job: Tuple[Any, Any, type, List[Tuple[Dict[str, Any], Any]]]) -> None:
    global _JOB
    _JOB = job


def _strategy_class(config: Any, strategy: Any) -> Optional[type]:
    """None for a registered strategy name (or None naming one), the class for a fastmm.Strategy
    class or a ``module:Class`` name."""
    from .strategy import Strategy, _named

    strategy = _named(config, strategy)
    if strategy is None or isinstance(strategy, str):
        return None
    if isinstance(strategy, type) and issubclass(strategy, Strategy):
        return strategy
    if isinstance(strategy, Strategy):
        raise TypeError("strategy: pass the fastmm.Strategy class, not an instance; every point "
                        "runs a fresh instance")
    raise TypeError("strategy must be a registered strategy name or a fastmm.Strategy subclass, "
                    f"got {type(strategy).__name__}")


def _point_config(config: Any) -> Any:
    # Every point restores the state_file as it is now and none writes its end state back.
    cfg = _core._shared_start(config)
    cfg.output_dir = ""
    cfg.journal_out = ""
    return cfg


def _run_one(cfg: Any, data: Any, cls: type, params: Dict[str, Any], window: Any) -> Any:
    from .strategy import run_backtest

    if window is not None:
        data = _core._TimeSlice(data, window[0], window[1])
    return run_backtest(cfg, data, strategy=cls, params=params)


def _run_task(i: int) -> Any:
    assert _JOB is not None
    cfg, data, cls, tasks = _JOB
    params, window = tasks[i]
    return _run_one(cfg, data, cls, params, window)


def _workers(threads: int, n: int) -> int:
    w = threads if threads > 0 else (os.cpu_count() or 1)
    return max(1, min(w, n, 32))


def _run_points(config: Any, data: Any, cls: type,
                tasks: List[Tuple[Dict[str, Any], Any]], threads: int) -> List[Any]:

    cfg = _point_config(config)
    workers = _workers(threads, len(tasks))
    if workers == 1 or "fork" not in multiprocessing.get_all_start_methods():
        return [_run_one(cfg, data, cls, p, w) for p, w in tasks]
    from concurrent.futures import ProcessPoolExecutor

    # A forked worker inherits the initializer's arguments as they are: nothing is pickled on the
    # way in, only the results on the way back.
    with ProcessPoolExecutor(max_workers=workers, mp_context=multiprocessing.get_context("fork"),
                             initializer=_set_job, initargs=((cfg, data, cls, tasks),)) as pool:
        return list(pool.map(_run_task, range(len(tasks))))


def sweep(config: Any, grid: Dict[str, Sequence[Any]], data: Any = None, strategy: Any = None,
          threads: int = 0, verify: int = 0) -> List[Tuple[Dict[str, Any], Any]]:
    """Cartesian parameter sweep. grid: {param: [values]}. Returns [(params, BacktestResult)] in
    grid order, first parameter varying slowest.

    data: the forms run_backtest takes. strategy: None (config.strategy), a registered C++
    strategy name, a fastmm.Strategy subclass or a ``module:Class`` name. Under the queue fill
    models (l2_queue, l3_queue) the points share passes over the data, each with the result
    run_backtest gives it: a C++ strategy or a class with @fastmm.hot hooks runs on threads with
    the GIL released. A plain Python strategy, or any strategy under the matching model, runs one
    backtest per point (a Python class in worker processes forked from this one, so it may live in
    ``__main__`` or a notebook). threads: workers; <= 0 uses every CPU, 1 runs in this thread.

    verify: run that many points, spread over the grid, again as backtests of their own and raise
    RuntimeError naming the first difference (fill, order, equity bar, count or outbound hash) if
    one gives another result.
    """
    cls = _strategy_class(config, strategy)
    if cls is None:
        out = _core.sweep(config, grid, data, strategy, threads)
    else:
        points = _core._expand_grid(grid)
        if _shares_passes(config, cls):
            results = _run_hot_points(_point_config(config), data, cls, points, threads)
        else:
            results = _run_points(config, data, cls, [(p, None) for p in points], threads)
        out = list(zip(points, results))
    _verify(config, data, strategy, out, verify)
    return out


def _shares_passes(config: Any, cls: type) -> bool:
    """A Strategy class whose points share passes over the data: hot hooks, a queue fill model."""
    return cls._fastmm_hot is not None and config.fill_model in ("l2_queue", "l3_queue")


def _run_hot_points(config: Any, data: Any, cls: type, points: List[Dict[str, Any]],
                    threads: int) -> List[Any]:
    from .strategy import _run_hot_points as run

    return run(config, data, cls, points, threads)


def _verify(config: Any, data: Any, strategy: Any, out: List[Tuple[Dict[str, Any], Any]],
            n: int) -> None:
    from .strategy import run_backtest

    if n <= 0 or not out:
        return
    cfg = _point_config(config)
    for k in range(min(n, len(out))):
        params, result = out[k * len(out) // min(n, len(out))]
        alone = run_backtest(cfg, data, strategy=strategy, params=params)
        diff = _core._result_difference(alone, result)
        if diff:
            raise RuntimeError(f"fastmm.sweep: the point {params} gives another result alone: "
                               f"{diff}")


def walk_forward(config: Any, grid: Dict[str, Sequence[Any]], folds: int, data: Any = None,
                 metric: str = "net_pnl", strategy: Any = None, threads: int = 0) -> Dict[str, Any]:
    """Walk-forward sweep: the grid runs on `folds` consecutive time slices of `data`, and the best
    point of fold i-1 by `metric` (net_pnl, realized_pnl, sharpe_bar, spread_captured_bps) is
    scored on fold i.

    Returns a dict: metric, folds (start_ts, end_ts, best, chosen, in_sample, out_of_sample,
    hindsight, scores, points), mean_in_sample, mean_out_of_sample, mean_hindsight, choice_changes
    and table (the printable report). folds=1 runs exactly fastmm.sweep. The synthetic market needs
    fill_model='l2_queue' for folds > 1. strategy and threads are as in fastmm.sweep, each fold's
    points sharing their passes as a sweep's do.
    """
    cls = _strategy_class(config, strategy)
    if cls is None:
        return _core.walk_forward(config, grid, folds, data, metric, strategy, threads)
    edges = _core._fold_edges(config, data, folds, metric)
    points = _core._expand_grid(grid)
    if folds == 1:
        windows: List[Any] = [None]
        spans = [(0, 0)]
    else:
        windows = [(edges[i], edges[i + 1]) for i in range(folds)]
        spans = windows
    if _shares_passes(config, cls):
        results: List[Any] = []
        for w in windows:
            fold = data if w is None else _core._TimeSlice(data, w[0], w[1])
            results += _run_hot_points(_point_config(config), fold, cls, points, threads)
    else:
        results = _run_points(config, data, cls, [(p, w) for w in windows for p in points], threads)
    n = len(points)
    per_fold = [(spans[i][0], spans[i][1], results[i * n:(i + 1) * n]) for i in range(len(windows))]
    return _core._walk_forward_report(grid, metric, per_fold)
