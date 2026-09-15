"""numpy mirror of include/fastmm/strategies/hot_abi.h, checked against fastmm._core at import."""

from __future__ import annotations

import hashlib
import json

import numpy as np

from .. import _core

VERSION = 1
BOOK_DEPTH = 10
QUOTE_LEVELS = 8

OK = 0
EXCEPTION = 1
FAILED = 2
BAD_VALUE = 3

ACTION_NONE = 0
ACTION_QUOTE = 1
ACTION_PULL = 2

FLAG_UNCROSS = 1
FLAG_KEEP_PASSIVE = 2

CTX_DTYPE = np.dtype(
    [
        ("now_ns", "i8"),
        ("instrument", "i4"),
        ("quoting_enabled", "u1"),
        ("connected", "u1"),
        ("fill_side", "u1"),
        ("fill_maker", "u1"),
        ("tick", "f8"),
        ("lot", "f8"),
        ("min_qty", "f8"),
        ("position", "f8"),
        ("tick_raw", "i8"),
        ("lot_raw", "i8"),
        ("min_qty_raw", "i8"),
        ("position_raw", "i8"),
        ("fill_price", "f8"),
        ("fill_qty", "f8"),
        ("fill_price_raw", "i8"),
        ("fill_qty_raw", "i8"),
        ("action", "i4"),
        ("flags", "i4"),
        ("n_bids", "i4"),
        ("n_asks", "i4"),
        ("status", "i4"),
        ("fail_code", "i4"),
        ("bid_px", "f8", (QUOTE_LEVELS,)),
        ("bid_qty", "f8", (QUOTE_LEVELS,)),
        ("ask_px", "f8", (QUOTE_LEVELS,)),
        ("ask_qty", "f8", (QUOTE_LEVELS,)),
        ("bid_px_raw", "i8", (QUOTE_LEVELS,)),
        ("bid_qty_raw", "i8", (QUOTE_LEVELS,)),
        ("ask_px_raw", "i8", (QUOTE_LEVELS,)),
        ("ask_qty_raw", "i8", (QUOTE_LEVELS,)),
        ("bid_is_raw", "u1", (QUOTE_LEVELS,)),
        ("ask_is_raw", "u1", (QUOTE_LEVELS,)),
    ],
    align=True,
)

BOOK_DTYPE = np.dtype(
    [
        ("ts_ns", "i8"),
        ("valid", "u1"),
        ("n_bids", "i4"),
        ("n_asks", "i4"),
        ("mid", "f8"),
        ("best_bid", "f8"),
        ("best_ask", "f8"),
        ("best_bid_qty", "f8"),
        ("best_ask_qty", "f8"),
        ("mid_raw", "i8"),
        ("best_bid_raw", "i8"),
        ("best_ask_raw", "i8"),
        ("best_bid_qty_raw", "i8"),
        ("best_ask_qty_raw", "i8"),
        ("bid_px", "f8", (BOOK_DEPTH,)),
        ("bid_qty", "f8", (BOOK_DEPTH,)),
        ("ask_px", "f8", (BOOK_DEPTH,)),
        ("ask_qty", "f8", (BOOK_DEPTH,)),
        ("bid_px_raw", "i8", (BOOK_DEPTH,)),
        ("bid_qty_raw", "i8", (BOOK_DEPTH,)),
        ("ask_px_raw", "i8", (BOOK_DEPTH,)),
        ("ask_qty_raw", "i8", (BOOK_DEPTH,)),
    ],
    align=True,
)


def _check(layout: dict) -> None:
    expected = {"version": VERSION, "book_depth": BOOK_DEPTH, "quote_levels": QUOTE_LEVELS}
    for key, value in expected.items():
        if layout[key] != value:
            raise ImportError(f"fastmm: hot ABI {key} is {layout[key]} in fastmm._core and {value} "
                              "in fastmm._hot.abi; reinstall fastmm")
    for key, dt in (("ctx", CTX_DTYPE), ("book", BOOK_DTYPE)):
        c = layout[key]
        names = [n for n in c if n != "__size__"]
        if c["__size__"] != dt.itemsize or sorted(names) != sorted(dt.names):
            raise ImportError(f"fastmm: hot ABI struct {key} differs between fastmm._core and "
                              "fastmm._hot.abi; reinstall fastmm")
        for name in names:
            if dt.fields[name][1] != c[name]:
                raise ImportError(f"fastmm: hot ABI field {key}.{name} is at offset {c[name]} in "
                                  f"fastmm._core and {dt.fields[name][1]} in fastmm._hot.abi")


LAYOUT = _core._hot_abi()
_check(LAYOUT)
TIMER_LIMIT = LAYOUT["timer_limit"]
# Part of the Numba cache key: a hook cached against another layout is never loaded.
ABI_HASH = hashlib.sha256(json.dumps(LAYOUT, sort_keys=True).encode()).hexdigest()[:16]
