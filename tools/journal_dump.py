#!/usr/bin/env python3
"""Dump a FastMM journal (.fmj): header, instrument table, blocks and events.

Layouts follow include/fastmm/core/journal.hpp and include/fastmm/core/messages.hpp:
  file   := FileHeader (256 B, crc32c over the first 252) | Instrument[count] (128 B each)
            | Block* | trailer block (flags & 1)
  block  := BlockHeader (64 B, crc32c of the payload) | messages
  message:= EventHeader (64 B: len, type, version, venue, flags, instrument, reserved, seq,
            venue_seq, exch_ts, recv_ts, t0_cycles, t1_delta, t2_delta) | body

Prices / quantities / notionals are int64 with a 1e-8 scale and are printed as exact decimals.

usage: journal_dump.py file.fmj [--first 20] [--type Trade] [--no-crc]
"""
import argparse
import struct
import sys
from collections import Counter

SCALE = 100_000_000

EVENT_TYPES = [
    "Padding", "BookDelta", "BookSnapshot", "BookTicker", "Trade", "OrderAck", "OrderReject",
    "OrderCancelAck", "OrderCancelReject", "OrderFill", "OrderExpired", "PositionUpdate", "Timer",
    "Control", "ConnectionState", "Reconcile", "LatencySample", "OutNewOrder", "OutCancel",
    "OutReplace", "OrderAddL3", "OrderExecL3", "OrderCancelL3", "OrderReplaceL3",
]
SIDES = {0: "Buy", 1: "Sell"}
ORDER_TYPES = {0: "Limit", 1: "Market", 2: "PostOnly"}
TIFS = {0: "Gtc", 1: "Ioc", 2: "Fok", 3: "Day"}
LIQUIDITY = {0: "Unknown", 1: "Maker", 2: "Taker"}
FLAG_NAMES = [(1, "synthetic"), (2, "replayed"), (4, "snapshot"), (8, "outbound")]

HEADER = struct.Struct("<4sIIIQqQqQQQII32s140sI")  # 256 bytes
BLOCK = struct.Struct("<4sIQQIII28s")  # 64 bytes
EVENT = struct.Struct("<IBBBBIIQQqqQII")  # 64 bytes
INSTRUMENT_HOT = struct.Struct("<IBBBBqqqqqqq")  # 64 bytes
assert HEADER.size == 256 and BLOCK.size == 64 and EVENT.size == 64 and INSTRUMENT_HOT.size == 64


def _crc32c_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


CRC_TABLE = _crc32c_table()


def crc32c(data: bytes) -> int:
    c = 0xFFFFFFFF
    for b in data:
        c = CRC_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def dec(raw: int) -> str:
    sign = "-" if raw < 0 else ""
    raw = abs(raw)
    whole, frac = divmod(raw, SCALE)
    if frac == 0:
        return f"{sign}{whole}"
    return f"{sign}{whole}.{frac:08d}".rstrip("0")


def fixed_string(buf: bytes, capacity: int) -> str:
    """FixedString<N>: N chars followed by a one-byte length."""
    n = min(buf[capacity], capacity)
    return buf[:n].decode("ascii", "replace")


def cl_ord_id(v: int) -> str:
    return f"{v}(epoch {v >> 32}, seq {v & 0xFFFFFFFF})" if v else "0"


def flags_str(f: int) -> str:
    names = [n for bit, n in FLAG_NAMES if f & bit]
    return "|".join(names) if names else "-"


