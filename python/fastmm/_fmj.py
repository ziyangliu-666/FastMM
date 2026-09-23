"""The slice of the .fmj journal a run report reads (docs/reference/journal-format.md).

A session journal holds what the engine consumed and what it sent. The report needs the header
(strategy, session, effective configuration), the fills, the outbound order counts, the rejects
and a price to mark inventory at; everything else is skipped without decoding it.
``tools/journal_dump.py`` is the full dumper.
"""

from __future__ import annotations

import struct
from typing import Any, Dict, List, Optional, Tuple

SCALE = 100_000_000
"""Raw fixed-point scale: a raw int64 divided by this is the natural unit."""

_HEADER = struct.Struct("<4sIIIQqQqQQQII32sHBBIQIIIIII100sI")  # 256 bytes
_BLOCK = struct.Struct("<4sIQQIII28s")  # 64 bytes
_EVENT = struct.Struct("<IBBBBIIQQqqQII")  # 64 bytes
_INSTRUMENT = struct.Struct("<IBBBBqqqqqqq")  # first 64 bytes of a 128-byte record

# EventType (include/fastmm/core/enums.hpp), the ones this reader decodes.
BOOK_SNAPSHOT, BOOK_TICKER, TRADE = 2, 3, 4
ORDER_REJECT, CANCEL_REJECT, ORDER_FILL = 6, 8, 9
LATENCY_SAMPLE, OUT_NEW, OUT_CANCEL, OUT_REPLACE = 16, 17, 18, 19

FLAG_DROPPED = 32
FEE_ASSETS = ("quote", "base", "other")
_SIDES = ("buy", "sell")
_LIQUIDITY = ("unknown", "maker", "taker")


class JournalError(Exception):
    """The file is not a readable FastMM journal."""


def _fixed_string(buf: bytes, capacity: int) -> str:
    """FixedString<N>: N characters followed by a one-byte length."""
    n = min(buf[capacity], capacity)
    return buf[:n].decode("ascii", "replace")


