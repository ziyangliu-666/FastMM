"""tools/replay_board.py: the board of a backtest against its live session."""
import importlib.util
from collections import defaultdict
from datetime import timezone
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "replay_board", Path(__file__).resolve().parents[2] / "tools" / "replay_board.py"
)
board = importlib.util.module_from_spec(spec)
spec.loader.exec_module(board)

S = 1_000_000_000
T0 = 1_700_000_040 * S  # the start of a minute
BAR = 60 * S


def series(mids):
    out = defaultdict(lambda: ([], []))
    for sym, values in mids.items():
        for t, m in values:
            out[sym][0].append(t)
            out[sym][1].append(m)
    return out


def test_buckets_wrap_midnight():
    b = board.parse_buckets("night=20-4,day=4-20")
    at = lambda hour: T0 - T0 % (86400 * S) + hour * 3600 * S
    assert board.bucket_of(at(23), b, timezone.utc) == "night"
    assert board.bucket_of(at(3), b, timezone.utc) == "night"
    assert board.bucket_of(at(4), b, timezone.utc) == "day"


def test_score_counts_a_maker_fill_and_marks_the_position():
    s = series({"A": [(T0, 100.0), (T0 + BAR, 101.0), (T0 + 2 * BAR, 102.0)]})
    # bought 2 at 100 in the first bar, 1 held from the start
    rows = [(T0 + 10 * S, "A", 1, 100.0, 2.0, True, 0.5)]
    marks = [[100.0, 101.0, None]]
    b, path = board.score(rows, marks, s, T0, T0 + 2 * BAR, BAR, [("all", 0.0, 24.0)], timezone.utc, {"A": 1.0}, 1.0)
    d = b["all"]
    assert d["bars"] == 2
    assert d["fills"] == 1 and d["notional"] == 200.0 and d["buy"] == 200.0
    assert d["rebate"] == 200.0 * 1e-4
    assert d["mo60"] == 2.0 and d["usd60"] == 200.0
    assert d["usd300"] == 0.0  # no mid at that horizon: not counted
    # first bar: 1 * (101 - 100), the fill 2 * (101 - 100), less the fee, plus the rebate; second bar: 3 * 1
    assert abs(d["money"] - (1.0 + 2.0 - 0.5 + 0.02 + 3.0)) < 1e-9
    assert path[T0]["A"] == 3.0 * 101.0
    assert path[T0 + BAR]["A"] == 3.0 * 102.0
