import threading
import time

import numpy as np
import pytest

import fastmm
from conftest import FIXTURE_FMJ, FIXTURE_SHA


def _short(cfg, seconds=10):
    cfg = cfg.copy()
    cfg.duration_s = seconds
    return cfg


def test_parity_with_cpp_golden_fixture(example_config):
    result = fastmm.run_backtest(example_config, data=str(FIXTURE_FMJ))
    expected = FIXTURE_SHA.read_text().strip()
    assert result.outbound_sha256 == expected
    assert result.md_events == 1000
    assert result.strategy == "basic_mm"


def test_pathlike_data_matches_str(example_config):
    a = fastmm.run_backtest(example_config, data=FIXTURE_FMJ)
    b = fastmm.run_backtest(example_config, data=str(FIXTURE_FMJ))
    assert a.outbound_sha256 == b.outbound_sha256


def test_synthetic_determinism(example_config):
    cfg = _short(example_config)
    a = fastmm.run_backtest(cfg, data="synthetic")
    b = fastmm.run_backtest(cfg, data="synthetic")
    assert a.outbound_messages > 0
    assert a.outbound_sha256 == b.outbound_sha256
    cfg.seed = cfg.seed + 1
    c = fastmm.run_backtest(cfg, data="synthetic")
    assert c.outbound_sha256 != a.outbound_sha256
    assert c.seed == cfg.seed


def test_csv_path_and_numpy_arrays_are_equivalent(example_config, synthetic_csv):
    by_path = fastmm.run_backtest(example_config, data=str(synthetic_csv))
    cols = fastmm.load_csv(synthetic_csv)
    by_arrays = fastmm.run_backtest(example_config, data=cols)
    assert by_path.md_events > 0 and len(by_path.fills["ts"]) > 0
    assert by_arrays.outbound_sha256 == by_path.outbound_sha256
    assert len(by_arrays.fills["ts"]) == len(by_path.fills["ts"])
    assert by_arrays.md_events == by_path.md_events


def test_float64_price_qty_columns_are_accepted(example_config, synthetic_csv):
    cols = fastmm.load_csv(synthetic_csv)
    cols["price"] = cols["price"].astype(np.float64) * 1e-8
    cols["qty"] = cols["qty"].astype(np.float64) * 1e-8
    del cols["seq"]
    r = fastmm.run_backtest(example_config, data=cols)
    assert r.md_events > 0


def _columns(n=4):
    return {
        "ts": np.arange(n, dtype=np.int64) + 1_700_000_000_000_000_000,
        "type": np.full(n, 2, dtype=np.uint8),
        "inst": np.zeros(n, dtype=np.uint32),
        "side": np.zeros(n, dtype=np.int8),
        "price": np.full(n, 6_000_000_000_000, dtype=np.int64),
        "qty": np.full(n, 100_000, dtype=np.int64),
    }


def test_dtype_strictness(example_config):
    cols = _columns()
    cols["price"] = cols["price"].astype(np.float32)
    with pytest.raises(TypeError, match="price"):
        fastmm.run_backtest(example_config, data=cols)

    cols = _columns()
    cols["ts"] = cols["ts"].astype(np.int32)
    with pytest.raises(TypeError, match="ts"):
        fastmm.run_backtest(example_config, data=cols)

    cols = _columns()
    cols["qty"] = np.full(8, 100_000, dtype=np.int64)[::2]  # non-contiguous view
    with pytest.raises(ValueError, match="C-contiguous"):
        fastmm.run_backtest(example_config, data=cols)

    cols = _columns()
    cols["side"] = [0, 0, 0, 0]
    with pytest.raises(TypeError, match="ndarray"):
        fastmm.run_backtest(example_config, data=cols)

    cols = _columns()
    cols["ts"] = cols["ts"].astype(">i8")  # non-native byte order
    with pytest.raises(TypeError, match="ts"):
        fastmm.run_backtest(example_config, data=cols)

    cols = _columns()
    del cols["qty"]
    with pytest.raises(ValueError, match="qty"):
        fastmm.run_backtest(example_config, data=cols)

    cols = _columns()
    cols["type"][1] = 9
    with pytest.raises(ValueError):
        fastmm.run_backtest(example_config, data=cols)

    with pytest.raises(TypeError):
        fastmm.run_backtest(example_config, data=42)
    with pytest.raises(RuntimeError):
        fastmm.run_backtest(example_config, data="no_such_file.fmj")
    with pytest.raises(ValueError, match="unknown strategy"):
        fastmm.run_backtest(example_config, data="synthetic", strategy="nope")


