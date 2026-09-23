"""The fetch/cache layer and the data-source specs, without touching the network."""

from __future__ import annotations

import gzip
import hashlib
import zipfile

import pytest

import fastmm
from fastmm.data import Cache, DownloadError, cache_root, sha256_file
from fastmm.data import binance, tardis

BOOK_TICKER = (
    "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,"
    "transaction_time,event_time\n"
    "1,70000.0,1.5,70000.1,2.0,1711497600000,1711497600002\n"
    "2,70000.0,1.0,70000.1,2.0,1711497600002,1711497600004\n"
)


def test_cache_root_follows_the_environment(tmp_path, monkeypatch):
    monkeypatch.setenv("FASTMM_DATA_HOME", str(tmp_path / "explicit"))
    assert cache_root() == tmp_path / "explicit"
    monkeypatch.delenv("FASTMM_DATA_HOME")
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "xdg"))
    assert cache_root() == tmp_path / "xdg" / "fastmm" / "data"
    assert cache_root(tmp_path / "override") == tmp_path / "override"


def test_download_verifies_the_checksum_and_leaves_nothing_behind(tmp_path):
    payload = b"some bytes"
    src = tmp_path / "src.bin"
    src.write_bytes(payload)
    cache = Cache(tmp_path / "cache")
    url = src.as_uri()
    digest = hashlib.sha256(payload).hexdigest()

    path = cache.download(url, "a/b.bin", sha256=digest)
    assert path.read_bytes() == payload
    assert sha256_file(path) == digest
    assert not (path.parent / "b.bin.part").exists()
    # A cached file is not fetched again, and a wrong checksum is refused.
    assert cache.download("file:///does-not-exist", "a/b.bin") == path
    with pytest.raises(DownloadError):
        cache.download(url, "a/c.bin", sha256="0" * 64)
    assert not cache.has("a/c.bin")


def test_extract_handles_zip_and_gzip(tmp_path):
    cache = Cache(tmp_path)
    (cache.root / "z").mkdir(parents=True)
    with zipfile.ZipFile(cache.root / "z" / "d.zip", "w") as zf:
        zf.writestr("d.csv", BOOK_TICKER)
    assert cache.extract("z/d.zip").read_text() == BOOK_TICKER
    with gzip.open(cache.root / "z" / "g.csv.gz", "wt") as fh:
        fh.write(BOOK_TICKER)
    assert cache.extract("z/g.csv.gz").read_text() == BOOK_TICKER


def test_archive_keys_match_the_layout_the_decoder_reads():
    assert binance.daily_key("um", "bookTicker", "BTCUSDT", "2024-03-27") == (
        "data/futures/um/daily/bookTicker/BTCUSDT/BTCUSDT-bookTicker-2024-03-27.zip"
    )
    assert binance.daily_key("spot", "aggTrades", "BTCUSDT", "2024-03-27", ".csv").startswith(
        "data/spot/daily/aggTrades/"
    )
    with pytest.raises(ValueError):
        binance.daily_key("coinm", "aggTrades", "BTCUSDT", "2024-03-27")
    assert binance.date_range("2024-02-28", "2024-03-01") == [
        "2024-02-28",
        "2024-02-29",
        "2024-03-01",
    ]
    with pytest.raises(ValueError):
        binance.date_range("2024-03-02", "2024-03-01")
    assert tardis.daily_key("binance-futures", "trades", "BTCUSDT", "2026-09-01") == (
        "tardis/v1/binance-futures/trades/2026/09/01/BTCUSDT.csv.gz"
    )
    assert tardis.url_for("binance-futures", "trades", "BTCUSDT", "2026-09-01").endswith(
        "/v1/binance-futures/trades/2026/09/01/BTCUSDT.csv.gz"
    )


def test_tardis_refuses_a_paid_date_without_a_key(tmp_path, monkeypatch):
    monkeypatch.delenv("TARDIS_API_KEY", raising=False)
    with pytest.raises(DownloadError, match="first day of a month"):
        tardis.fetch("binance", "BTCUSDT", ["2026-09-02"], cache=Cache(tmp_path))


def test_data_sources_lists_what_each_source_carries():
    text = fastmm.data_sources()
    for name in ("synthetic", "journal", "csv", "binance", "tardis"):
        assert name in text
    assert "no depth beyond the touch" in text


def test_run_backtest_accepts_a_source_spec(tmp_path, example_config):
    key = binance.daily_key("um", "bookTicker", "BTCUSDT", "2024-03-27", ".csv")
    path = tmp_path / key
    path.parent.mkdir(parents=True)
    path.write_text(BOOK_TICKER)
    spec = f"binance:BTCUSDT,2024-03-27,trades=false,dir={tmp_path}"

    result = fastmm.run_backtest(example_config, data=spec, strategy="basic_mm")
    assert result.md_events == 2

    out = tmp_path / "day.fmj"
    assert fastmm.convert_data(spec, str(out), example_config) == 2
    from_journal = fastmm.run_backtest(example_config, data=str(out), strategy="basic_mm")
    assert from_journal.md_events == result.md_events
    assert from_journal.outbound_sha256 == result.outbound_sha256


def test_a_bad_spec_says_what_is_wrong(example_config):
    with pytest.raises(RuntimeError, match="nosuchsource"):
        fastmm.run_backtest(example_config, data="nosuchsource:x", strategy="basic_mm")
    with pytest.raises(RuntimeError, match="unknown option"):
        fastmm.run_backtest(example_config, data="binance:BTCUSDT,typo=1", strategy="basic_mm")
