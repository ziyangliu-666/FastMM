"""fastmm.sweep(verify=...) and fastmm.walk_forward on recorded data: two minutes of BTC/USD,
Bitstamp order by order and Coinbase L2 (tests/fixtures/btcusd). Each point's result must be the
one its own backtest gives, under both queue fill models, for a strategy on one venue (basic_mm,
C++ and a @fastmm.hot class) and one on two (xmm).
"""

import sys

import pytest

import fastmm
from fastmm import _core
from conftest import REPO

sys.path.insert(0, str(REPO / "examples" / "python" / "strategies"))

FIXTURES = REPO / "tests" / "fixtures" / "btcusd"
BITSTAMP = f"itch:{FIXTURES / 'bitstamp-btcusd.itch'},date=2026-10-27"
COINBASE = f"csv:{FIXTURES / 'coinbase-btcusd.csv'},venue=1"
MODELS = ["l3_queue", "l2_queue"]

BASIC_GRID = {"half_spread_bps": [0.0, 0.3], "levels": [1, 2], "quote_qty": [1, 4]}
XMM_GRID = {"edge_bps": [0.0, 0.2], "quote_qty": [1, 3], "basis_halflife_s": [10, 60]}


def _config(name, model, **params):
    cfg = fastmm.BacktestConfig.from_toml(FIXTURES / name)
    cfg.fill_model = model
    for k, v in params.items():
        cfg.set_param(k, v)
    return cfg


def _points(grid):
    return len(_core._expand_grid(grid))


def _fills(points):
    return sum(len(r.fills["ts"]) for _, r in points)


def _alone(cfg, data, strategy, params):
    return fastmm.run_backtest(cfg, data=data, strategy=strategy, params=params)


@pytest.mark.parametrize("model", MODELS)
def test_basic_mm_sweep_points_are_their_own_backtests(model):
    cfg = _config("basic_mm.toml", model)
    points = fastmm.sweep(cfg, BASIC_GRID, data=BITSTAMP, threads=3, verify=_points(BASIC_GRID))
    assert len(points) == _points(BASIC_GRID)
    assert _fills(points) > 0
    params, swept = points[-1]
    assert _core._result_difference(_alone(cfg, BITSTAMP, "basic_mm", params), swept) == ""


@pytest.mark.parametrize("model", MODELS)
def test_hot_class_sweep_on_recorded_data_is_the_cpp_strategy(model):
    pytest.importorskip("numba")
    from basic_mm_hot import BasicMMHot

    cfg = _config("basic_mm.toml", model)
    grid = {"half_spread_bps": [0.0, 0.3], "quote_qty": [1, 4]}
    hot = fastmm.sweep(cfg, grid, data=BITSTAMP, strategy=BasicMMHot, threads=2, verify=4)
    cpp = fastmm.sweep(cfg, grid, data=BITSTAMP, strategy="basic_mm", threads=2)
    assert [(p, r.outbound_sha256) for p, r in hot] == [(p, r.outbound_sha256) for p, r in cpp]


# xmm quotes on Bitstamp and hedges on Coinbase; under l2_queue it also quotes on Coinbase, whose
# trades fill only under l2_queue.
@pytest.mark.parametrize("model,quote", [("l3_queue", 0), ("l2_queue", 0), ("l2_queue", 1)])
def test_xmm_sweep_points_are_their_own_backtests(model, quote):
    cfg = _config("xmm.toml", model, quote_instrument=quote, hedge_instrument=1 - quote)
    data = [BITSTAMP, COINBASE]
    points = fastmm.sweep(cfg, XMM_GRID, data=data, threads=3, verify=_points(XMM_GRID))
    assert len(points) == _points(XMM_GRID)
    if quote == 1:
        assert _fills(points) > 0
    params, swept = points[0]
    assert _core._result_difference(_alone(cfg, data, "xmm", params), swept) == ""


@pytest.mark.parametrize("name,strategy,grid,data", [
    ("basic_mm.toml", "basic_mm", BASIC_GRID, BITSTAMP),
    ("xmm.toml", "xmm", XMM_GRID, [BITSTAMP, COINBASE]),
])
@pytest.mark.parametrize("model", MODELS)
def test_walk_forward_folds_are_their_own_backtests(name, strategy, grid, data, model):
    cfg = _config(name, model)
    report = fastmm.walk_forward(cfg, grid, folds=3, data=data, threads=3)
    assert len(report["folds"]) == 3
    for fold in report["folds"]:
        assert fold["end_ts"] > fold["start_ts"]
        window = _core._TimeSlice(data, fold["start_ts"], fold["end_ts"])
        assert len(fold["points"]) == _points(grid)
        for params, swept in fold["points"][:: _points(grid) // 2]:
            diff = _core._result_difference(_alone(cfg, window, strategy, params), swept)
            assert diff == "", (fold["start_ts"], params, diff)
