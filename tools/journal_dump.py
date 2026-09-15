#!/usr/bin/env python3
"""Dump a FastMM journal (.fmj): header, instrument table, blocks and events.

Layouts follow include/fastmm/core/journal.hpp and include/fastmm/core/messages.hpp:
  file   := FileHeader (256 B, crc32c over the first 252) | Instrument[count] (128 B each)
            | effective config TOML (v2, zero-padded to 64 B) | parameter table (v3, zero-padded to 64 B)
            | Block* | trailer block (flags & 1)
  block  := BlockHeader (64 B, crc32c of the payload) | messages
  message:= EventHeader (64 B: len, type, version, venue, flags, instrument, reserved, seq,
            venue_seq, exch_ts, recv_ts, t0_cycles, t1_delta, t2_delta) | body

Format v2: a record flagged engine_time carries the engine clock as an int32 ns delta in `reserved`
from the previous one; EngineTime records carry absolute values (start, finish, overflow). The
dump prints the reconstructed clock as engine_ts. Version 1 files are read as before.

Format v3: the header holds the strategy's parameter table (per schema index: type u8, name length
u8, name), and ParamUpdate records carry (field index, raw int64) pairs, printed with the names.

Prices / quantities / notionals are int64 with a 1e-8 scale and are printed as exact decimals.

usage: journal_dump.py file.fmj [--first 20] [--type Trade] [--no-crc]

parse_header, parse_instruments, iter_events and fill_fields are also used by
tools/pnl_report.py.
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
    "OutReplace", "OrderAddL3", "OrderExecL3", "OrderCancelL3", "OrderReplaceL3", "OptionTicker",
    "EngineTime", "ParamUpdate",
]
PARAM_TYPES = {0: "int", 1: "double", 2: "bool", 3: "decimal", 4: "bps", 5: "ms"}
ENGINE_TIME_KINDS = {0: "sync", 1: "start", 2: "finish"}
SIDES = {0: "Buy", 1: "Sell"}
ORDER_TYPES = {0: "Limit", 1: "Market", 2: "PostOnly"}
TIFS = {0: "Gtc", 1: "Ioc", 2: "Fok", 3: "Day"}
LIQUIDITY = {0: "Unknown", 1: "Maker", 2: "Taker"}
FLAG_NAMES = [(1, "synthetic"), (2, "replayed"), (4, "snapshot"), (8, "outbound"),
              (16, "engine_time"), (32, "dropped")]
FLAG_ENGINE_TIME = 16

HEADER = struct.Struct("<4sIIIQqQqQQQII32sHBBIQIIIIII100sI")  # 256 bytes
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


FEE_ASSETS = ("quote", "base", "other")


def fill_fields(body: bytes) -> dict:
    """OrderFillMsg body (after the 64-byte EventHeader) as a dict of raw fixed-point integers.

    `fee_raw` is in units of `fee_asset`: "quote" (a quote amount), "base" (base-asset units) or
    "other" (not convertible, for example BNB).
    """
    px, qty, cum, leaves, fee = struct.unpack_from("<qqqqq", body, 96)
    return {
        "cl_ord_id": struct.unpack_from("<Q", body, 0)[0],
        "exec_id": fixed_string(body[49:90], 40),
        "px_raw": px, "qty_raw": qty, "cum_raw": cum, "leaves_raw": leaves, "fee_raw": fee,
        "fee_asset": FEE_ASSETS[body[138]] if body[138] < 3 else body[138],
        "side": SIDES.get(body[136], body[136]),
        "liq": LIQUIDITY.get(body[137], body[137]),
    }


def param_value(raw: int, type_name: str) -> str:
    """A ParamUpdate raw value in the parameter's unit."""
    if type_name == "double":
        return repr(struct.unpack("<d", struct.pack("<q", raw))[0])
    if type_name == "decimal":
        return dec(raw)
    if type_name == "bps":
        return dec(raw * 10_000)  # Ratio raw: 1 bp = 10'000
    if type_name == "ms":
        return f"{raw / 1_000_000:g}"
    if type_name == "bool":
        return "true" if raw else "false"
    return str(raw)