def decode_body(type_name: str, body: bytes) -> str:
    q = lambda off: struct.unpack_from("<q", body, off)[0]  # noqa: E731
    u64 = lambda off: struct.unpack_from("<Q", body, off)[0]  # noqa: E731
    if type_name in ("BookDelta", "BookSnapshot"):
        nb, na, first, last, prev = struct.unpack_from("<IIQQQ", body, 0)
        levels = [struct.unpack_from("<qq", body, 32 + 16 * i) for i in range(nb + na)]
        fmt_lv = lambda lv: " ".join(f"{dec(p)}x{dec(s)}" for p, s in lv[:5]) + (" ..." if len(lv) > 5 else "")  # noqa: E731
        return (f"U={first} u={last} pu={prev} bids({nb})=[{fmt_lv(levels[:nb])}] "
                f"asks({na})=[{fmt_lv(levels[nb:])}]")
    if type_name == "Trade":
        return f"px={dec(q(0))} qty={dec(q(8))} trade_id={u64(16)} aggressor={SIDES.get(body[24], body[24])}"
    if type_name == "BookTicker":
        return f"bid={dec(q(0))}x{dec(q(8))} ask={dec(q(16))}x{dec(q(24))}"
    if type_name in ("OrderAck", "OrderCancelAck", "OrderExpired"):
        s = f"cl_ord_id={cl_ord_id(u64(0))} venue_order_id={fixed_string(body[8:49], 40)}"
        if type_name != "OrderAck":
            s += f" cum_qty={dec(q(56))}"
        return s
    if type_name in ("OrderReject", "OrderCancelReject"):
        reason, code = body[8], struct.unpack_from("<i", body, 12)[0]
        return f"cl_ord_id={cl_ord_id(u64(0))} reason={reason} code={code} text='{fixed_string(body[16:57], 40)}'"
    if type_name == "OrderFill":
        return (f"cl_ord_id={cl_ord_id(u64(0))} exec_id={fixed_string(body[49:90], 40)} px={dec(q(96))} "
                f"qty={dec(q(104))} cum={dec(q(112))} leaves={dec(q(120))} fee={dec(q(128))} fee_asset={('quote', 'base', 'other')[body[138]] if body[138] < 3 else body[138]} "
                f"side={SIDES.get(body[136], body[136])} liq={LIQUIDITY.get(body[137], body[137])}")
    if type_name == "Timer":
        return f"timer_id={struct.unpack_from('<I', body, 0)[0]} user_data={u64(8):#x} fire_ts={q(16)}"
    if type_name == "LatencySample":
        return f"interval={body[0]} count={u64(8)} p50={q(16)} p90={q(24)} p99={q(32)} p999={q(40)} max={q(48)} ns"
    if type_name == "OutNewOrder":
        return (f"cl_ord_id={cl_ord_id(u64(0))} px={dec(q(8))} qty={dec(q(16))} side={SIDES.get(body[24], body[24])} "
                f"type={ORDER_TYPES.get(body[25], body[25])} tif={TIFS.get(body[26], body[26])} reduce_only={body[27]}")
    if type_name == "OutCancel":
        return f"cl_ord_id={cl_ord_id(u64(0))} venue_order_id={fixed_string(body[8:49], 40)}"
    if type_name == "OutReplace":
        return (f"cl_ord_id={cl_ord_id(u64(0))} orig={cl_ord_id(u64(8))} "
                f"venue_order_id={fixed_string(body[16:57], 40)} px={dec(q(64))} qty={dec(q(72))}")
    if type_name == "PositionUpdate":
        return f"qty={dec(q(0))} avg_px={dec(q(8))} realized={dec(q(16))} unrealized={dec(q(24))} fees={dec(q(32))}"
    return f"({len(body)} body bytes)"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("journal")
    ap.add_argument("--first", type=int, default=20, help="print the first N events (0 = none)")
    ap.add_argument("--type", help="only print events of this type (counts are still global)")
    ap.add_argument("--no-crc", action="store_true", help="skip crc32c verification")
    args = ap.parse_args()

    data = open(args.journal, "rb").read()
    if len(data) < HEADER.size:
        print("error: file shorter than the 256-byte header", file=sys.stderr)
        return 2
    (magic, version, header_bytes, inst_count, session_id, start_ts, tsc0, tsc_ns0, ns_per_cycle,
     config_hash, rng_seed, msg_version, block_bytes, strategy, _reserved, hdr_crc) = HEADER.unpack_from(data, 0)
    if magic != b"FMJ1":
        print(f"error: bad magic {magic!r}", file=sys.stderr)
        return 2
    crc_ok = args.no_crc or crc32c(data[:252]) == hdr_crc
    print(f"file            {args.journal} ({len(data)} bytes)")
    print(f"version         {version} (messages v{msg_version}), block size {block_bytes}")
    print(f"session_id      {session_id}   rng_seed {rng_seed}   config_hash {config_hash:#018x}")
    name = strategy.split(b"\0", 1)[0].decode("ascii", "replace")
    print(f"strategy        '{name}'")
    print(f"start_ts        {start_ts}   tsc0 {tsc0} tsc_ns0 {tsc_ns0} ns/cycle q32 {ns_per_cycle}")
    print(f"header crc32c   {'ok' if crc_ok else 'MISMATCH'}")

    print(f"instruments     {inst_count}")
    for i in range(inst_count):
        off = HEADER.size + 128 * i
        iid, venue, asset, flags, decimals, tick, lot, min_qty, max_qty, min_notional, mult, max_notional = \
            INSTRUMENT_HOT.unpack_from(data, off)
        symbol = fixed_string(data[off + 80:off + 101], 20)
        print(f"  #{iid} venue {venue} {symbol:<12} tick {dec(tick)} lot {dec(lot)} min_qty {dec(min_qty)} "
              f"min_notional {dec(min_notional)} multiplier {dec(mult)} asset_class {asset} flags {flags:#x}")

    counts = Counter()
    blocks = bad_blocks = printed = 0
    trailer = False
    off = header_bytes
    while off + BLOCK.size <= len(data):
        bmagic, byte_len, seq_first, seq_last, count, bcrc, bflags, _ = BLOCK.unpack_from(data, off)
        if bmagic != b"FMJB":
            print(f"warning: bad block magic at offset {off}; stopping", file=sys.stderr)
            break
        payload = data[off + BLOCK.size:off + BLOCK.size + byte_len]
        if len(payload) < byte_len:
            print(f"warning: truncated block at offset {off}", file=sys.stderr)
            break
        if bflags & 1:
            trailer = True
        if not args.no_crc and crc32c(payload) != bcrc:
            bad_blocks += 1
            print(f"warning: block at offset {off} (seq {seq_first}..{seq_last}) crc mismatch", file=sys.stderr)
        blocks += 1
        pos = 0
        while pos + EVENT.size <= len(payload):
            (length, etype, ver, venue, flags, inst, _res, seq, venue_seq, exch_ts, recv_ts, t0, t1, t2) = \
                EVENT.unpack_from(payload, pos)
            if length < EVENT.size or pos + length > len(payload):
                print(f"warning: bad message length {length} in block at offset {off}", file=sys.stderr)
                break
            name = EVENT_TYPES[etype] if etype < len(EVENT_TYPES) else f"type{etype}"
            direction = "out" if flags & 8 else "in"
            counts[(name, direction)] += 1
            if printed < args.first and (args.type is None or args.type == name):
                body = payload[pos + EVENT.size:pos + length]
                print(f"#{seq:<7} {name:<17} {direction:<3} inst {inst} venue {venue} flags {flags_str(flags)} "
                      f"exch_ts {exch_ts} recv_ts {recv_ts} venue_seq {venue_seq}\n          {decode_body(name, body)}")
                printed += 1
            pos += length
        off += BLOCK.size + byte_len
        if trailer:
            break

    total = sum(counts.values())
    print(f"blocks          {blocks} (crc mismatches {bad_blocks}), trailer {'present' if trailer else 'MISSING'}")
    print(f"events          {total}")
    for (name, direction), n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"  {name:<18} {direction:<3} {n}")
    return 0 if crc_ok and bad_blocks == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
