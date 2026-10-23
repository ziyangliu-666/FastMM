"""The slice of the .fmj journal a run report reads (docs/reference/journal-format.md).

A session journal holds what the engine consumed and what it sent. The report needs the header
(strategy, session, effective configuration), the fills, the outbound order counts, the rejects
and a price to mark inventory at; everything else is skipped without decoding it.
``tools/journal_dump.py`` is the full dumper.
"""

from __future__ import annotations

import mmap
import struct
from typing import Any, Dict, List, Optional, Tuple

SCALE = 100_000_000
"""Raw fixed-point scale: a raw int64 divided by this is the natural unit."""

_HEADER = struct.Struct("<4sIIIQqQqQQQII32sHBBIQIIIIII100sI")  # 256 bytes
_BLOCK = struct.Struct("<4sIQQIII28s")  # 64 bytes
_EVENT = struct.Struct("<IBBBBIIQQqqQII")  # 64 bytes
_INSTRUMENT = struct.Struct("<IBBBBqqqqqqq")  # first 64 bytes of a 128-byte record
_TICKER = struct.Struct("<q8xq")  # bid, ask
_COUNTS = struct.Struct("<II")
_I64 = struct.Struct("<q")
_FILL = struct.Struct("<qqqqq")  # price, qty, cum, leaves, fee
_LATENCY = struct.Struct("<8xQqqq")

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


def _fixed_string(buf: memoryview, capacity: int) -> str:
    """FixedString<N>: N characters followed by a one-byte length."""
    n = min(buf[capacity], capacity)
    return bytes(buf[:n]).decode("ascii", "replace")


def _header(data: memoryview) -> Dict[str, Any]:
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
        "config": bytes(data[config_off:config_off + config_bytes]).decode("utf-8", "replace"),
        "meta": bytes(data[meta_off:meta_off + meta_bytes]).decode("utf-8", "replace"),
    }


def _instruments(data: memoryview, count: int) -> List[Dict[str, Any]]:
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
    """Header, fills, outbound counts, rejects and mark-price series of a session journal.

    ``marks`` holds ``(ts_ns, instrument, mid)`` per instrument, sampled every
    ``mark_interval_ns`` (the venue's best mid, or the last trade print when the venue publishes
    no book ticker); fills carry the mark of their instrument that was current when they arrived,
    so the marked position is never ahead of the market.
    """
    with open(path, "rb") as fh:
        try:
            data: Any = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        except ValueError:  # an empty file cannot be mapped
            data = b""
    view = memoryview(data)
    try:
        return _read_session(view, mark_interval_ns)
    finally:
        view.release()
        if isinstance(data, mmap.mmap):
            data.close()


def _read_session(data: memoryview, mark_interval_ns: int) -> Dict[str, Any]:
    header = _header(data)
    instruments = _instruments(data, header["instrument_count"])
    fills: List[Dict[str, Any]] = []
    marks: List[Tuple[int, int, float]] = []  # (ts_ns, instrument, mid)
    rejects: Dict[int, int] = {}
    counts = {"orders": 0, "cancels": 0, "replaces": 0, "dropped": 0, "events": 0,
              "rejects": 0, "cancel_rejects": 0, "duplicate_fills": 0}
    latency: Dict[int, Tuple[int, int, int]] = {}  # interval -> (count, p50, p99)
    mids: Dict[int, float] = {}
    last_mark: Dict[int, int] = {}
    # Each execution is counted once, keyed like the engine's OMS (venue exec id, instrument,
    # side): a reconnect replays executions the stream already delivered.
    booked = set()
    first_ts = last_ts = 0

    for ts, etype, inst, eflags, body in _iter_events(data, header["header_bytes"]):
        counts["events"] += 1
        if ts:
            first_ts = first_ts or ts
            last_ts = ts
        mid = None
        if etype == BOOK_TICKER:
            bid, ask = _TICKER.unpack_from(body, 0)
            if bid > 0 and ask > 0:
                mid = (bid + ask) / (2 * SCALE)
        elif etype == BOOK_SNAPSHOT:
            nb, na = _COUNTS.unpack_from(body, 0)
            if nb and na:
                bid = _I64.unpack_from(body, 32)[0]
                ask = _I64.unpack_from(body, 32 + 16 * nb)[0]
                if bid > 0 and ask > 0:
                    mid = (bid + ask) / (2 * SCALE)
        elif etype == TRADE:
            if inst not in mids:
                mid = _I64.unpack_from(body, 0)[0] / SCALE
        elif etype == ORDER_FILL:
            exec_id = bytes(body[49:49 + min(body[89], 40)])
            key = (exec_id, inst, body[136])
            if exec_id and key in booked:
                counts["duplicate_fills"] += 1
                continue
            booked.add(key)
            px, qty, _cum, _leaves, fee = _FILL.unpack_from(body, 96)
            fills.append({
                "ts": ts,
                "instrument": inst,
                "side": _SIDES[body[136]] if body[136] < 2 else "?",
                "liquidity": _LIQUIDITY[body[137]] if body[137] < 3 else "?",
                "price": px / SCALE,
                "qty": qty / SCALE,
                "fee": fee / SCALE,
                "fee_asset": FEE_ASSETS[body[138]] if body[138] < 3 else "?",
                "mid": mids.get(inst),
            })
        elif etype == ORDER_REJECT or etype == CANCEL_REJECT:
            counts["rejects" if etype == ORDER_REJECT else "cancel_rejects"] += 1
            rejects[body[8]] = rejects.get(body[8], 0) + 1
        elif etype in (OUT_NEW, OUT_CANCEL, OUT_REPLACE):
            counts[{OUT_NEW: "orders", OUT_CANCEL: "cancels", OUT_REPLACE: "replaces"}[etype]] += 1
            if eflags & FLAG_DROPPED:
                counts["dropped"] += 1
        elif etype == LATENCY_SAMPLE:
            n, p50, _p90, p99 = _LATENCY.unpack_from(body, 0)
            latency[body[0]] = (n, p50, p99)
        if mid is not None:
            mids[inst] = mid
            if ts - last_mark.get(inst, -mark_interval_ns) >= mark_interval_ns:
                marks.append((ts, inst, mid))
                last_mark[inst] = ts
    for inst, mid in mids.items():
        if last_mark.get(inst) != last_ts:
            marks.append((last_ts, inst, mid))
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


def _iter_events(data: memoryview, start: int):
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
                   data[pos + _EVENT.size:pos + length])  # a memoryview: no copy
            pos += length
        off = payload_end
