"""numpy mirror of the structs in include/fastmm/strategies/slow_channel.hpp, checked against
fastmm._core at import."""

from __future__ import annotations

from typing import Dict

import numpy as np

from .. import _core

STATE_DTYPE = np.dtype(
    [
        ("book_ts_ns", "<i8"),
        ("best_bid_raw", "<i8"),
        ("best_bid_qty_raw", "<i8"),
        ("best_ask_raw", "<i8"),
        ("best_ask_qty_raw", "<i8"),
        ("mid_raw", "<i8"),
        ("position_raw", "<i8"),
        ("avg_price_raw", "<i8"),
        ("realized_pnl_raw", "<i8"),
        ("unrealized_pnl_raw", "<i8"),
        ("fees_raw", "<i8"),
        ("bid_open_qty_raw", "<i8"),
        ("ask_open_qty_raw", "<i8"),
        ("param_ts_ns", "<i8"),
        ("param_seq", "<u8"),
        ("fills", "<u4"),
        ("book_valid", "u1"),
        ("quoting", "u1"),
        ("_pad", "V2"),
    ]
)

ROW_DTYPE = np.dtype(
    [
        ("ts_ns", "<i8"),
        ("bid_raw", "<i8"),
        ("bid_qty_raw", "<i8"),
        ("ask_raw", "<i8"),
        ("ask_qty_raw", "<i8"),
        ("trade_price_raw", "<i8"),
        ("trade_qty_raw", "<i8"),
        ("kind", "u1"),
        ("side", "u1"),
        ("_pad", "V6"),
    ]
)

FILL_DTYPE = np.dtype(
    [
        ("seq", "<u8"),
        ("ts_ns", "<i8"),
        ("price_raw", "<i8"),
        ("qty_raw", "<i8"),
        ("fee_raw", "<i8"),
        ("position_raw", "<i8"),
        ("instrument", "<u4"),
        ("side", "u1"),
        ("maker", "u1"),
        ("_pad", "V10"),
    ]
)

_LAYOUT: Dict = _core._slow_abi()

MAX_FIELDS: int = _LAYOUT["max_fields"]
FAILURE_EXCEPTION: int = _LAYOUT["failure_exception"]
FAILURE_TIMEOUT: int = _LAYOUT["failure_timeout"]
FAILURE_FILLS_OVERFLOW: int = _LAYOUT["failure_fills_overflow"]
FAILURE_THREAD_EXITED: int = _LAYOUT["failure_thread_exited"]
FAILURE_NAMES = {
    0: "none",
    FAILURE_EXCEPTION: "exception",
    FAILURE_TIMEOUT: "timeout",
    FAILURE_FILLS_OVERFLOW: "fills overflow",
    FAILURE_THREAD_EXITED: "thread exited",
}


def _check() -> None:
    for key, dt in (("state", STATE_DTYPE), ("row", ROW_DTYPE), ("fill", FILL_DTYPE)):
        layout = _LAYOUT[key]
        if dt.itemsize != layout["__size__"]:
            raise ImportError(f"fastmm: slow channel ABI mismatch: {key} is {layout['__size__']} "
                              f"bytes in fastmm._core, {dt.itemsize} in fastmm._slow.abi")
        for name, offset in layout.items():
            if name == "__size__":
                continue
            if name not in dt.fields or dt.fields[name][1] != offset:
                raise ImportError(f"fastmm: slow channel ABI mismatch at {key}.{name}")


_check()
