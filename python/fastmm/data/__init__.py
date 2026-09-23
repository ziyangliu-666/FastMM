"""Market data for backtests: the fetch/cache layer and the numpy row helpers.

The decoders are C++ (`fastmm::bt`, one per registered `--data` source); this side downloads
what they read. `python -m fastmm.data --help` is the command line.
"""

from .cache import Cache, DownloadError, cache_root, sha256_file
from .rows import load_csv

__all__ = ["Cache", "DownloadError", "cache_root", "load_csv", "sha256_file"]


def __getattr__(name: str):  # noqa: ANN202
    # The per-archive fetchers load on first use; only `fetch` needs them.
    if name in ("binance", "tardis"):
        import importlib

        return importlib.import_module("." + name, __name__)
    raise AttributeError(f"module 'fastmm.data' has no attribute {name!r}")