def param_update_fields(body: bytes, params) -> str:
    """ParamUpdateMsg body: count, publish_seq and the (field, raw value) pairs by name."""
    count, _pad, publish_seq = struct.unpack_from("<IIQ", body, 0)
    fields = struct.unpack_from("<32H", body, 16)
    values = struct.unpack_from("<32q", body, 128)
    out = []
    for i in range(min(count, 32)):
        idx = fields[i]
        if idx < len(params):
            name, type_name = params[idx]
            out.append(f"{name}={param_value(values[i], type_name)}")
        else:
            out.append(f"#{idx}=raw {values[i]}")
    return f"publish_seq={publish_seq} " + (" ".join(out) if out else "(no fields)")


def decode_body(type_name: str, body: bytes, params=()) -> str:
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
    if type_name == "OptionTicker":
        (mark_iv, bid_iv, ask_iv, delta, gamma, vega, theta, rho, rate) = struct.unpack_from("<9d", body, 24)
        return (f"mark={dec(q(0))} underlying={dec(q(8))} index={dec(q(16))} iv mark={mark_iv:.4f} "
                f"bid={bid_iv:.4f} ask={ask_iv:.4f} delta={delta:.5f} gamma={gamma:.6g} vega={vega:.5f} "
                f"theta={theta:.5f} rho={rho:.5f} r={rate:.4f}")
    if type_name in ("OrderAck", "OrderCancelAck", "OrderExpired"):
        s = f"cl_ord_id={cl_ord_id(u64(0))} venue_order_id={fixed_string(body[8:49], 40)}"
        if type_name != "OrderAck":
            s += f" cum_qty={dec(q(56))}"
        return s
    if type_name in ("OrderReject", "OrderCancelReject"):
        reason, code = body[8], struct.unpack_from("<i", body, 12)[0]
        return f"cl_ord_id={cl_ord_id(u64(0))} reason={reason} code={code} text='{fixed_string(body[16:57], 40)}'"
    if type_name == "OrderFill":
        f = fill_fields(body)
        return (f"cl_ord_id={cl_ord_id(f['cl_ord_id'])} exec_id={f['exec_id']} px={dec(f['px_raw'])} "
                f"qty={dec(f['qty_raw'])} cum={dec(f['cum_raw'])} leaves={dec(f['leaves_raw'])} fee={dec(f['fee_raw'])} "
                f"fee_asset={f['fee_asset']} side={f['side']} liq={f['liq']}")
    if type_name == "Timer":
        engine = " engine (max_param_age)" if body[4] else ""
        return f"timer_id={struct.unpack_from('<I', body, 0)[0]} user_data={u64(8):#x} fire_ts={q(16)}{engine}"
    if type_name == "ParamUpdate":
        return param_update_fields(body, params)
    if type_name == "EngineTime":
        return f"kind={ENGINE_TIME_KINDS.get(body[8], body[8])} engine_ts={q(0)}"
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


class JournalError(Exception):
    """The file is not a readable FastMM journal."""


