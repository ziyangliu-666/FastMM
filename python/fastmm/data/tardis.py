"""Tardis.dev normalized datasets (https://datasets.tardis.dev) as a fetcher for the `tardis`
market-data source.

The first day of every month is free for every exchange, symbol and data type; any other day
needs an API key in ``$TARDIS_API_KEY``. Files are gzipped CSV.

    fetch("binance-futures", "BTCUSDT", ["2026-09-01"])

The server rejects range requests, so an interrupted download restarts instead of resuming.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Iterable, List, Optional, Sequence

if __package__:
    from .cache import Cache, DownloadError, stderr_progress
else:  # python3 python/fastmm/data/tardis.py
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from cache import Cache, DownloadError, stderr_progress  # type: ignore[no-redef]

BASE_URL = "https://datasets.tardis.dev/v1"

# The data types this repository can decode.
KINDS = ("incremental_book_L2", "trades")


def daily_key(exchange: str, kind: str, symbol: str, date: str, ext: str = ".csv.gz") -> str:
    """Cache key of one dataset file, mirroring the download URL:
    ``tardis/v1/binance-futures/trades/2026/09/01/BTCUSDT.csv.gz``."""
    year, month, day = date.split("-")
    return f"tardis/v1/{exchange}/{kind}/{year}/{month}/{day}/{symbol}{ext}"


def url_for(exchange: str, kind: str, symbol: str, date: str) -> str:
    year, month, day = date.split("-")
    return f"{BASE_URL}/{exchange}/{kind}/{year}/{month}/{day}/{symbol}.csv.gz"


def fetch(
    exchange: str,
    symbol: str,
    dates: Sequence[str],
    *,
    kinds: Optional[Iterable[str]] = None,
    cache: Optional[Cache] = None,
    extract: bool = True,
    force: bool = False,
    quiet: bool = False,
    api_key: Optional[str] = None,
) -> List[Path]:
    """Download and gunzip the datasets of ``symbol`` on ``exchange``.

    Returns the extracted CSV paths. ``api_key`` defaults to ``$TARDIS_API_KEY``; without one
    only the first day of a month is served and anything else raises
    :class:`DownloadError` with the HTTP 401.
    """
    cache = cache or Cache()
    kinds = tuple(kinds) if kinds is not None else KINDS
    key = api_key if api_key is not None else os.environ.get("TARDIS_API_KEY", "")
    headers = {"Authorization": f"Bearer {key}"} if key else {}
    out: List[Path] = []
    progress = None if quiet else stderr_progress
    for date in dates:
        for kind in kinds:
            cache_key = daily_key(exchange, kind, symbol, date)
            csv = cache_key[: -len(".csv.gz")] + ".csv"
            if force or not (cache.has(cache_key) or (extract and cache.has(csv))):
                if not key and not date.endswith("-01"):
                    raise DownloadError(
                        f"{date}: only the first day of a month is free; set $TARDIS_API_KEY "
                        "for any other date"
                    )
                cache.download(
                    url_for(exchange, kind, symbol, date),
                    cache_key,
                    headers=headers,
                    force=force,
                    progress=progress,
                )
                if progress and sys.stderr.isatty():
                    sys.stderr.write("\n")
            if not extract:
                out.append(cache.path(cache_key))
            elif cache.has(csv) and not force:
                out.append(cache.path(csv))
            else:
                out.append(cache.extract(cache_key, force=force))
    return out
