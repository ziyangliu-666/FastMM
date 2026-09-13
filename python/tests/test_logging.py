from pathlib import Path

import pytest

import fastmm

REPO = Path(__file__).resolve().parents[2]


def test_enable_logging_to_file_and_disable(tmp_path):
    log = tmp_path / "fastmm.log"
    fastmm.enable_logging("debug", log)
    try:
        cfg = fastmm.BacktestConfig.from_toml(REPO / "configs" / "backtest-example.toml")
        cfg.duration_s = 5
        result = fastmm.run_backtest(cfg, data="synthetic")
        assert result.stats()["fills"] >= 0
    finally:
        fastmm.disable_logging()
    assert log.exists()
    fastmm.disable_logging()  # idempotent


def test_enable_logging_restarts_and_accepts_stderr():
    fastmm.enable_logging("warn")
    fastmm.enable_logging("error")
    fastmm.disable_logging()


def test_enable_logging_rejects_unknown_level():
    with pytest.raises(ValueError):
        fastmm.enable_logging("loud")
