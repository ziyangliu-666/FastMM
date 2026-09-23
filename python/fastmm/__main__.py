"""The ``fastmm`` command (also ``python -m fastmm``).

    fastmm run module:Class --config file.toml [options]    fastmm.run_live from a shell
    fastmm report <run-dir | session.fmj> [-o out.html]     the HTML report of a run

Before importing the strategy's module, ``run`` sets each of OPENBLAS_NUM_THREADS, OMP_NUM_THREADS
and MKL_NUM_THREADS that is not set to 1. numpy reads them when it loads, and ``python -m fastmm``
has loaded it already, so the interpreter starts again with them set.
"""

from __future__ import annotations

import argparse
import importlib
import os
import sys
from typing import List, Optional

THREAD_VARIABLES = ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS")


def _parser() -> argparse.ArgumentParser:
    prog = "fastmm" if os.path.basename(sys.argv[0] or "") == "fastmm" else "python -m fastmm"
    parser = argparse.ArgumentParser(prog=prog)
    commands = parser.add_subparsers(dest="command", required=True, metavar="command")
    from . import report as report_command

    report_command.add_arguments(commands.add_parser(
        "report", help=report_command.COMMAND_HELP,
        description=report_command.COMMAND_DESCRIPTION))
    run = commands.add_parser(
        "run", help="run a strategy with hot hooks against live venues",
        description="Run a fastmm.Strategy with @fastmm.hot methods against the venues in the "
                    "configuration, as fastmm-live does, and exit with its exit code.")
    run.add_argument("strategy", metavar="module:Class", help="the strategy class, importable")
    run.add_argument("--config", required=True, metavar="file",
                     help="engine / venue / strategy configuration")
    run.add_argument("--param", action="append", default=[], metavar="key=value",
                     help="strategy parameter override (repeatable)")
    run.add_argument("--duration", metavar="t", help="stop after t (e.g. 60s, 5m, 1500ms)")
    run.add_argument("--dry-run", action="store_true",
                     help="public market data only: no API keys, no orders")
    run.add_argument("--record-raw", metavar="dir",
                     help="append raw WebSocket frames to <dir>/<venue>-<channel>.jsonl")
    run.add_argument("--journal", metavar="path", help="write the session journal (.fmj) here")
    run.add_argument("--no-journal", action="store_true",
                     help="disable journaling even if [engine] journal = true")
    run.add_argument("--status", metavar="path", help="live status file for fastmm-top")
    run.add_argument("--no-status", action="store_true", help="do not publish live status")
    run.add_argument("--log", metavar="path", help="write the log to a file")
    run.add_argument("--allow-inline-secrets", action="store_true",
                     help="accept literal API secrets in the config file")
    run.add_argument("--slow-tier-timeout-ms", type=int, default=None, metavar="ms",
                     help="after the session stops, wait this long for the slow thread, then exit "
                          "(default 10000)")
    return parser


def _load_class(target: str) -> type:
    module_name, sep, qualname = target.partition(":")
    if not sep or not module_name or not qualname:
        raise ValueError(f"expected module:Class, got '{target}'")
    obj = importlib.import_module(module_name)
    for part in qualname.split("."):
        obj = getattr(obj, part)
    return obj  # type: ignore[return-value]


def main(argv: Optional[List[str]] = None) -> int:
    args_list = sys.argv[1:] if argv is None else argv
    parser = _parser()
    args = parser.parse_args(args_list)
    if args.command == "report":
        from .report import run_command

        return run_command(args)
    params = {}
    for item in args.param:
        key, sep, value = item.partition("=")
        if not sep or not key:
            parser.error(f"--param expects key=value, got '{item}'")
        params[key] = value
    if argv is None and any(v not in os.environ for v in THREAD_VARIABLES):
        for v in THREAD_VARIABLES:
            os.environ.setdefault(v, "1")
        os.execv(sys.executable, sys.orig_argv)

    from .live import EXIT_CONFIG, SLOW_TIER_TIMEOUT_MS, _run_live
    from .strategy import Strategy

    timeout_ms = args.slow_tier_timeout_ms
    if timeout_ms is None:
        timeout_ms = SLOW_TIER_TIMEOUT_MS
    if timeout_ms < 0:
        parser.error("--slow-tier-timeout-ms must be >= 0")
    if args.duration is not None:
        from ._hot.decl import parse_period

        try:
            parse_period(args.duration)
        except ValueError as e:
            parser.error(str(e))
    sys.path.insert(0, os.getcwd())
    try:
        cls = _load_class(args.strategy)
    except (ImportError, AttributeError, ValueError) as e:
        print(f"fastmm: cannot load strategy '{args.strategy}': {e}", file=sys.stderr)
        return EXIT_CONFIG
    if not (isinstance(cls, type) and issubclass(cls, Strategy)):
        print(f"fastmm: '{args.strategy}' is not a fastmm.Strategy subclass", file=sys.stderr)
        return EXIT_CONFIG
    rc, stuck = _run_live(cls, args.config, params, duration=args.duration, dry_run=args.dry_run,
                          journal=args.journal, no_journal=args.no_journal, status=args.status,
                          no_status=args.no_status, log=args.log, record_raw=args.record_raw,
                          allow_inline_secrets=args.allow_inline_secrets, fills_capacity=None,
                          recent_rows=4096, slow_tier_timeout_ms=timeout_ms)
    if stuck:  # the slow thread still runs: do not wait for it at interpreter exit
        print(f"fastmm: exiting with code {rc} without waiting for the slow thread",
              file=sys.stderr, flush=True)
        sys.stdout.flush()
        os._exit(rc)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
