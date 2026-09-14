"""Python strategies drive the engine exactly like C++ ones (ADR-0012, section 7).

basic_mm_exact.py is a raw-integer port of the C++ BasicMM; on the same configuration, data and
seed it must reproduce the C++ outbound SHA-256 (the determinism fingerprint).
"""

import os
import subprocess
import sys

import pytest

import fastmm
from conftest import FIXTURE_FMJ, FIXTURE_SHA, REPO

sys.path.insert(0, str(REPO / "examples" / "python" / "strategies"))

from basic_mm_exact import BasicMMExact  # noqa: E402
from skew_mm import SkewMM  # noqa: E402


def test_basic_mm_exact_reproduces_the_committed_fixture_hash(example_config):
    expected = FIXTURE_SHA.read_text().strip()
    cpp = fastmm.run_backtest(example_config, data=FIXTURE_FMJ, strategy="basic_mm")
    py = fastmm.run_backtest(example_config, data=FIXTURE_FMJ, strategy=BasicMMExact)
    assert cpp.outbound_sha256 == expected
    assert py.outbound_sha256 == expected
    assert py.outbound_messages == cpp.outbound_messages > 0
    assert py.md_events == 1000
    assert py.strategy == "py:BasicMMExact"
    assert py.params["quote_qty"] == "0.002" and py.params["levels"] == "1"


@pytest.mark.parametrize(
    "fill_model, params",
    [
        ("l2_queue", {"levels": 3, "level_step_ticks": 2, "pull_on_stale_ms": 20,
                      "skew_bps_per_unit": 0.05, "max_inventory": 0.006}),
        ("matching", {"levels": 2, "half_spread_bps": 0.003, "skew_bps_per_unit": 0.02,
                      "requote_threshold_ticks": 1}),
    ],
)
def test_basic_mm_exact_matches_cpp_with_fills_timers_and_skew(example_config, fill_model, params):
    cfg = example_config.copy()
    cfg.fill_model = fill_model
    cfg.duration_s = 30
    cfg.seed = 11
    cpp = fastmm.run_backtest(cfg, data="synthetic", strategy="basic_mm", params=params)
    py = fastmm.run_backtest(cfg, data="synthetic", strategy=BasicMMExact, params=params)
    assert cpp.stats()["fills"] > 10
    assert cpp.stats()["cancels"] > 10
    if fill_model == "l2_queue":
        assert cpp.engine_stats()["timers_fired"] > 0
    assert abs(cpp.stats()["final_position"]) > 0 or cpp.stats()["inventory_max"] > 0
    assert py.outbound_sha256 == cpp.outbound_sha256
    assert py.outbound_messages == cpp.outbound_messages
    assert py.stats()["net_pnl"] == cpp.stats()["net_pnl"]


def test_python_journal_records_the_py_name(example_config, tmp_path):
    cfg = example_config.copy()
    cfg.journal_out = str(tmp_path / "py.fmj")
    r = fastmm.run_backtest(cfg, data=FIXTURE_FMJ, strategy=BasicMMExact)
    info = fastmm.inspect_journal(cfg.journal_out)
    assert info["strategy"] == "py:BasicMMExact"
    assert info["outbound_messages"] == r.outbound_messages

    class AVeryLongStrategyClassNameThatDoesNotFit(fastmm.Strategy):
        def on_start(self, ctx):
            pass

    cfg.clear_params()
    cfg.journal_out = str(tmp_path / "long.fmj")
    long_name = AVeryLongStrategyClassNameThatDoesNotFit
    r = fastmm.run_backtest(cfg, data=FIXTURE_FMJ, strategy=long_name)
    assert r.strategy.startswith("py:test_python_journal_records_the_py_name.<locals>.")
    name = fastmm.inspect_journal(cfg.journal_out)["strategy"]
    assert len(name) == 31 and r.strategy.startswith(name)


def _skew_cfg(example_config):
    cfg = example_config.copy()
    cfg.clear_params()
    cfg.duration_s = 20
    return cfg


def test_two_runs_give_the_same_hash(example_config):
    cfg = _skew_cfg(example_config)
    a = fastmm.run_backtest(cfg, data="synthetic", strategy=SkewMM)
    b = fastmm.run_backtest(cfg, data="synthetic", strategy=SkewMM)
    assert a.outbound_messages > 100
    assert a.outbound_sha256 == b.outbound_sha256


_SUBPROCESS = """
import sys
sys.path.insert(0, {examples!r})
import fastmm
from skew_mm import SkewMM
cfg = fastmm.BacktestConfig.from_toml({toml!r})
cfg.clear_params()
cfg.duration_s = 20
print(fastmm.run_backtest(cfg, data="synthetic", strategy=SkewMM).outbound_sha256)
"""


def test_hash_is_independent_of_pythonhashseed(example_config):
    expected = fastmm.run_backtest(_skew_cfg(example_config), data="synthetic",
                                   strategy=SkewMM).outbound_sha256
    code = _SUBPROCESS.format(examples=str(REPO / "examples" / "python" / "strategies"),
                              toml=str(REPO / "configs" / "backtest-example.toml"))
    for seed in ("0", "12345"):
        env = dict(os.environ, PYTHONHASHSEED=seed)
        out = subprocess.run([sys.executable, "-c", code], env=env, check=True,
                             capture_output=True, text=True)
        assert out.stdout.strip() == expected