def parse_header(data: bytes, verify_crc: bool = True) -> dict:
    if len(data) < HEADER.size:
        raise JournalError("file shorter than the 256-byte header")
    (magic, version, header_bytes, inst_count, session_id, start_ts, tsc0, tsc_ns0, ns_per_cycle,
     config_hash, rng_seed, msg_version, block_bytes, strategy, session_epoch, quoting_enabled,
     header_flags, config_bytes, replace_venues, _config_crc, param_count, param_bytes, _param_crc,
     meta_bytes, _meta_crc, _reserved,
     hdr_crc) = HEADER.unpack_from(data, 0)
    if magic != b"FMJ1":
        raise JournalError(f"bad magic {magic!r}")
    if version < 2:
        session_epoch = quoting_enabled = header_flags = config_bytes = replace_venues = 0
    if version < 3:
        param_count = param_bytes = meta_bytes = 0
    config_off = HEADER.size + 128 * inst_count
    meta_off = config_off + (config_bytes + 63) // 64 * 64 + (param_bytes + 63) // 64 * 64
    params = []
    off = config_off + (config_bytes + 63) // 64 * 64
    for _ in range(param_count):
        type_id, name_len = data[off], data[off + 1]
        params.append((data[off + 2:off + 2 + name_len].decode("utf-8", "replace"),
                       PARAM_TYPES.get(type_id, f"type{type_id}")))
        off += 2 + name_len
    return {
        "params": params,
        "strategy_meta": data[meta_off:meta_off + meta_bytes].decode("utf-8", "replace"),
        "session": bool(header_flags & 1), "session_epoch": session_epoch,
        "quoting_enabled": bool(quoting_enabled), "replace_venues": replace_venues,
        "config": data[config_off:config_off + config_bytes].decode("utf-8", "replace"),
        "version": version, "header_bytes": header_bytes, "instrument_count": inst_count,
        "session_id": session_id, "start_ts": start_ts, "tsc0": tsc0, "tsc_ns0": tsc_ns0,
        "ns_per_cycle_q32": ns_per_cycle, "config_hash": config_hash, "rng_seed": rng_seed,
        "message_version": msg_version, "block_bytes": block_bytes,
        "strategy": strategy.split(b"\0", 1)[0].decode("ascii", "replace"),
        "crc_ok": (not verify_crc) or crc32c(data[:252]) == hdr_crc,
    }


def parse_instruments(data: bytes, header: dict) -> list:
    out = []
    for i in range(header["instrument_count"]):
        off = HEADER.size + 128 * i
        iid, venue, asset, flags, decimals, tick, lot, min_qty, max_qty, min_notional, mult, max_notional = \
            INSTRUMENT_HOT.unpack_from(data, off)
        out.append({
            "id": iid, "venue": venue, "asset_class": asset, "flags": flags,
            "symbol": fixed_string(data[off + 80:off + 101], 20),
            "tick_raw": tick, "lot_raw": lot, "min_qty_raw": min_qty, "max_qty_raw": max_qty,
            "min_notional_raw": min_notional, "multiplier_raw": mult,
        })
    return out


class BlockStats:
    def __init__(self):
        self.blocks = 0
        self.bad_blocks = 0
        self.trailer = False


