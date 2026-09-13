import numpy as np
import pytest

import fastmm


def test_snapshot_delta_and_features():
    ob = fastmm.OrderBook()
    assert ob.best_bid() is None and ob.mid() is None
    ob.apply_snapshot(
        bids=[(100.0, 1.0), (99.5, 2.0)],
        asks=np.array([[100.5, 3.0], [101.0, 1.0]]),
    )
    assert ob.best_bid() == (100.0, 1.0)
    assert ob.best_ask() == (100.5, 3.0)
    assert ob.mid() == pytest.approx(100.25)
    assert ob.spread() == pytest.approx(0.5)
    assert ob.imbalance(1) == pytest.approx((1 - 3) / 4)
    assert ob.imbalance(2) == pytest.approx((3 - 4) / 7)

    # delete the best bid, add a better one, change an ask
    ob.apply_delta(bids=[(100.0, 0.0), (100.25, 1.5)], asks=[(101.0, 4.0)])
    assert ob.best_bid() == (100.25, 1.5)
    assert ob.bid_depth == 2 and ob.ask_depth == 2
    assert ob.mid() == pytest.approx(100.375)
    assert ob.microprice() == pytest.approx((100.5 * 1.5 + 100.25 * 3.0) / 4.5)
    np.testing.assert_allclose(ob.bids(), [[100.25, 1.5], [99.5, 2.0]])
    np.testing.assert_allclose(ob.asks(1), [[100.5, 3.0]])
    assert ob.asks().dtype == np.float64 and ob.asks().shape == (2, 2)
    assert not ob.crossed

    ob.apply_snapshot(bids=[], asks=[(10.0, 1.0)])
    assert ob.best_bid() is None and ob.mid() is None and ob.bids().shape == (0, 2)


def test_bad_levels():
    ob = fastmm.OrderBook()
    with pytest.raises(ValueError):
        ob.apply_snapshot(bids=np.ones((2, 3)), asks=[])
    with pytest.raises(ValueError):
        ob.apply_delta(bids=[(-1.0, 1.0)])
    with pytest.raises(ValueError):
        ob.imbalance(0)
