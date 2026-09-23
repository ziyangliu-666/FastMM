"""Binance public data dumps (https://data.binance.vision) as a fetcher for the `binance`
market-data source.

The archive publishes one ZIP of CSV per symbol and day next to a `.CHECKSUM` holding its
SHA-256. This downloads both, verifies, and unpacks into the cache at the archive's own key,
which is where ``fastmm::bt::BinanceDataset`` looks.

    fetch("BTCUSDT", ["2024-03-27"], market="um")

What exists:

  market="um"    USDⓈ-M futures. bookTicker (top of book) 2023-05-16 .. 2024-03-30 only: the
                 archive stopped publishing it. aggTrades to yesterday.
  market="spot"  aggTrades and trades to yesterday. No book data at all.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Iterable, List, Optional, Sequence

if __package__:
    from .cache import Cache, DownloadError, stderr_progress
else:  # python3 python/fastmm/data/binance.py
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from cache import Cache, DownloadError, stderr_progress  # type: ignore[no-redef]

BASE_URL = "https://data.binance.vision"

MARKETS = {"spot": "data/spot/daily", "um": "data/futures/um/daily"}

# Per market, the kinds this repository can decode.
KINDS = {"spot": ("aggTrades",), "um": ("bookTicker", "aggTrades")}


def daily_key(market: str, kind: str, symbol: str, date: str, ext: str = ".zip") -> str:
    """Archive key of one daily file, e.g.
    ``data/futures/um/daily/bookTicker/BTCUSDT/BTCUSDT-bookTicker-2024-03-27.zip``."""
    if market not in MARKETS:
        raise ValueError(f"market must be one of {sorted(MARKETS)}, got {market!r}")
    return f"{MARKETS[market]}/{kind}/{symbol}/{symbol}-{kind}-{date}{ext}"


def date_range(first: str, last: str) -> List[str]:
    """Every ``YYYY-MM-DD`` from ``first`` to ``last`` inclusive."""
    from datetime import date, timedelta

    a = date.fromisoformat(first)
    b = date.fromisoformat(last)
    if b < a:
        raise ValueError(f"date range runs backwards: {first} .. {last}")
    return [(a + timedelta(days=i)).isoformat() for i in range((b - a).days + 1)]


def fetch(
    symbol: str,
    dates: Sequence[str],
    *,
    market: str = "um",
    kinds: Optional[Iterable[str]] = None,
    cache: Optional[Cache] = None,
    extract: bool = True,
    force: bool = False,
    quiet: bool = False,
) -> List[Path]:
    """Download and unpack the daily dumps of ``symbol``.

    Returns the extracted CSV paths (the ZIPs when ``extract`` is false). Every file is checked
    against the archive's published SHA-256 before it is accepted and a partial download
    resumes. A day the archive does not have raises :class:`DownloadError` naming the URL.
    """
    cache = cache or Cache()
    kinds = tuple(kinds) if kinds is not None else KINDS[market]
    symbol = symbol.upper()
    out: List[Path] = []
    progress = None if quiet else stderr_progress
    for date in dates:
        for kind in kinds:
            key = daily_key(market, kind, symbol, date)
            csv = key[:-4] + ".csv"
            if force or not (cache.has(key) or (extract and cache.has(csv))):
                text = cache.fetch_text(f"{BASE_URL}/{key}.CHECKSUM")
                if text is None:
                    raise DownloadError(
                        f"{BASE_URL}/{key} does not exist. The archive stopped publishing "
                        "futures bookTicker after 2024-03-30; check the date and the market."
                    )
                cache.download(
                    f"{BASE_URL}/{key}",
                    key,
                    sha256=text.split()[0],
                    force=force,
                    progress=progress,
                )
                if progress and sys.stderr.isatty():
                    sys.stderr.write("\n")
            if not extract:
                out.append(cache.path(key))
            elif cache.has(csv) and not force:
                out.append(cache.path(csv))
            else:
                out.append(cache.extract(key, force=force))
    return out
