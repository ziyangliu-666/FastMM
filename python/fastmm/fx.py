"""Exact int64 fixed-point helpers for hot hooks: the C++ operators, bit for bit.

Raw values use the 1e-8 scale of ``Price::raw`` and ``Qty::raw``; a Ratio raw value is 1e-8 too, so
1 bp is ``RATIO_PER_BP`` (10,000). Every function works in plain Python and inside hot hooks.

    tdiv(a, b)                C++ integer division, truncating toward zero
    mul_ratio(v, r)           Fixed * Ratio: 128-bit product, one truncation toward zero
    bps_ratio(bps_raw)        a parameter in bps (its _raw field) as a Ratio raw value
    round_price(p, tick, s)   bids down, asks up to the tick
    round_qty(q, lot)         down to the lot
    to_raw(x)                 float to raw, nearest (half away from zero)
    to_float(raw)             raw to float

Needs numba (``pip install "fastmm[hot]"``).
"""

from __future__ import annotations

import math

try:
    from llvmlite import ir
    from numba import types
    from numba.extending import intrinsic, overload
except ImportError as e:  # pragma: no cover
    raise ImportError('fastmm.fx needs numba; install it with: pip install "fastmm[hot]"') from e

__all__ = [
    "BUY",
    "RATIO_PER_BP",
    "SCALE",
    "SELL",
    "bps_ratio",
    "mul_ratio",
    "round_price",
    "round_qty",
    "tdiv",
    "to_float",
    "to_raw",
]

SCALE = 100_000_000
RATIO_PER_BP = 10_000
BUY = 0
SELL = 1
_MAX_RAW_FLOAT = 9.2e18


def _wrap64(v: int) -> int:
    return ((v + 2**63) % 2**64) - 2**63


def tdiv(a: int, b: int) -> int:
    """a / b truncated toward zero, like C++. ZeroDivisionError when b is 0."""
    if b == 0:
        raise ZeroDivisionError("fastmm.fx.tdiv: division by zero")
    q = abs(a) // abs(b)
    return _wrap64(q if (a < 0) == (b < 0) else -q)


def mul_ratio(v: int, r: int) -> int:
    """Fixed * Ratio: (v * r) / 1e8 through a 128-bit product, truncated toward zero, low 64 bits."""
    p = v * r
    q = abs(p) // SCALE
    return _wrap64(q if p >= 0 else -q)


def bps_ratio(bps_raw: int) -> int:
    """The Ratio raw value of a basis-point amount given as 1e-8 raw (a Param's `_raw` field)."""
    return tdiv(bps_raw, RATIO_PER_BP)


