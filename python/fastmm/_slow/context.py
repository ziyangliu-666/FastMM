"""What slow methods see: ctx.snapshot(), ctx.recent(), ctx.fills() and ctx.publish()."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Iterator, List, NamedTuple, Optional, Sequence, Tuple, Union

import numpy as np

from . import abi

if TYPE_CHECKING:  # pragma: no cover
    from .runner import SlowRunner

SCALE = 1e-8
TOP = 0
"""ctx.recent() row kind: the top of book changed."""
TRADE = 1
"""ctx.recent() row kind: a trade, with the top of book at that time."""

RECENT_DTYPE = np.dtype(
    [
        ("ts_ns", "<i8"),
        ("kind", "u1"),
        ("side", "u1"),
        ("bid", "<f8"),
        ("bid_qty", "<f8"),
        ("ask", "<f8"),
        ("ask_qty", "<f8"),
        ("mid", "<f8"),
        ("price", "<f8"),
        ("qty", "<f8"),
    ]
)

FILLS_DTYPE = np.dtype(
    [
        ("seq", "<u8"),
        ("ts_ns", "<i8"),
        ("instrument", "<u4"),
        ("side", "u1"),
        ("maker", "?"),
        ("price", "<f8"),
        ("qty", "<f8"),
        ("fee", "<f8"),
        ("position", "<f8"),
    ]
)

InstrumentArg = Union[int, str]


def instrument_index(inst: Any, symbols: Sequence[str]) -> int:
    """An instrument id from an int id or a symbol; ValueError when it is not in the table."""
    if isinstance(inst, (bool, np.bool_)):
        raise ValueError(f"instrument must be an int id or a symbol, got {inst!r}")
    if isinstance(inst, (int, np.integer)):
        k = int(inst)
        if 0 <= k < len(symbols):
            return k
        raise ValueError(f"instrument {k} is not in the instrument table ({len(symbols)} instruments)")
    if isinstance(inst, str):
        try:
            return list(symbols).index(inst)
        except ValueError:
            raise ValueError(f"instrument '{inst}' is not in the instrument table") from None
    raise ValueError(f"instrument must be an int id or a symbol, got {type(inst).__name__}")


class InstrumentSnapshot(NamedTuple):
    """One instrument in a Snapshot: prices in quote currency, quantities in base units, PnL and fees
    in quote currency."""

    instrument: int
    symbol: str
    book_valid: bool
    book_ts_ns: int
    bid: float
    bid_qty: float
    ask: float
    ask_qty: float
    mid: float
    position: float
    avg_price: float
    realized_pnl: float
    unrealized_pnl: float
    fees: float
    bid_open_qty: float
    ask_open_qty: float
    fills: int
    quoting: bool
    param_seq: int
    param_age_ms: Optional[float]


class Snapshot:
    """A copy of the latest state the engine published. ``snap[inst]`` takes an id or a symbol."""

    __slots__ = ("ts_ns", "version", "age_ms", "quoting_enabled", "killed", "instruments")

    def __init__(self, ts_ns: int, version: int, age_ms: Optional[float], quoting_enabled: bool,
                 killed: bool, instruments: Tuple[InstrumentSnapshot, ...]) -> None:
        self.ts_ns = ts_ns
        self.version = version
        self.age_ms = age_ms
        self.quoting_enabled = quoting_enabled
        self.killed = killed
        self.instruments = instruments

    def __getitem__(self, inst: InstrumentArg) -> InstrumentSnapshot:
        return self.instruments[instrument_index(inst, [i.symbol for i in self.instruments])]

    def __len__(self) -> int:
        return len(self.instruments)

    def __iter__(self) -> Iterator[InstrumentSnapshot]:
        return iter(self.instruments)

    def __repr__(self) -> str:
        return (f"<Snapshot ts_ns={self.ts_ns} version={self.version} age_ms={self.age_ms} "
                f"instruments={len(self.instruments)}>")


class Recent(NamedTuple):
    """ctx.recent(): rows oldest first, and the rows written since the previous call for the
    instrument that left the window before this one."""

    rows: np.ndarray
    dropped: int


def snapshot_from(raw: Tuple[int, int, bool, bool, bytes], symbols: Sequence[str],
                  now_ns: int) -> Snapshot:
    ts_ns, version, quoting_enabled, killed, data = raw
    states = np.frombuffer(data, abi.STATE_DTYPE)
    out = []
    for k, s in enumerate(states):
        param_ts = int(s["param_ts_ns"])
        out.append(InstrumentSnapshot(
            instrument=k,
            symbol=symbols[k] if k < len(symbols) else "",
            book_valid=bool(s["book_valid"]),
            book_ts_ns=int(s["book_ts_ns"]),
            bid=int(s["best_bid_raw"]) * SCALE,
            bid_qty=int(s["best_bid_qty_raw"]) * SCALE,
            ask=int(s["best_ask_raw"]) * SCALE,
            ask_qty=int(s["best_ask_qty_raw"]) * SCALE,
            mid=int(s["mid_raw"]) * SCALE,
            position=int(s["position_raw"]) * SCALE,
            avg_price=int(s["avg_price_raw"]) * SCALE,
            realized_pnl=int(s["realized_pnl_raw"]) * SCALE,
            unrealized_pnl=int(s["unrealized_pnl_raw"]) * SCALE,
            fees=int(s["fees_raw"]) * SCALE,
            bid_open_qty=int(s["bid_open_qty_raw"]) * SCALE,
            ask_open_qty=int(s["ask_open_qty_raw"]) * SCALE,
            fills=int(s["fills"]),
            quoting=bool(s["quoting"]),
            param_seq=int(s["param_seq"]),
            param_age_ms=(now_ns - param_ts) / 1e6 if param_ts else None,
        ))
    age = (now_ns - ts_ns) / 1e6 if ts_ns else None
    return Snapshot(ts_ns, version, age, bool(quoting_enabled), bool(killed), tuple(out))


def recent_from(data: bytes, dropped: int) -> Recent:
    raw = np.frombuffer(data, abi.ROW_DTYPE)
    rows = np.zeros(len(raw), RECENT_DTYPE)
    rows["ts_ns"] = raw["ts_ns"]
    rows["kind"] = raw["kind"]
    rows["side"] = raw["side"]
    bid = raw["bid_raw"] * SCALE
    ask = raw["ask_raw"] * SCALE
    rows["bid"] = bid
    rows["ask"] = ask
    rows["bid_qty"] = raw["bid_qty_raw"] * SCALE
    rows["ask_qty"] = raw["ask_qty_raw"] * SCALE
    rows["mid"] = np.where((bid > 0) & (ask > 0), (bid + ask) * 0.5, np.nan)
    rows["price"] = raw["trade_price_raw"] * SCALE
    rows["qty"] = raw["trade_qty_raw"] * SCALE
    return Recent(rows, int(dropped))


def fills_from(chunks: List[np.ndarray]) -> np.ndarray:
    if not chunks:
        return np.zeros(0, FILLS_DTYPE)
    raw = np.concatenate(chunks)
    out = np.zeros(len(raw), FILLS_DTYPE)
    out["seq"] = raw["seq"]
    out["ts_ns"] = raw["ts_ns"]
    out["instrument"] = raw["instrument"]
    out["side"] = raw["side"]
    out["maker"] = raw["maker"] != 0
    out["price"] = raw["price_raw"] * SCALE
    out["qty"] = raw["qty_raw"] * SCALE
    out["fee"] = raw["fee_raw"] * SCALE
    out["position"] = raw["position_raw"] * SCALE
    return out


class SlowContext:
    """The ctx of on_start, on_stop and @fastmm.every methods. Use it from the slow thread; other
    threads publish with strategy.publish()."""

    def __init__(self, runner: "SlowRunner") -> None:
        self._runner = runner

    @property
    def now_ns(self) -> int:
        """Session time of this call, ns: simulated in backtests, the wall clock live."""
        return self._runner.now_ns

    @property
    def instruments(self) -> Tuple[str, ...]:
        """Instrument symbols by id."""
        return tuple(self._runner.channel.symbols)

    def snapshot(self) -> Snapshot:
        """The latest state the engine published, per instrument."""
        return self._runner.snapshot()

    def recent(self, inst: InstrumentArg) -> Recent:
        """Recent top-of-book changes and trades of one instrument (a numpy array, oldest first) and
        the count of rows that did not fit."""
        return self._runner.recent(inst)

    def fills(self) -> np.ndarray:
        """The fills since the previous call, as a numpy array with a ``seq`` column."""
        return self._runner.take_fills()

    def publish(self, inst: Optional[InstrumentArg] = None, **values: Any) -> bool:
        """New values for any subset of the parameters, applied together at one engine event;
        ``inst=None`` applies them to every instrument. ValueError when a name or value is invalid;
        False when the session did not take the update."""
        publisher = self._runner.publisher
        if publisher is None:
            return False
        return publisher.publish(inst, values)
