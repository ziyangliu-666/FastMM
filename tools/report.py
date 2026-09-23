#!/usr/bin/env python3
"""Write the HTML run report from this checkout, without installing the Python package.

    python3 tools/report.py runs/backtest                 # -> runs/backtest/report.html
    python3 tools/report.py runs/20260923-101500          # a live run directory or a .fmj

The report is python/fastmm/report.py (`fastmm report` once the wheel is installed); this loads
that module straight from the source tree, so scripts/run-sim.sh and the quickstart can end with
a report on a machine that only built the C++ programs. It needs nothing but the standard
library.
"""
import importlib.util
import sys
import types
from pathlib import Path

PACKAGE = Path(__file__).resolve().parent.parent / "python" / "fastmm"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def main(argv) -> int:
    # A stub `fastmm` package: report.py imports `._fmj` relative to it, and nothing else.
    package = types.ModuleType("fastmm")
    package.__path__ = [str(PACKAGE)]
    sys.modules["fastmm"] = package
    _load("fastmm._fmj", PACKAGE / "_fmj.py")
    return _load("fastmm.report", PACKAGE / "report.py").main(argv)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