def _header(data: bytes) -> Dict[str, Any]:
    if len(data) < _HEADER.size:
        raise JournalError("file shorter than the 256-byte header")
    (magic, version, header_bytes, instruments, session_id, start_ts, _tsc0, _tsc_ns0,
     _ns_per_cycle, _config_hash, rng_seed, _msg_version, _block_bytes, strategy, _epoch,
     quoting, flags, config_bytes, _replace, _config_crc, _param_count, param_bytes, _param_crc,
     meta_bytes, _meta_crc, _reserved, _crc) = _HEADER.unpack_from(data, 0)
    if magic != b"FMJ1":
        raise JournalError("not a FastMM journal (bad magic)")
    if version < 2:
        quoting, flags, config_bytes = 1, 0, 0
    if version < 3:
        param_bytes = meta_bytes = 0
    config_off = _HEADER.size + 128 * instruments
    meta_off = config_off + -(-config_bytes // 64) * 64 + -(-param_bytes // 64) * 64
    return {
        "version": version,
        "header_bytes": header_bytes,
        "instrument_count": instruments,
        "session_id": session_id,
        "start_ts": start_ts,
        "rng_seed": rng_seed,
        "strategy": strategy.split(b"\0", 1)[0].decode("ascii", "replace"),
        "quoting_enabled": bool(quoting) if version >= 2 and flags & 1 else True,
        "config": data[config_off:config_off + config_bytes].decode("utf-8", "replace"),
        "meta": data[meta_off:meta_off + meta_bytes].decode("utf-8", "replace"),
    }


def _instruments(data: bytes, count: int) -> List[Dict[str, Any]]:
    out = []
    for i in range(count):
        off = _HEADER.size + 128 * i
        iid, _venue, _asset, _flags, _dec, tick, lot = _INSTRUMENT.unpack_from(data, off)[:7]
        out.append({
            "id": iid,
            "symbol": _fixed_string(data[off + 80:off + 101], 20),
            "tick": tick / SCALE,
            "lot": lot / SCALE,
        })
    return out


def read_session(path: str, mark_interval_ns: int = 1_000_000_000) -> Dict[str, Any]:
    """Header, fills, outbound counts, rejects and a mark-price series of a session journal.

    ``mark_interval_ns`` is the sampling period of the mark price (the venue's best mid, or the
    last trade print when the venue publishes no book ticker); fills always carry the mark that
    was current when they arrived, so the marked position is never ahead of the market.
    """
    with open(path, "rb") as fh:
        data = fh.read()
    header = _header(data)
    instruments = _instruments(data, header["instrument_count"])
    fills: List[Dict[str, Any]] = []
    marks: List[Tuple[int, float]] = []  # (ts_ns, mid) of instrument 0
    rejects: Dict[int, int] = {}
    counts = {"orders": 0, "cancels": 0, "replaces": 0, "dropped": 0, "events": 0,
              "rejects": 0, "cancel_rejects": 0}
    latency: Dict[int, Tuple[int, int, int]] = {}  # interval -> (count, p50, p99)
    mid: Optional[float] = None
    last_mark = 0
    first_ts = last_ts = 0

    for ts, etype, inst, eflags, body in _iter_events(data, header["header_bytes"]):
        counts["events"] += 1
        if ts:
            first_ts = first_ts or ts
            last_ts = ts
        if etype == BOOK_TICKER and inst == 0:
            bid, ask = struct.unpack_from("<q8xq", body, 0)
            if bid > 0 and ask > 0:
                mid = (bid + ask) / (2 * SCALE)
        elif etype == BOOK_SNAPSHOT and inst == 0:
            nb, na = struct.unpack_from("<II", body, 0)
            if nb and na:
                bid = struct.unpack_from("<q", body, 32)[0]
                ask = struct.unpack_from("<q", body, 32 + 16 * nb)[0]
                if bid > 0 and ask > 0:
                    mid = (bid + ask) / (2 * SCALE)
        elif etype == TRADE and inst == 0 and mid is None:
            mid = struct.unpack_from("<q", body, 0)[0] / SCALE
        elif etype == ORDER_FILL:
            px, qty, _cum, _leaves, fee = struct.unpack_from("<qqqqq", body, 96)
            fills.append({
                "ts": ts,
                "instrument": inst,
                "side": _SIDES[body[136]] if body[136] < 2 else "?",
                "liquidity": _LIQUIDITY[body[137]] if body[137] < 3 else "?",
                "price": px / SCALE,
                "qty": qty / SCALE,
                "fee": fee / SCALE,
                "fee_asset": FEE_ASSETS[body[138]] if body[138] < 3 else "?",
                "mid": mid,
            })
        elif etype == ORDER_REJECT or etype == CANCEL_REJECT:
            counts["rejects" if etype == ORDER_REJECT else "cancel_rejects"] += 1
            rejects[body[8]] = rejects.get(body[8], 0) + 1
        elif etype in (OUT_NEW, OUT_CANCEL, OUT_REPLACE):
            counts[{OUT_NEW: "orders", OUT_CANCEL: "cancels", OUT_REPLACE: "replaces"}[etype]] += 1
            if eflags & FLAG_DROPPED:
                counts["dropped"] += 1
        elif etype == LATENCY_SAMPLE:
            n, p50, _p90, p99 = struct.unpack_from("<8xQqqq", body, 0)
            latency[body[0]] = (n, p50, p99)
        if mid is not None and ts - last_mark >= mark_interval_ns:
            marks.append((ts, mid))
            last_mark = ts
    if mid is not None and (not marks or marks[-1][0] != last_ts):
        marks.append((last_ts, mid))
    return {
        "header": header,
        "instruments": instruments,
        "fills": fills,
        "marks": marks,
        "counts": counts,
        "reject_reasons": rejects,
        "latency": latency,
        "first_ts": first_ts,
        "last_ts": last_ts,
    }


def _iter_events(data: bytes, start: int):
    """(ts_ns, type, instrument, flags, body) of every message, in consumption order."""
    off = start
    end = len(data)
    while off + _BLOCK.size <= end:
        magic, byte_len, _first, _last, _count, _crc, _flags, _pad = _BLOCK.unpack_from(data, off)
        if magic != b"FMJB":
            break
        payload_end = off + _BLOCK.size + byte_len
        if payload_end > end:
            break
        pos = off + _BLOCK.size
        while pos + _EVENT.size <= payload_end:
            (length, etype, _ver, _venue, eflags, inst, _res, _seq, _vseq, exch_ts,
             recv_ts, _t0, _t1, _t2) = _EVENT.unpack_from(data, pos)
            if length < _EVENT.size or pos + length > payload_end:
                break
            yield (exch_ts or recv_ts, etype, inst, eflags,
                   data[pos + _EVENT.size:pos + length])
            pos += length
        off = payload_end