def test_results_are_zero_copy_views(example_config):
    r = fastmm.run_backtest(_short(example_config), data="synthetic")
    assert len(r.fills["price"]) > 0
    a = r.fills["price"]
    b = r.fills["price"]
    assert a is not b
    assert np.shares_memory(a, b)
    assert a.dtype == np.int64 and not a.flags.writeable
    assert np.shares_memory(r.equity["realized"], r.equity["realized"])
    assert np.shares_memory(r.orders["ts"], r.orders["ts"])
    snapshot = a.copy()
    equity_ts = r.equity["ts"]
    n_orders = len(r.orders["cl_ord_id"])
    del r, b
    import gc

    gc.collect()
    np.testing.assert_array_equal(a, snapshot)  # still backed by the C++ result
    assert len(equity_ts) > 0 and np.all(np.diff(equity_ts) > 0)
    assert n_orders > 0


def test_drawdown_pct_needs_initial_capital(example_config):
    cfg = _short(example_config)
    cfg.initial_capital = 0.0
    assert np.isnan(fastmm.run_backtest(cfg, data="synthetic").stats()["max_drawdown_pct"])


def test_stats_and_summaries(example_config, tmp_path):
    r = fastmm.run_backtest(_short(example_config), data="synthetic")
    s = r.stats()
    for key in ("net_pnl", "sharpe_annualized", "max_drawdown", "fill_ratio", "quote_uptime"):
        assert isinstance(s[key], float)
    assert np.isnan(s["sharpe_annualized"])  # not annualised: the run is shorter than 1 day
    assert "n/a" in r.summary_table()
    assert s["max_drawdown_pct"] == pytest.approx(s["max_drawdown"] / 10_000)  # initial_capital
    assert s["fills"] == len(r.fills["ts"])
    e = r.equity
    last = (int(e["realized"][-1]) + int(e["unrealized"][-1]) - int(e["fees"][-1])) * 1e-8
    assert s["net_pnl"] == pytest.approx(last, abs=1e-6)
    assert "net pnl" in r.summary_table()
    assert '"net_pnl"' in r.summary_json()
    r.write_all(tmp_path / "out")
    assert {p.name for p in (tmp_path / "out").iterdir()} >= {
        "equity.csv",
        "fills.csv",
        "orders.csv",
        "summary.json",
    }
    assert r.engine_stats()["events"] > 0
    assert r.transport_stats()["md_delivered"] > 0


def test_gil_is_released_during_backtest(example_config):
    cfg = example_config.copy()
    cfg.fill_model = "matching"
    cfg.duration_s = 900
    out = {}
    started = threading.Event()

    def worker():
        started.set()
        out["result"] = fastmm.run_backtest(cfg, data="synthetic")

    t = threading.Thread(target=worker)
    counter = 0
    max_gap = 0.0
    t.start()
    started.wait()
    t0 = last = time.perf_counter()
    while t.is_alive():
        counter += 1
        now = time.perf_counter()
        max_gap = max(max_gap, now - last)
        last = now
    elapsed = time.perf_counter() - t0
    t.join()
    assert "result" in out
    assert elapsed > 0.05, "backtest too short to observe the GIL"
    assert counter > 1000
    # Holding the GIL would freeze this loop for the whole run.
    assert max_gap < 0.5 * elapsed


