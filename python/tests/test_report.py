"""The HTML run report (fastmm.write_report, `fastmm report`)."""

from __future__ import annotations

import re

import pytest

import fastmm
from conftest import EXAMPLE_TOML
from fastmm import report as report_module


@pytest.fixture(scope="module")
def run(tmp_path_factory):
    """One 10 s backtest, written out as a run directory and as a session journal."""
    cfg = fastmm.BacktestConfig.from_toml(EXAMPLE_TOML)
    cfg.duration_s = 10
    out = tmp_path_factory.mktemp("run")
    cfg.journal_out = str(out / "session.fmj")
    result = fastmm.run_backtest(cfg, data="synthetic")
    result.write_all(str(out / "backtest"))
    return result, out


def _assert_self_contained(html: str) -> None:
    assert html.startswith("<!doctype html>")
    assert "<script" not in html.lower()
    # Nothing to fetch: no stylesheet, image, font or frame from anywhere.
    assert not re.search(r'(src|href)\s*=\s*"(?!data:,)(?!#)', html)
    assert html.rstrip().endswith("</html>")


def test_report_from_result(run, tmp_path):
    result, _ = run
    out = fastmm.write_report(result, tmp_path / "result.html")
    html = (tmp_path / "result.html").read_text(encoding="utf-8")
    assert out == str(tmp_path / "result.html")
    _assert_self_contained(html)
    assert "basic_mm" in html
    assert "net PnL" in html
    assert "Where the PnL came from" in html
    assert "gross spread capture" in html  # the decomposition, not the realized/unrealized split
    assert "Markouts" in html
    assert "Fill quality" in html
    assert "<svg" in html


def test_report_from_run_directory(run):
    result, out = run
    path = fastmm.write_report(out / "backtest")
    assert path == str(out / "backtest" / "report.html")
    html = (out / "backtest" / "report.html").read_text(encoding="utf-8")
    _assert_self_contained(html)
    # The instrument's currencies and the parameters come from the configuration beside it.
    assert "half_spread_bps" in html
    assert f"{result.stats()['fills']:,}" in html


def test_report_from_session_journal(run, tmp_path):
    _, out = run
    path = fastmm.write_report(out / "session.fmj", tmp_path / "session.html")
    html = open(path, encoding="utf-8").read()
    _assert_self_contained(html)
    assert "session report" in html
    # A session has fills and counts but no venue book at each fill.
    assert "Quotes, rejects and fills" in html
    assert "Markouts" not in html
    assert "markouts, spread capture and fill quality are not in this report" in html


def test_missing_input_is_an_error(tmp_path):
    with pytest.raises(FileNotFoundError):
        fastmm.write_report(tmp_path / "nowhere")


def test_command_line(run, tmp_path, capsys):
    from fastmm.__main__ import main

    _, out = run
    target = tmp_path / "cli.html"
    assert main(["report", str(out / "backtest"), "-o", str(target), "--config",
                 str(EXAMPLE_TOML)]) == 0
    assert capsys.readouterr().out.strip() == str(target)
    _assert_self_contained(target.read_text(encoding="utf-8"))
    assert main(["report", str(tmp_path / "nowhere")]) == 1


def test_missing_metrics_drop_their_panel(tmp_path):
    """A report of a run with no fills keeps its shape instead of inventing numbers."""
    run = report_module.Run("backtest", "empty_mm", "nowhere")
    run.metrics = {"net_pnl": 0.0, "fills": 0}
    html = report_module.render_html(run)
    _assert_self_contained(html)
    assert "Markouts" not in html
    assert "Equity and inventory" not in html
    assert "&mdash;" in html  # an em dash where a number is not known


def _journal(path, instruments, events):
    """A minimal session journal: header, instrument records and one block of events."""
    import struct

    header = bytearray(256)
    header[0:4] = b"FMJ1"
    struct.pack_into("<III", header, 4, 3, 256 + 128 * len(instruments), len(instruments))
    records = b""
    for i, symbol in enumerate(instruments):
        r = bytearray(128)
        struct.pack_into("<I", r, 0, i)
        r[80:80 + len(symbol)] = symbol.encode()
        r[100] = len(symbol)
        records += bytes(r)
    body = b"".join(events)
    block = struct.pack("<4sIQQIII28s", b"FMJB", len(body), 0, 0, 0, 0, 0, b"")
    path.write_bytes(bytes(header) + records + block + body)


def _event(etype, inst, ts, body):
    import struct

    e = bytearray(64)
    struct.pack_into("<IBBBBI", e, 0, 64 + len(body), etype, 0, 0, 0, inst)
    struct.pack_into("<q", e, 40, ts)
    return bytes(e) + body


def _ticker(inst, ts, bid, ask):
    import struct

    b = bytearray(64)
    struct.pack_into("<q8xq", b, 0, round(bid * 1e8), round(ask * 1e8))
    return _event(3, inst, ts, bytes(b))


def _fill(inst, ts, side, px, qty, exec_id):
    import struct

    b = bytearray(144)
    b[49:49 + len(exec_id)] = exec_id
    b[89] = len(exec_id)
    struct.pack_into("<qqqqq", b, 96, round(px * 1e8), round(qty * 1e8), 0, 0, 0)
    b[136] = side
    b[137] = 1
    return _event(9, inst, ts, bytes(b))


def test_session_report_keeps_a_book_per_instrument(tmp_path):
    """A buy of one instrument and a sell of another are two open positions, each marked at its
    own price, not one closed round trip."""
    path = tmp_path / "two.fmj"
    _journal(path, ["AAAUSDT", "BBBUSDT"], [
        _ticker(0, 1, 99.9, 100.1),
        _ticker(1, 1, 2999.0, 3001.0),
        _fill(0, 2, 0, 100.0, 1.0, b"a"),
        _fill(1, 3, 1, 3000.0, 1.0, b"b"),
        _ticker(0, 4, 100.9, 101.1),
        _ticker(1, 4, 2989.0, 2991.0),
    ])
    run = report_module.read_journal(path)
    m = run.metrics
    assert m["fills"] == 2
    assert m["realized_pnl"] == pytest.approx(0.0)
    # +1 on AAA (100 -> 101) and +10 on the short BBB (3000 -> 2990).
    assert m["unrealized_pnl"] == pytest.approx(11.0)
    assert m["net_pnl"] == pytest.approx(11.0)
    assert m["final_position"] == pytest.approx(1.0)
    assert run.symbol == "AAAUSDT"
    assert run.equity["mid"][-1] == pytest.approx(101.0)
    assert any("2 instruments traded" in n for n in run.notes)
