"""Market-data helpers for the numpy input path of :func:`fastmm.run_backtest`."""

from __future__ import annotations

import os
from typing import Dict, Union

import numpy as np

_TYPE_CODES = {"S": 0, "D": 1, "T": 2, "B": 3}
_SIDE_CODES = {"B": 0, "A": 1}


def _decimal_to_raw(text: str) -> int:
    """Exact decimal string -> int with a 1e-8 scale (same rules as the C++ parser)."""
    s = text.strip()
    neg = s.startswith("-")
    if s[:1] in "+-":
        s = s[1:]
    whole, _, frac = s.partition(".")
    if (not whole and not frac) or not (whole or "0").isdigit() or (frac and not frac.isdigit()):
        raise ValueError(f"not a decimal: {text!r}")
    if len(frac) > 8:
        if frac[8:].strip("0"):
            raise ValueError(f"more than 8 decimal places: {text!r}")
        frac = frac[:8]
    raw = int(whole or "0") * 100_000_000 + int(frac.ljust(8, "0") or "0")
    return -raw if neg else raw


def load_csv(path: Union[str, "os.PathLike[str]"]) -> Dict[str, np.ndarray]:
    """Read a ``ts_ns,type,inst,side,price,qty,seq`` CSV (the CsvSource format, e.g. from
    ``tools/gen_synthetic_data.py``) into the column dict :func:`fastmm.run_backtest` accepts.

    Prices and quantities are parsed exactly into raw int64 (1e-8), so the arrays replay
    bit-identically to running the CSV by path.
    """
    ts, typ, inst, side, price, qty, seq = [], [], [], [], [], [], []
    with open(path, encoding="ascii") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("ts"):
                continue
            f = line.split(",")
            if len(f) < 6:
                raise ValueError(f"{path}:{lineno}: expected at least 6 columns")
            try:
                ts.append(int(f[0]))
                typ.append(_TYPE_CODES[f[1]] if f[1] in _TYPE_CODES else int(f[1]))
                inst.append(int(f[2]))
                side.append(_SIDE_CODES[f[3]] if f[3] in _SIDE_CODES else int(f[3]))
                price.append(_decimal_to_raw(f[4]))
                qty.append(_decimal_to_raw(f[5]))
                seq.append(int(f[6]) if len(f) > 6 and f[6] else 0)
            except (KeyError, ValueError) as exc:
                raise ValueError(f"{path}:{lineno}: {exc}") from exc
    return {
        "ts": np.asarray(ts, dtype=np.int64),
        "type": np.asarray(typ, dtype=np.uint8),
        "inst": np.asarray(inst, dtype=np.uint32),
        "side": np.asarray(side, dtype=np.int8),
        "price": np.asarray(price, dtype=np.int64),
        "qty": np.asarray(qty, dtype=np.int64),
        "seq": np.asarray(seq, dtype=np.uint64),
    }