def round_price(p: int, tick: int, side: int) -> int:
    """round_to_tick: BUY floors, SELL ceils to a multiple of tick."""
    floored = (p // tick) * tick
    if side == BUY or floored == p:
        return floored
    return floored + tick


def round_qty(q: int, lot: int) -> int:
    """round_to_lot: floors to a multiple of lot."""
    return (q // lot) * lot


def to_raw(x: float) -> int:
    """A float as raw 1e-8, nearest, halves away from zero. ValueError when not finite or out of
    range."""
    s = x * SCALE
    if not (abs(s) < _MAX_RAW_FLOAT):
        raise ValueError("fastmm.fx.to_raw: value not finite or out of range")
    return int(s + 0.5) if s >= 0 else -int(-s + 0.5)


def to_float(raw: int) -> float:
    return raw / SCALE


# ---- Numba implementations ---------------------------------------------------------------------------

@intrinsic
def _mul_ratio_128(typingctx, v, r):  # type: ignore[no-untyped-def]
    if not (isinstance(v, types.Integer) and isinstance(r, types.Integer)):
        return None
    sig = types.int64(types.int64, types.int64)

    def codegen(context, builder, signature, args):  # type: ignore[no-untyped-def]
        # A 128-bit division would be a __divti3 libcall. Divide the magnitude hi:lo by 1e8 in
        # 64-bit steps (each a multiply-shift, the divisor being constant):
        #   t = (hi % d) << 32 | lo >> 32;  u = (t % d) << 32 | lo & 0xffffffff
        #   |p| / d mod 2^64 = (t / d) << 32 + u / d
        # hi % d < 2^27, so t and u fit in 64 bits; the quotient's bits above 64 are dropped, as the
        # C++ static_cast<std::int64_t> drops them.
        i64 = ir.IntType(64)
        i128 = ir.IntType(128)
        d = ir.Constant(i64, SCALE)
        c32 = ir.Constant(i64, 32)
        a = context.cast(builder, args[0], signature.args[0], types.int64)
        b = context.cast(builder, args[1], signature.args[1], types.int64)
        prod = builder.mul(builder.sext(a, i128), builder.sext(b, i128))
        neg = builder.icmp_signed("<", prod, ir.Constant(i128, 0))
        mag = builder.select(neg, builder.neg(prod), prod)
        hi = builder.trunc(builder.lshr(mag, ir.Constant(i128, 64)), i64)
        lo = builder.trunc(mag, i64)
        t = builder.or_(builder.shl(builder.urem(hi, d), c32), builder.lshr(lo, c32))
        u = builder.or_(builder.shl(builder.urem(t, d), c32),
                        builder.and_(lo, ir.Constant(i64, 0xFFFF_FFFF)))
        q = builder.add(builder.shl(builder.udiv(t, d), c32), builder.udiv(u, d))
        return builder.select(neg, builder.neg(q), q)

    return sig, codegen


@intrinsic
def _sdiv64(typingctx, a, b):  # type: ignore[no-untyped-def]
    if not (isinstance(a, types.Integer) and isinstance(b, types.Integer)):
        return None
    sig = types.int64(types.int64, types.int64)

    def codegen(context, builder, signature, args):  # type: ignore[no-untyped-def]
        x = context.cast(builder, args[0], signature.args[0], types.int64)
        y = context.cast(builder, args[1], signature.args[1], types.int64)
        return builder.sdiv(x, y)

    return sig, codegen


def _require_ints(name: str, *args: object) -> None:
    from numba.core.errors import TypingError

    for a in args:
        if not isinstance(a, (types.Integer, types.Boolean)):
            raise TypingError(f"fastmm.fx.{name} takes int raw values, got {a}")


@overload(tdiv)
def _tdiv_impl(a, b):  # type: ignore[no-untyped-def]
    _require_ints("tdiv", a, b)

    def impl(a, b):  # type: ignore[no-untyped-def]
        if b == 0:
            raise ZeroDivisionError("fastmm.fx.tdiv: division by zero")
        if b == -1:  # INT64_MIN / -1 traps in the CPU
            return -a
        return _sdiv64(a, b)
    return impl


@overload(mul_ratio)
def _mul_ratio_impl(v, r):  # type: ignore[no-untyped-def]
    _require_ints("mul_ratio", v, r)

    def impl(v, r):  # type: ignore[no-untyped-def]
        return _mul_ratio_128(v, r)
    return impl


@overload(bps_ratio)
def _bps_ratio_impl(bps_raw):  # type: ignore[no-untyped-def]
    _require_ints("bps_ratio", bps_raw)

    def impl(bps_raw):  # type: ignore[no-untyped-def]
        return _sdiv64(bps_raw, RATIO_PER_BP)
    return impl


@overload(round_price)
def _round_price_impl(p, tick, side):  # type: ignore[no-untyped-def]
    _require_ints("round_price", p, tick, side)

    def impl(p, tick, side):  # type: ignore[no-untyped-def]
        floored = (p // tick) * tick
        if side == BUY or floored == p:
            return floored
        return floored + tick
    return impl


@overload(round_qty)
def _round_qty_impl(q, lot):  # type: ignore[no-untyped-def]
    _require_ints("round_qty", q, lot)

    def impl(q, lot):  # type: ignore[no-untyped-def]
        return (q // lot) * lot
    return impl


@overload(to_raw)
def _to_raw_impl(x):  # type: ignore[no-untyped-def]
    def impl(x):  # type: ignore[no-untyped-def]
        s = x * SCALE
        if not (abs(s) < _MAX_RAW_FLOAT):
            raise ValueError("fastmm.fx.to_raw: value not finite or out of range")
        if s >= 0:
            return int(s + 0.5)
        return -int(-s + 0.5)
    return impl


@overload(to_float)
def _to_float_impl(raw):  # type: ignore[no-untyped-def]
    def impl(raw):  # type: ignore[no-untyped-def]
        return raw / SCALE
    return impl


assert math.isfinite(_MAX_RAW_FLOAT)