def test_sweep_grid_order_and_params(example_config):
    cfg = _short(example_config, 5)
    grid = {"half_spread_bps": [0.01, 0.02], "skew_bps_per_unit": [0.0, 0.01]}
    points = fastmm.sweep(cfg, grid, data="synthetic", threads=2)
    assert [p for p, _ in points] == [
        {"half_spread_bps": 0.01, "skew_bps_per_unit": 0.0},
        {"half_spread_bps": 0.01, "skew_bps_per_unit": 0.01},
        {"half_spread_bps": 0.02, "skew_bps_per_unit": 0.0},
        {"half_spread_bps": 0.02, "skew_bps_per_unit": 0.01},
    ]
    for params, result in points:
        assert isinstance(result, fastmm.BacktestResult)
        assert result.params["half_spread_bps"] == repr(params["half_spread_bps"])
        assert result.params["skew_bps_per_unit"] == repr(params["skew_bps_per_unit"])
        assert result.params["quote_qty"] == "0.002"  # base params kept
    single = cfg.copy()
    single.set_param("half_spread_bps", 0.02)
    single.set_param("skew_bps_per_unit", 0.01)
    assert fastmm.run_backtest(single, data="synthetic").outbound_sha256 == (
        points[3][1].outbound_sha256
    )


def test_sweep_over_numpy_arrays(example_config, synthetic_csv):
    cols = fastmm.load_csv(synthetic_csv)
    grid = {"half_spread_bps": [0.01, 0.03]}
    points = fastmm.sweep(example_config, grid, data=cols, threads=2)
    assert len(points) == 2
    for params, result in points:
        single = example_config.copy()
        single.set_param("half_spread_bps", params["half_spread_bps"])
        assert fastmm.run_backtest(single, data=cols).outbound_sha256 == result.outbound_sha256


def test_sweep_errors(example_config):
    with pytest.raises(TypeError):
        fastmm.sweep(example_config, {"half_spread_bps": "0.1"}, data="synthetic")
    with pytest.raises(ValueError):
        fastmm.sweep(example_config, {"half_spread_bps": []}, data="synthetic")
    with pytest.raises(RuntimeError):
        fastmm.sweep(example_config, {"half_spread_bps": [1]}, data="missing.csv")
    with pytest.raises(ValueError, match="outside"):
        fastmm.sweep(_short(example_config, 1), {"half_spread_bps": [-1]}, data="synthetic")


def test_walk_forward(example_config, synthetic_csv):
    grid = {"half_spread_bps": [0.01, 0.03]}
    rep = fastmm.walk_forward(example_config, grid, folds=3, data=str(synthetic_csv), threads=2)
    assert rep["metric"] == "net_pnl"
    folds = rep["folds"]
    assert len(folds) == 3
    assert folds[0]["chosen"] is None
    for prev, fold in zip(folds, folds[1:]):
        assert fold["start_ts"] == prev["end_ts"]
        assert fold["chosen"] == prev["best"]
        assert fold["in_sample"] == max(prev["scores"])
        assert [p["half_spread_bps"] for p, _ in fold["points"]] == [0.01, 0.03]
        assert fold["out_of_sample"] <= fold["hindsight"] == max(fold["scores"])
    assert rep["table"].startswith("walk-forward: 3 folds, 2 points, metric net_pnl")
    again = fastmm.walk_forward(example_config, grid, folds=3, data=str(synthetic_csv), threads=1)
    assert again["table"] == rep["table"]

    one = fastmm.walk_forward(example_config, grid, folds=1, data=str(synthetic_csv))
    plain = fastmm.sweep(example_config, grid, data=str(synthetic_csv))
    assert [r.outbound_sha256 for _, r in one["folds"][0]["points"]] == [
        r.outbound_sha256 for _, r in plain
    ]
    synthetic = _short(example_config, 6)
    assert len(fastmm.walk_forward(synthetic, grid, folds=2, data="synthetic")["folds"]) == 2
    synthetic.fill_model = "matching"  # the coupled market cannot be cut in time
    with pytest.raises(ValueError, match="l2_queue"):
        fastmm.walk_forward(synthetic, grid, folds=2, data="synthetic")
    with pytest.raises(ValueError, match="metric"):
        fastmm.walk_forward(example_config, grid, folds=2, data=str(synthetic_csv), metric="x")


def test_transport_reject_breakdown_sums_to_total():
    from pathlib import Path

    cfg = fastmm.BacktestConfig.from_toml(
        Path(__file__).resolve().parents[2] / "configs" / "backtest-example.toml"
    )
    cfg.fill_model = "matching"
    cfg.duration_s = 60
    cfg.seed = 7
    t = fastmm.run_backtest(cfg, data="synthetic").transport_stats()
    parts = ("post_only", "level_full", "invalid", "duplicate", "other")
    assert sum(t[f"rejects_{p}"] for p in parts) == t["rejects"]



