"""fastmm.shadow over a recorded session's journal: the journal's own strategy by its name and a class
with hot hooks give the same results, the sim error at the session's own parameters is none, since
the session here is itself a backtest of the same engine.
"""

import sys

import pytest

import fastmm
from conftest import REPO

sys.path.insert(0, str(REPO / "examples" / "python" / "strategies"))

FIXTURES = REPO / "tests" / "fixtures" / "btcusd"
BITSTAMP = f"itch:{FIXTURES / 'bitstamp-btcusd.itch'},date=2026-10-27"
GRID = {"half_spread_bps": [0.0, 0.3], "quote_qty": [1, 4]}


def _session(tmp_path):
    cfg = fastmm.BacktestConfig.from_toml(FIXTURES / "basic_mm.toml")
    cfg.fill_model = "l2_queue"
    path = tmp_path / "session.fmj"
    cfg.journal_out = str(path)
    fastmm.run_backtest(cfg, data=BITSTAMP, strategy="basic_mm")
    return path


def test_shadow_of_a_cpp_strategy_has_no_error_at_the_session_parameters(tmp_path):
    report = fastmm.shadow(str(_session(tmp_path)), GRID, window_s=5, settle_s=1,
                           report=str(tmp_path / "r.json"))
    assert report["reader"]["late"] == 0
    assert report["windows"] > 3
    assert len(report["variants"]) == 4
    every = report["error"]["all"]
    assert every["live"]["fills"] > 0
    assert every["sim"]["fills"] == every["live"]["fills"]
    assert abs(every["net_error"]) < 1e-6
    assert (tmp_path / "r.json").exists()
    assert (tmp_path / "r.jsonl").exists()


def test_shadow_of_a_hot_class_is_the_cpp_strategy(tmp_path):
    pytest.importorskip("numba")
    from basic_mm_hot import BasicMMHot

    path = str(_session(tmp_path))
    cpp = fastmm.shadow(path, GRID, window_s=5, settle_s=1)
    hot = fastmm.shadow(path, GRID, strategy=BasicMMHot, window_s=5, settle_s=1)
    a = cpp["sessions"][0]["points"]
    b = hot["sessions"][0]["points"]
    assert [p["outbound_sha256"] for p in a] == [p["outbound_sha256"] for p in b]
    assert sum(p["fills"] for p in a) > 0
