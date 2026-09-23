"""fastmm.features / fastmm.evaluate_signal: zero-copy columns and the signal report."""

from __future__ import annotations

import gc

import numpy as np
import pytest

import fastmm

FIXED = 100_000_000


def _tape() -> str:
    """Twelve one-second updates whose mid rises by 1 from 101, on a 0.02 wide book."""
    rows = ["ts_ns,type,inst,side,price,qty,seq"]
    for i in range(12):
        ts = (i + 1) * 1_000_000_000
        rows.append(f"{ts},S,0,B,{100 + i}.99,1,{i + 1}")
        rows.append(f"{ts},S,0,A,{101 + i}.01,2,{i + 1}")
    return "\n".join(rows) + "\n"


@pytest.fixture()
def tape_csv(tmp_path):
    path = tmp_path / "tape.csv"
    path.write_text(_tape())
    return str(path)


def test_columns_and_coverage(tape_csv):
    t = fastmm.features(data=tape_csv, horizons=[1.0, 10.0])
    assert len(t) == 12
    assert t.horizons == [1_000_000_000, 10_000_000_000]
    cols = t.columns
    assert cols["mid"][0] == 101 * FIXED
    assert cols["best_bid"][0] == 100_99_000_000
    assert cols["spread"][0] == 2_000_000
    # Bid 1 against ask 2: imbalance (1 - 2) / 3 == -1/3.
    assert cols["imbalance"][0] == pytest.approx(-FIXED / 3, abs=1)
    fwd = t.forward_mid
    assert set(fwd) == {"1s", "10s"}
    assert fwd["1s"][0] == 102 * FIXED
    assert fwd["10s"][0] == 111 * FIXED
    assert fwd["1s"][-1] == 0  # past the end of the data, unset and counted
    cov = {c["label"]: c for c in t.coverage}
    assert cov["1s"]["resolved"] == 11
    assert cov["1s"]["excluded_past_end"] == 1
    assert cov["10s"]["resolved"] == 2
    assert cov["10s"]["excluded_past_end"] == 10
    assert t.book_updates == 12
    assert "horizon" in t.summary()
    assert t.csv().startswith("ts_ns,inst,mid,")


def test_columns_are_zero_copy_views(tape_csv):
    t = fastmm.features(data=tape_csv, horizons=[1.0])
    a = t.columns["mid"]
    b = t.columns["mid"]
    assert a is not b
    assert np.shares_memory(a, b)
    assert a.dtype == np.int64 and not a.flags.writeable
    assert np.shares_memory(t.forward_mid["1s"], t.forward_mid["1s"])
    snapshot = a.copy()
    del t, b
    gc.collect()
    np.testing.assert_array_equal(a, snapshot)  # still backed by the C++ table


def test_subsample_and_trade_rows(tape_csv):
    every = fastmm.features(data=tape_csv, horizons=[1.0])
    assert len(every) == 12
    coarse = fastmm.features(data=tape_csv, horizons=[1.0], subsample=5.0)
    assert len(coarse) == 3
    assert coarse.skipped_subsample == 9
    none = fastmm.features(
        data=tape_csv, horizons=[1.0], sample_book_updates=False, sample_trades=True
    )
    assert len(none) == 0  # the tape has no trades


def test_evaluate_a_built_in_feature(tape_csv):
    t = fastmm.features(data=tape_csv, horizons=[1.0])
    e = fastmm.evaluate_signal(t, "imbalance", buckets=2, blocks=2)
    assert e["signal"] == "imbalance"
    assert e["rows"] == 12
    h = e["horizons"][0]
    assert h["label"] == "1s"
    assert h["n"] == 11
    assert h["excluded_past_end"] == 1
    assert len(h["buckets"]) == 2
    assert sum(b["n"] for b in h["buckets"]) == 11
    assert "IC (Spearman)" in e["table"]


def test_evaluate_a_user_array(tape_csv):
    t = fastmm.features(data=tape_csv, horizons=[1.0])
    # The mid rises by 1 every second, so a signal equal to the row index ranks the move flat:
    # what matters here is that the array path accepts one value per row.
    values = np.arange(len(t), dtype=np.float64)
    e = fastmm.evaluate_signal(t, values, name="index", blocks=2)
    assert e["signal"] == "index"
    assert e["horizons"][0]["n"] == 11
    # Every bucket's two sides average to the half spread: the forward move cancels.
    for b in e["horizons"][0]["buckets"]:
        assert (b["buy_bps"] + b["sell_bps"]) / 2 == pytest.approx(b["half_spread_bps"])


def test_evaluate_rejects_bad_values(tape_csv):
    t = fastmm.features(data=tape_csv, horizons=[1.0])
    with pytest.raises(ValueError):
        fastmm.evaluate_signal(t, np.zeros(3, dtype=np.float64))
    with pytest.raises(TypeError):
        fastmm.evaluate_signal(t, np.zeros(len(t), dtype=np.int64))
    with pytest.raises(ValueError):
        fastmm.evaluate_signal(t, "not_a_feature")
    with pytest.raises(ValueError):
        fastmm.features(data=tape_csv, horizons=[0.0])


def test_forward_mid_matches_the_backtest_at_every_fill(example_config, synthetic_csv):
    """The cross-check of docs/explanation/signal-research.md, on a synthetic tape."""
    cfg = example_config
    cfg.fill_model = "l2_queue"
    cfg.markout_horizons_s = [1.0]
    r = fastmm.run_backtest(cfg, data=str(synthetic_csv), strategy="basic_mm")
    fills = r.fills
    if len(fills["ts"]) == 0:
        pytest.skip("the synthetic tape produced no fills")
    t = fastmm.features(
        data=str(synthetic_csv),
        horizons=[1.0],
        sample_book_updates=False,
        sample_trades=True,
    )
    by_ts = {int(ts): i for i, ts in enumerate(t.columns["ts"])}
    mid = t.columns["mid"]
    fwd = t.forward_mid["1s"]
    markout = fills["markout_mid_1000000000ns"]
    for k, ts in enumerate(fills["ts"]):
        i = by_ts[int(ts)]
        assert mid[i] == fills["mid"][k]
        assert fwd[i] == markout[k]
