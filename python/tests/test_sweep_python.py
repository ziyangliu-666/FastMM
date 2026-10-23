"""fastmm.sweep and fastmm.walk_forward of a fastmm.Strategy class, in worker processes.

BasicMMExact reproduces the C++ basic_mm's outbound hash, so a sweep or walk-forward of the class
must give the same points, hashes and report as the same call with the name "basic_mm".
"""

import pickle
import sys

import pytest

import fastmm
from conftest import REPO

sys.path.insert(0, str(REPO / "examples" / "python" / "strategies"))

from basic_mm_exact import BasicMMExact  # noqa: E402

GRID = {"half_spread_bps": [0.01, 0.03], "skew_bps_per_unit": [0.0, 0.01]}


def _short(cfg, seconds):
    out = cfg.copy()
    out.duration_s = seconds
    return out


def _hashes(points):
    return [(p, r.outbound_sha256) for p, r in points]


@pytest.mark.parametrize("threads", [1, 3])
def test_sweep_of_a_python_class_matches_the_cpp_strategy(example_config, threads):
    cfg = _short(example_config, 8)
    cpp = fastmm.sweep(cfg, GRID, data="synthetic", strategy="basic_mm", threads=2)
    py = fastmm.sweep(cfg, GRID, data="synthetic", strategy=BasicMMExact, threads=threads)
    assert _hashes(py) == _hashes(cpp)
    for (_, a), (_, b) in zip(py, cpp):
        assert isinstance(a, fastmm.BacktestResult)
        assert a.strategy == "py:BasicMMExact"
        assert a.stats()["net_pnl"] == b.stats()["net_pnl"]
        assert len(a.fills["ts"]) == len(b.fills["ts"])
    assert sum(len(r.fills["ts"]) for _, r in py) > 0


def test_walk_forward_of_a_python_class_matches_the_cpp_strategy(example_config, synthetic_csv):
    cpp = fastmm.walk_forward(example_config, GRID, folds=3, data=str(synthetic_csv),
                              strategy="basic_mm", threads=2)
    py = fastmm.walk_forward(example_config, GRID, folds=3, data=str(synthetic_csv),
                             strategy=BasicMMExact, threads=4)
    assert py["table"] == cpp["table"]
    for a, b in zip(py["folds"], cpp["folds"]):
        assert (a["start_ts"], a["end_ts"], a["best"], a["chosen"]) == (
            b["start_ts"], b["end_ts"], b["best"], b["chosen"])
        assert a["scores"] == b["scores"]
        assert _hashes(a["points"]) == _hashes(b["points"])
    synthetic = _short(example_config, 6)
    cpp = fastmm.walk_forward(synthetic, GRID, folds=2, data="synthetic", strategy="basic_mm")
    py = fastmm.walk_forward(synthetic, GRID, folds=2, data="synthetic", strategy=BasicMMExact)
    assert py["table"] == cpp["table"]
    one = fastmm.walk_forward(synthetic, GRID, folds=1, data="synthetic", strategy=BasicMMExact)
    plain = fastmm.sweep(synthetic, GRID, data="synthetic", strategy="basic_mm")
    assert _hashes(one["folds"][0]["points"]) == _hashes(plain)
    assert one["folds"][0]["end_ts"] > one["folds"][0]["start_ts"] > 0


def test_python_sweep_errors(example_config):
    cfg = _short(example_config, 2)
    with pytest.raises(TypeError, match="class, not an instance"):
        fastmm.sweep(cfg, GRID, data="synthetic", strategy=BasicMMExact())
    with pytest.raises(ValueError, match="outside"):
        fastmm.sweep(cfg, {"half_spread_bps": [-1.0]}, data="synthetic", strategy=BasicMMExact,
                     threads=2)
    with pytest.raises(ValueError, match="metric"):
        fastmm.walk_forward(cfg, GRID, folds=2, data="synthetic", metric="x",
                            strategy=BasicMMExact)
    matching = cfg.copy()
    matching.fill_model = "matching"
    with pytest.raises(ValueError, match="l2_queue"):
        fastmm.walk_forward(matching, GRID, folds=2, data="synthetic", strategy=BasicMMExact)


def test_module_class_names_resolve(example_config):
    cfg = _short(example_config, 4)
    cfg.strategy = "basic_mm_exact:BasicMMExact"
    r = fastmm.run_backtest(cfg, data="synthetic")
    assert r.strategy == "py:BasicMMExact"
    cpp = fastmm.run_backtest(_short(example_config, 4), data="synthetic")
    assert r.outbound_sha256 == cpp.outbound_sha256
    points = fastmm.sweep(cfg, {"half_spread_bps": [0.01]}, data="synthetic", threads=1)
    assert points[0][1].strategy == "py:BasicMMExact"
    cfg.strategy = "py:BasicMMExact"
    with pytest.raises(ValueError, match="without its module"):
        fastmm.run_backtest(cfg, data="synthetic")
    with pytest.raises(ValueError, match="cannot load"):
        fastmm.run_backtest(cfg, data="synthetic", strategy="basic_mm_exact:Nope")


def test_backtest_result_pickles(example_config):
    r = fastmm.run_backtest(_short(example_config, 5), data="synthetic")
    back = pickle.loads(pickle.dumps(r))
    assert back.summary_json() == r.summary_json()
    assert back.summary_table() == r.summary_table()
    assert back.markouts() == r.markouts()
    assert back.engine_stats() == r.engine_stats()
    assert back.transport_stats() == r.transport_stats()
    for name in ("fills", "equity", "orders"):
        a, b = getattr(back, name), getattr(r, name)
        assert a.keys() == b.keys()
        for k in a:
            assert (a[k] == b[k]).all(), (name, k)
    assert len(back.equity_by_instrument) == len(r.equity_by_instrument) > 0
    with pytest.raises(ValueError, match="pickle state"):
        cls = type(r)
        cls.__new__(cls).__setstate__(b"nonsense")


def test_sweep_of_a_hot_class_matches_the_cpp_strategy(example_config):
    pytest.importorskip("numba")
    from basic_mm_hot import BasicMMHot

    cfg = _short(example_config, 8)
    grid = {"half_spread_bps": [0.01, 0.03]}
    cpp = fastmm.sweep(cfg, grid, data="synthetic", strategy="basic_mm")
    hot = fastmm.sweep(cfg, grid, data="synthetic", strategy=BasicMMHot, threads=2)
    assert _hashes(hot) == _hashes(cpp)


class _KeepsState(BasicMMExact):
    """Ends every run with the state it was restored with plus one "+"."""

    restored = []

    def restore(self, data):
        _KeepsState.restored.append(bytes(data))
        self.kept = bytes(data)
        return True

    def state(self):
        return getattr(self, "kept", b"") + b"+"


def test_sweep_points_start_from_the_state_file_and_none_writes_it(example_config, tmp_path):
    cfg = _short(example_config, 3)
    path = tmp_path / "keeps.state"
    path.write_bytes(b"s0")
    cfg.state_file = str(path)
    _KeepsState.restored.clear()
    fastmm.sweep(cfg, {"half_spread_bps": [0.01, 0.03]}, data="synthetic", strategy=_KeepsState,
                 threads=1)
    assert _KeepsState.restored == [b"s0", b"s0"]
    assert path.read_bytes() == b"s0"
    fastmm.run_backtest(cfg, data="synthetic", strategy=_KeepsState)
    assert path.read_bytes() == b"s0+"
