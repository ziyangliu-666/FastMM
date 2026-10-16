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
    cfg = config.copy()
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
          threads: int = 0) -> List[Tuple[Dict[str, Any], Any]]:
    """Cartesian parameter sweep. grid: {param: [values]}. Returns [(params, BacktestResult)] in
    grid order, first parameter varying slowest.

    data: the forms run_backtest takes. strategy: None (config.strategy), a registered C++
    strategy name, a fastmm.Strategy subclass or a ``module:Class`` name. A C++ strategy runs on a
    thread pool with the GIL released, every worker with its own cursor over `data`. A Python
    strategy (plain or hot hooks) runs one fresh instance per point in worker processes forked
    from this one, so the class may live in ``__main__`` or a notebook; results are the same as
    run_backtest's for each point. threads: workers (threads or processes); <= 0 uses every CPU, 1
    runs the points one after another in this process.
    """
    cls = _strategy_class(config, strategy)
    if cls is None:
        return _core.sweep(config, grid, data, strategy, threads)
    points = _core._expand_grid(grid)
    results = _run_points(config, data, cls, [(p, None) for p in points], threads)
    return list(zip(points, results))


def walk_forward(config: Any, grid: Dict[str, Sequence[Any]], folds: int, data: Any = None,
                 metric: str = "net_pnl", strategy: Any = None, threads: int = 0) -> Dict[str, Any]:
    """Walk-forward sweep: the grid runs on `folds` consecutive time slices of `data`, and the best
    point of fold i-1 by `metric` (net_pnl, realized_pnl, sharpe_bar, spread_captured_bps) is
    scored on fold i.

    Returns a dict: metric, folds (start_ts, end_ts, best, chosen, in_sample, out_of_sample,
    hindsight, scores, points), mean_in_sample, mean_out_of_sample, mean_hindsight, choice_changes
    and table (the printable report). folds=1 runs exactly fastmm.sweep. The synthetic market needs
    fill_model='l2_queue' for folds > 1. strategy and threads are as in fastmm.sweep: a
    fastmm.Strategy class runs every (fold, point) in worker processes.
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
    tasks = [(p, w) for w in windows for p in points]
    results = _run_points(config, data, cls, tasks, threads)
    n = len(points)
    per_fold = [(spans[i][0], spans[i][1], results[i * n:(i + 1) * n]) for i in range(len(windows))]
    return _core._walk_forward_report(grid, metric, per_fold)
