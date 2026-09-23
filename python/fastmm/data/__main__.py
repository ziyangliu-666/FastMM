"""Download public market data into the local cache.

    python3 -m fastmm.data fetch --symbol BTCUSDT --date 2024-03-27
    python3 -m fastmm.data fetch --symbol BTCUSDT --from 2024-03-25 --to 2024-03-27
    python3 -m fastmm.data ls
    python3 -m fastmm.data convert --config configs/backtest-binance.toml \\
        --data binance:BTCUSDT,2024-03-27 --out day.fmj

`convert` needs the compiled bindings; `fetch` and `ls` are the standard library alone, so a
source checkout without a built extension can run them directly:

    python3 python/fastmm/data/__main__.py fetch --symbol BTCUSDT --date 2024-03-27
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

if __package__:
    from . import binance, tardis
    from .cache import Cache, DownloadError, human
else:  # python3 python/fastmm/data/__main__.py
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import binance  # type: ignore[no-redef]
    import tardis  # type: ignore[no-redef]
    from cache import Cache, DownloadError, human  # type: ignore[no-redef]


def _dates(args: argparse.Namespace) -> list:
    if args.date:
        return [args.date]
    if not args.date_from:
        raise SystemExit("fastmm.data: --date, or --from and --to, is required")
    return binance.date_range(args.date_from, args.date_to or args.date_from)


def _fetch(args: argparse.Namespace) -> int:
    cache = Cache(args.dir)
    kinds = args.kind or None
    try:
        if args.source == "tardis":
            if not args.exchange:
                raise SystemExit("fastmm.data: --source tardis needs --exchange")
            files = tardis.fetch(
                args.exchange,
                args.symbol,
                _dates(args),
                kinds=kinds,
                cache=cache,
                extract=not args.no_extract,
                force=args.force,
            )
        else:
            files = binance.fetch(
                args.symbol,
                _dates(args),
                market=args.market,
                kinds=kinds,
                cache=cache,
                extract=not args.no_extract,
                force=args.force,
            )
    except (DownloadError, ValueError) as exc:
        print(f"fastmm.data: {exc}", file=sys.stderr)
        return 1
    total = 0
    for path in files:
        size = path.stat().st_size
        total += size
        print(f"{path}  {human(size)}")
    print(f"{len(files)} files, {human(total)} in {cache.root}")
    return 0


def _ls(args: argparse.Namespace) -> int:
    cache = Cache(args.dir)
    if not cache.root.is_dir():
        print(f"{cache.root} (empty)")
        return 0
    total = 0
    for path in sorted(cache.root.rglob("*")):
        if path.is_file() and path.suffix in (".csv", ".zip", ".gz", ".fmj"):
            size = path.stat().st_size
            total += size
            print(f"{path.relative_to(cache.root)}  {human(size)}")
    print(f"{human(total)} in {cache.root}")
    return 0


def _convert(args: argparse.Namespace) -> int:
    try:
        import fastmm
    except ImportError:
        print(
            "fastmm.data: convert needs the compiled bindings (pip install fastmm-engine), "
            "or use the fastmm-data program",
            file=sys.stderr,
        )
        return 1
    config = fastmm.BacktestConfig.from_toml(args.config)
    events = fastmm.convert_data(args.data, args.out, config)
    print(f"{events} events -> {args.out}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="python -m fastmm.data",
                                     description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    fetch = sub.add_parser("fetch", help="download daily dumps into the cache")
    fetch.add_argument("--source", default="binance", choices=("binance", "tardis"),
                       help="archive to download from (default: binance)")
    fetch.add_argument("--symbol", required=True, help="e.g. BTCUSDT")
    fetch.add_argument("--date", help="YYYY-MM-DD")
    fetch.add_argument("--from", dest="date_from", help="first date of a range")
    fetch.add_argument("--to", dest="date_to", help="last date of a range (default: --from)")
    fetch.add_argument("--market", default="um", choices=sorted(binance.MARKETS),
                       help="binance: um = USD-M futures (book + trades), spot = trades only")
    fetch.add_argument("--exchange", help="tardis: the exchange id, e.g. binance-futures")
    fetch.add_argument("--kind", action="append",
                       help="bookTicker | aggTrades | incremental_book_L2 | trades (repeatable; "
                            "default: everything the source has)")
    fetch.add_argument("--dir", help="cache root (default: $FASTMM_DATA_HOME)")
    fetch.add_argument("--no-extract", action="store_true", help="keep the ZIPs packed")
    fetch.add_argument("--force", action="store_true", help="re-download files already cached")
    fetch.set_defaults(fn=_fetch)

    ls = sub.add_parser("ls", help="what the cache holds")
    ls.add_argument("--dir", help="cache root (default: $FASTMM_DATA_HOME)")
    ls.set_defaults(fn=_ls)

    convert = sub.add_parser("convert", help="write a --data source to an .fmj journal")
    convert.add_argument("--config", required=True, help="backtest TOML (for the instruments)")
    convert.add_argument("--data", required=True,
                         help="source spec, e.g. binance:BTCUSDT,2024-03-27")
    convert.add_argument("--out", required=True, help="output .fmj")
    convert.set_defaults(fn=_convert)

    args = parser.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