def iter_events(data: bytes, header: dict, verify_crc: bool = True, stats: BlockStats = None):
    """Yields (event header dict, body bytes) for every message in file order.

    Damaged blocks are reported on stderr; `stats` (optional) receives the block counts.
    """
    stats = stats if stats is not None else BlockStats()
    off = header["header_bytes"]
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
            stats.trailer = True
        if verify_crc and crc32c(payload) != bcrc:
            stats.bad_blocks += 1
            print(f"warning: block at offset {off} (seq {seq_first}..{seq_last}) crc mismatch", file=sys.stderr)
        stats.blocks += 1
        pos = 0
        while pos + EVENT.size <= len(payload):
            (length, etype, ver, venue, flags, inst, res, seq, venue_seq, exch_ts, recv_ts, t0, t1, t2) = \
                EVENT.unpack_from(payload, pos)
            if length < EVENT.size or pos + length > len(payload):
                print(f"warning: bad message length {length} in block at offset {off}", file=sys.stderr)
                break
            ev = {
                "type": EVENT_TYPES[etype] if etype < len(EVENT_TYPES) else f"type{etype}",
                "version": ver, "venue": venue, "flags": flags, "instrument": inst, "seq": seq,
                "venue_seq": venue_seq, "exch_ts": exch_ts, "recv_ts": recv_ts,
                "engine_delta": struct.unpack("<i", struct.pack("<I", res))[0] if flags & FLAG_ENGINE_TIME else None,
            }
            yield ev, payload[pos + EVENT.size:pos + length]
            pos += length
        off += BLOCK.size + byte_len
        if stats.trailer:
            break


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("journal")
    ap.add_argument("--first", type=int, default=20, help="print the first N events (0 = none)")
    ap.add_argument("--type", help="only print events of this type (counts are still global)")
    ap.add_argument("--no-crc", action="store_true", help="skip crc32c verification")
    args = ap.parse_args()

    data = open(args.journal, "rb").read()
    try:
        hdr = parse_header(data, verify_crc=not args.no_crc)
    except JournalError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    crc_ok = hdr["crc_ok"]
    print(f"file            {args.journal} ({len(data)} bytes)")
    print(f"version         {hdr['version']} (messages v{hdr['message_version']}), block size {hdr['block_bytes']}")
    print(f"session_id      {hdr['session_id']}   rng_seed {hdr['rng_seed']}   config_hash {hdr['config_hash']:#018x}")
    print(f"strategy        '{hdr['strategy']}'")
    print(f"start_ts        {hdr['start_ts']}   tsc0 {hdr['tsc0']} tsc_ns0 {hdr['tsc_ns0']} "
          f"ns/cycle q32 {hdr['ns_per_cycle_q32']}")
    if hdr["session"]:
        print(f"session         epoch {hdr['session_epoch']}   quoting_enabled {hdr['quoting_enabled']}   "
              f"replace_venues {hdr['replace_venues']:#x}")
    if hdr["config"]:
        print(f"config          {len(hdr['config'])} bytes of effective TOML embedded")
    for line in hdr["strategy_meta"].splitlines():
        print(f"strategy meta   {line}")
    if hdr["params"]:
        print("parameters      " + " ".join(f"{i}:{n}({t})" for i, (n, t) in enumerate(hdr["params"])))
    print(f"header crc32c   {'ok' if crc_ok else 'MISMATCH'}")

    print(f"instruments     {hdr['instrument_count']}")
    for inst in parse_instruments(data, hdr):
        print(f"  #{inst['id']} venue {inst['venue']} {inst['symbol']:<12} tick {dec(inst['tick_raw'])} "
              f"lot {dec(inst['lot_raw'])} min_qty {dec(inst['min_qty_raw'])} "
              f"min_notional {dec(inst['min_notional_raw'])} multiplier {dec(inst['multiplier_raw'])} "
              f"asset_class {inst['asset_class']} flags {inst['flags']:#x}")

    counts = Counter()
    printed = 0
    stats = BlockStats()
    engine_ts = None
    for ev, body in iter_events(data, hdr, verify_crc=not args.no_crc, stats=stats):
        name = ev["type"]
        direction = "out" if ev["flags"] & 8 else "in"
        counts[(name, direction)] += 1
        if name == "EngineTime":
            engine_ts = struct.unpack_from("<q", body, 0)[0]
        elif ev["engine_delta"] is not None and engine_ts is not None:
            engine_ts += ev["engine_delta"]
        if printed < args.first and (args.type is None or args.type == name):
            clock = f" engine_ts {engine_ts}" if ev["engine_delta"] is not None else ""
            print(f"#{ev['seq']:<7} {name:<17} {direction:<3} inst {ev['instrument']} venue {ev['venue']} "
                  f"flags {flags_str(ev['flags'])} exch_ts {ev['exch_ts']} recv_ts {ev['recv_ts']}{clock} "
                  f"venue_seq {ev['venue_seq']}\n          {decode_body(name, body, hdr['params'])}")
            printed += 1

    total = sum(counts.values())
    print(f"blocks          {stats.blocks} (crc mismatches {stats.bad_blocks}), "
          f"trailer {'present' if stats.trailer else 'MISSING'}")
    print(f"events          {total}")
    for (name, direction), n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"  {name:<18} {direction:<3} {n}")
    return 0 if crc_ok and stats.bad_blocks == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