def test_markouts_match_the_fill_columns(example_config):
    cfg = _short(example_config, 30)
    r = fastmm.run_backtest(cfg, data="synthetic")
    horizons = r.markouts()
    assert [h["label"] for h in horizons] == ["1s", "10s", "1m"]

    f = r.fills
    sign = np.where(f["side"] == 0, 1.0, -1.0)
    price = f["price"] / 1e8
    qty = f["qty"] / 1e8
    mid = f["mid"] / 1e8
    for h in horizons:
        column = f[f"markout_mid_{h['horizon_ns']}ns"] / 1e8
        # 0 means "not measured": the horizon is past the end of the run, or the venue book had
        # only one side there. Those fills stay out of every bucket.
        measured = (column > 0) & (mid > 0)
        assert h["total"]["fills"] == int(measured.sum())
        assert h["excluded_fills"] == len(price) - int(measured.sum())
        assert h["excluded_fills"] == h["excluded_past_end"] + h["excluded_no_mid"]
        markout = (sign * (column - price) * qty)[measured].sum()
        capture = (sign * (mid - price) * qty)[measured].sum()
        assert h["total"]["markout"] == pytest.approx(markout, abs=1e-6)
        assert h["total"]["capture"] == pytest.approx(capture, abs=1e-6)
        assert h["total"]["adverse_selection"] == pytest.approx(capture - markout, abs=1e-6)
        assert h["buy"]["fills"] + h["sell"]["fills"] == h["total"]["fills"]
        assert h["maker"]["fills"] + h["taker"]["fills"] == h["total"]["fills"]
        if h["total"]["notional"] > 0:
            # The C++ side sums exact 1e-8 fixed point, so the two differ by the truncation.
            assert h["total"]["markout_bps"] == pytest.approx(
                1e4 * markout / h["total"]["notional"], rel=1e-4
            )


def test_pnl_decomposition_adds_up(example_config):
    s = fastmm.run_backtest(_short(example_config, 30), data="synthetic").stats()
    net = s["spread_capture"] + s["mid_drift"] - s["fees_paid"] + s["rebates_received"]
    assert s["decomposition_net"] == pytest.approx(net, abs=1e-9)
    assert s["decomposition_residual"] == pytest.approx(s["net_pnl"] - net, abs=1e-9)
    assert abs(s["decomposition_residual"]) < 1e-6
    # Realistic fees: the example config charges Binance spot VIP 0 on both sides.
    assert s["fees_paid"] > 0 and s["rebates_received"] == 0.0
    assert s["quotes_filled"] <= s["quotes_placed"] == s["orders"]


def test_markouts_can_be_turned_off(example_config):
    cfg = _short(example_config)
    cfg.markout_horizons_s = []
    r = fastmm.run_backtest(cfg, data="synthetic")
    assert r.markouts() == []
    assert not any(k.startswith("markout_mid_") for k in r.fills)
    # Turning them off does not change what the engine sent.
    assert r.outbound_sha256 == fastmm.run_backtest(_short(example_config), "synthetic").outbound_sha256


def test_per_instrument_fees(example_config):
    cfg = example_config.copy()
    cfg.set_instrument_fees(0, maker_bps=-0.5, taker_bps=3.0)
    assert cfg.instrument_fees(0) == (-0.5, 3.0)
    rebate = fastmm.run_backtest(_short(cfg, 20), data="synthetic").stats()
    fee = fastmm.run_backtest(_short(example_config, 20), data="synthetic").stats()
    assert rebate["rebates_received"] > 0 and rebate["fees_paid"] == 0.0
    assert fee["fees_paid"] > 0 and fee["rebates_received"] == 0.0
    # The rebate is the whole difference: the fills themselves are identical.
    assert rebate["spread_capture"] == pytest.approx(fee["spread_capture"], abs=1e-9)
    assert rebate["net_pnl"] > fee["net_pnl"]
