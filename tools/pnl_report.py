#!/usr/bin/env python3
"""Session PnL report from a FastMM journal (.fmj), with optional account reconciliation.

Reads the OrderFill events of a journal and prints, per instrument:
  * a per-hour table: fills, maker share, bought and sold quantity, traded notional, fees in the
    quote asset and the inventory at the end of the hour;
  * the session totals and the trading PnL marked at the last fill price (or at the end snapshot's
    mid when one is given).

Commission is booked the way the engine books it (include/fastmm/core/engine.hpp, on_fill):
  * fee_asset quote: the fee is a quote amount and is taken from cash;
  * fee_asset base:  the fee is in base units, so a buy receives qty - fee and a sell delivers
    qty + fee; the inventory already carries the cost, which is fee * price in quote terms and is
    shown in the fee column only (it is not subtracted a second time);
  * fee_asset other: not convertible (for example BNB); counted and left out, like the engine does.

With --engine-log, the final "realized_pnl=... unrealized_pnl=... fees=..." line of fastmm-live is
parsed and compared with the journal figure.

With --start and --end account snapshots (JSON objects), the account's equity change is split into
the revaluation of the starting inventory and trading:
    equity change = start_base * (end_mid - start_mid) + trading
Snapshot keys: "base" and "quote" balances (or the lower-case asset names given by --base-asset
and --quote-asset, e.g. "btc" and "usdt"), "mid", and optionally "equity" or "equity_<quote>"
(computed as quote + base * mid when absent), "utc" and "open_orders".

usage:
  pnl_report.py runs/<session>/session.fmj
  pnl_report.py session.fmj --start equity_start.json --end equity_end.json --engine-log engine.log
  pnl_report.py --self-test
"""
import argparse
import json
import math
import os
import re
import struct
import sys
import tempfile
from collections import OrderedDict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import journal_dump as jd  # noqa: E402

SCALE = jd.SCALE
NS_PER_HOUR = 3_600_000_000_000


def nz(x: float, digits: int = 8) -> float:
    """x without a negative sign on a value that prints as zero."""
    return 0.0 if abs(x) < 0.5 * 10 ** -digits else x


class InstrumentBook:
    """Cash, inventory and per-hour buckets of one instrument, in float units of the asset."""

    def __init__(self, symbol: str, multiplier: float):
        self.symbol = symbol
        self.multiplier = multiplier
        self.fills = 0
        self.maker = 0
        self.inventory = 0.0   # base units, commission in base already removed
        self.cash = 0.0        # quote units, commission in quote already removed
        self.fees_quote = 0.0  # every convertible commission valued in quote
        self.fees_base = 0.0   # commission charged in base units
        self.fees_quote_asset = 0.0  # commission charged in quote units
        self.fees_other = 0    # fills whose commission could not be converted
        self.notional = 0.0
        self.buy_qty = 0.0
        self.sell_qty = 0.0
        self.last_px = 0.0
        self.first_ts = None
        self.last_ts = None
        self.hours = OrderedDict()

    def on_fill(self, ts: int, t0: int, f: dict) -> None:
        px = f["px_raw"] / SCALE
        qty = f["qty_raw"] / SCALE
        fee = f["fee_raw"] / SCALE
        buy = f["side"] == "Buy"
        mult = self.multiplier
        if f["fee_asset"] == "base":
            held = qty - fee if buy else qty + fee
            fee_q = fee * px * mult
            self.fees_base += fee
            cash_fee = 0.0
        elif f["fee_asset"] == "quote":
            held = qty
            fee_q = fee
            self.fees_quote_asset += fee
            cash_fee = fee
        else:
            held = qty
            fee_q = 0.0
            self.fees_other += 1
            cash_fee = 0.0
        notional = px * qty * mult
        if buy:
            self.inventory += held
            self.cash -= notional
            self.buy_qty += qty
        else:
            self.inventory -= held
            self.cash += notional
            self.sell_qty += qty
        self.cash -= cash_fee
        self.fees_quote += fee_q
        self.notional += notional
        self.fills += 1
        maker = f["liq"] == "Maker"
        self.maker += 1 if maker else 0
        self.last_px = px
        self.first_ts = ts if self.first_ts is None else self.first_ts
        self.last_ts = ts
        h = max(0, (ts - t0) // NS_PER_HOUR)
        b = self.hours.setdefault(h, {"fills": 0, "maker": 0, "buy": 0.0, "sell": 0.0, "notional": 0.0,
                                      "fees": 0.0, "inventory": 0.0})
        b["fills"] += 1
        b["maker"] += 1 if maker else 0
        b["buy" if buy else "sell"] += qty
        b["notional"] += notional
        b["fees"] += fee_q
        b["inventory"] = self.inventory

    def pnl(self, mark: float) -> float:
        """Trading PnL in quote units: cash plus inventory at `mark`; commission is already in both."""
        return self.cash + self.inventory * mark * self.multiplier


def read_fills(path: str, verify_crc: bool):
    data = open(path, "rb").read()
    hdr = jd.parse_header(data, verify_crc=verify_crc)
    instruments = {i["id"]: i for i in jd.parse_instruments(data, hdr)}
    books = OrderedDict()
    stats = jd.BlockStats()
    events = 0
    for ev, body in jd.iter_events(data, hdr, verify_crc=verify_crc, stats=stats):
        events += 1
        if ev["type"] != "OrderFill":
            continue
        inst = instruments.get(ev["instrument"])
        symbol = inst["symbol"] if inst else f"instrument {ev['instrument']}"
        mult = inst["multiplier_raw"] / SCALE if inst and inst["multiplier_raw"] > 0 else 1.0
        book = books.setdefault(ev["instrument"], InstrumentBook(symbol, mult))
        ts = ev["recv_ts"] if ev["recv_ts"] > 0 else ev["exch_ts"]
        book.on_fill(ts, hdr["start_ts"], jd.fill_fields(body))
    return hdr, instruments, books, stats, events


def parse_engine_log(path: str) -> dict:
    """Last summary lines of a fastmm-live log (apps/fastmm-live/live_backend.cpp)."""
    text = Path(path).read_text(errors="replace")
    out = {}
    m = re.findall(r"fastmm-live: realized_pnl=(\S+) unrealized_pnl=(\S+) fees=(\S+)", text)
    if m:
        out["realized"], out["unrealized"], out["fees"] = (float(x) for x in m[-1])
    m = re.findall(r"fastmm-live: events=(\d+) .*? fills=(\d+) risk_rejects=(\d+)", text)
    if m:
        out["events"], out["fills"], out["risk_rejects"] = (int(x) for x in m[-1])
    m = re.findall(r"fastmm-live: shutdown took (\d+) ms \(cancel_all (ok|FAILED)\)", text)
    if m:
        out["shutdown_ms"], out["cancel_all"] = int(m[-1][0]), m[-1][1]
    m = re.findall(r"fastmm-live: shutting down \(([^)]*)\)", text)
    if m:
        out["reason"] = m[-1]
    return out


def load_snapshot(path: str, base_asset: str, quote_asset: str) -> dict:
    raw = json.load(open(path))

    def pick(*keys):
        for k in keys:
            if k in raw and raw[k] is not None:
                return float(raw[k])
        raise KeyError(f"{path}: none of {', '.join(keys)} present")

    base = pick("base", base_asset.lower(), base_asset.upper())
    quote = pick("quote", quote_asset.lower(), quote_asset.upper())
    mid = pick("mid")
    try:
        equity = pick("equity", f"equity_{quote_asset.lower()}")
    except KeyError:
        equity = quote + base * mid
    return {"base": base, "quote": quote, "mid": mid, "equity": equity, "utc": raw.get("utc", "?"),
            "open_orders": raw.get("open_orders")}


def fmt_hours(t0: int, ts: int) -> str:
    return f"{(ts - t0) / NS_PER_HOUR:.2f} h" if ts is not None else "-"


def report(args, out=sys.stdout) -> int:
    hdr, instruments, books, stats, events = read_fills(args.journal, verify_crc=args.verify_crc)
    base_a, quote_a = args.base_asset, args.quote_asset
    p = lambda *a: print(*a, file=out)  # noqa: E731
    p(f"journal {args.journal}")
    p(f"strategy '{hdr['strategy']}', session {hdr['session_id']}, {events} events, {stats.blocks} blocks, "
      f"trailer {'present' if stats.trailer else 'MISSING'}"
      + (f", crc mismatches {stats.bad_blocks}" if args.verify_crc else ""))
    if not books:
        p("no fills")
    for iid, b in books.items():
        p("")
        p(f"{b.symbol} (instrument {iid}): {b.fills} fills from {fmt_hours(hdr['start_ts'], b.first_ts)} "
          f"to {fmt_hours(hdr['start_ts'], b.last_ts)} after the session start")
        p(f"{'hour':>4} {'fills':>6} {'maker':>7} {'bought':>12} {'sold':>12} {'notional':>12} "
          f"{'fees':>10} {'inventory':>12}")
        for h, x in b.hours.items():
            p(f"{h:>4} {x['fills']:>6} {100.0 * x['maker'] / x['fills']:>6.1f}% {x['buy']:>12.6f} {x['sell']:>12.6f} "
              f"{x['notional']:>12.2f} {x['fees']:>10.4f} {x['inventory']:>+12.6f}")
        p(f"total: maker share {100.0 * b.maker / b.fills:.1f}%, notional {b.notional:.2f} {quote_a}, "
          f"fees {b.fees_quote:.4f} {quote_a} ({1e4 * b.fees_quote / b.notional:.2f} bps of notional; "
          f"charged {b.fees_base:.8f} {base_a} and {b.fees_quote_asset:.4f} {quote_a}"
          + (f"; {b.fees_other} fills in another asset not included" if b.fees_other else "") + ")")
        p(f"inventory change {b.inventory:+.8f} {base_a}, cash change {b.cash:+.4f} {quote_a} (both after fees)")
        p(f"trading PnL at the last fill price {b.last_px:.2f}: {b.pnl(b.last_px):+.4f} {quote_a}")

    engine = parse_engine_log(args.engine_log) if args.engine_log else {}
    if args.engine_log:
        p("")
        if "realized" in engine:
            net = engine["realized"] + engine["unrealized"] - engine["fees"]
            p(f"engine: realized {engine['realized']:+.4f} unrealized {engine['unrealized']:+.4f} "
              f"fees {engine['fees']:.4f} net {net:+.4f} {quote_a} (marked at the engine's last mid)")
            engine["net"] = net
        else:
            p(f"engine: no final PnL line in {args.engine_log}")
        if "fills" in engine:
            total = sum(b.fills for b in books.values())
            p(f"engine fills {engine['fills']}, journal fills {total}"
              + ("" if engine["fills"] == total else "  <- MISMATCH"))
        if "cancel_all" in engine:
            p(f"shutdown ({engine.get('reason', '?')}) took {engine['shutdown_ms']} ms, cancel_all {engine['cancel_all']}")

    rc = 0
    if args.start or args.end:
        if not (args.start and args.end):
            print("error: --start and --end are needed together", file=sys.stderr)
            return 2
        if len(books) > 1:
            print("error: account reconciliation needs a journal with fills on one instrument", file=sys.stderr)
            return 2
        s = load_snapshot(args.start, base_a, quote_a)
        e = load_snapshot(args.end, base_a, quote_a)
        b = next(iter(books.values()), InstrumentBook("-", 1.0))
        p("")
        p(f"account start {s['utc']}: {base_a} {s['base']:.8f}, {quote_a} {s['quote']:.4f}, mid {s['mid']:.2f}, "
          f"equity {s['equity']:.4f} {quote_a}")
        p(f"account end   {e['utc']}: {base_a} {e['base']:.8f}, {quote_a} {e['quote']:.4f}, mid {e['mid']:.2f}, "
          f"equity {e['equity']:.4f} {quote_a}"
          + (f", open orders {e['open_orders']}" if e["open_orders"] is not None else ""))
        equity_change = e["equity"] - s["equity"]
        hold = s["base"] * (e["mid"] - s["mid"])
        trading = equity_change - hold
        p(f"equity change {equity_change:+.4f} {quote_a} = starting inventory revaluation {hold:+.4f} "
          f"+ trading {trading:+.4f}")
        d_base = e["base"] - s["base"]
        d_quote = e["quote"] - s["quote"]
        p(f"balance change {base_a} {d_base:+.8f} (journal {b.inventory:+.8f}, unexplained {nz(d_base - b.inventory):+.8f}); "
          f"{quote_a} {d_quote:+.4f} (journal {b.cash:+.4f}, unexplained {nz(d_quote - b.cash):+.4f})")
        journal_pnl = b.pnl(e["mid"])
        p(f"journal trading PnL at the end mid: {journal_pnl:+.4f} {quote_a} "
          f"(difference to the account {nz(trading - journal_pnl):+.4f})")
        if "net" in engine:
            p(f"engine net {engine['net']:+.4f} {quote_a} (difference to the account {nz(trading - engine['net']):+.4f})")
    return rc


# ---- self-test: a synthetic journal with known answers ------------------------------------------

def _event(etype: int, seq: int, inst: int, recv_ts: int, body: bytes, flags: int = 0) -> bytes:
    length = 64 + len(body)
    assert length % 64 == 0
    return jd.EVENT.pack(length, etype, 1, 0, flags, inst, 0, seq, 0, recv_ts, recv_ts, 0, 0, 0) + body


def _fill_body(px: str, qty: str, fee: str, fee_asset: int, side: int, liq: int, n: int) -> bytes:
    def raw(s):
        whole, _, frac = s.partition(".")
        sign = -1 if whole.startswith("-") else 1
        return sign * (abs(int(whole or "0")) * SCALE + int((frac + "0" * 8)[:8] or "0"))
    body = bytearray(192)
    struct.pack_into("<Q", body, 0, (7 << 32) | n)
    exec_id = str(n).encode()
    body[49:49 + len(exec_id)] = exec_id
    body[89] = len(exec_id)
    struct.pack_into("<qqqqq", body, 96, raw(px), raw(qty), raw(qty), 0, raw(fee))
    body[136], body[137], body[138] = side, liq, fee_asset
    return bytes(body)


def write_synthetic_journal(path: str) -> None:
    """Two instruments' worth of header, one symbol with fills in hours 0 and 1, and a trailer."""
    start = 1_789_000_000_000_000_000
    fill = jd.EVENT_TYPES.index("OrderFill")
    trade = jd.EVENT_TYPES.index("Trade")
    events = [
        _event(trade, 1, 0, start + 1, bytes(64)),
        # buy 0.5 @ 100, commission 0.0005 base (the account receives 0.4995)
        _event(fill, 2, 0, start + 10, _fill_body("100", "0.5", "0.0005", 1, 0, 1, 1)),
        # sell 0.3 @ 102, commission 0.0306 quote
        _event(fill, 3, 0, start + NS_PER_HOUR + 5, _fill_body("102", "0.3", "0.0306", 0, 1, 1, 2)),
        # sell 0.1 @ 101 as taker, commission in another asset (ignored)
        _event(fill, 4, 0, start + NS_PER_HOUR + 9, _fill_body("101", "0.1", "0.01", 2, 1, 2, 3)),
    ]
    payload = b"".join(events)
    inst = bytearray(128)
    jd.INSTRUMENT_HOT.pack_into(inst, 0, 0, 0, 0, 1, 8, SCALE // 100, SCALE // 1000, 0, 0, 0, SCALE, 0)
    inst[80:87] = b"TESTUSD"
    inst[100] = 7
    header = bytearray(jd.HEADER.pack(b"FMJ1", 1, 256 + 128, 1, 42, start, 0, 0, 0, 0, 1, 1, 1 << 20,
                                      b"basic_mm", b"", 0))
    struct.pack_into("<I", header, 252, jd.crc32c(bytes(header[:252])))
    block = jd.BLOCK.pack(b"FMJB", len(payload), 1, 4, len(events), jd.crc32c(payload), 0, b"")
    trailer = jd.BLOCK.pack(b"FMJB", 0, 5, 4, 0, jd.crc32c(b""), 1, b"")
    with open(path, "wb") as f:
        f.write(bytes(header) + bytes(inst) + block + payload + trailer)


def self_test() -> int:
    with tempfile.TemporaryDirectory() as d:
        fmj = os.path.join(d, "t.fmj")
        write_synthetic_journal(fmj)
        hdr, instruments, books, stats, events = read_fills(fmj, verify_crc=True)
        assert stats.bad_blocks == 0 and stats.trailer and events == 4, (stats.__dict__, events)
        assert instruments[0]["symbol"] == "TESTUSD"
        b = books[0]
        close = lambda a, x: math.isclose(a, x, rel_tol=0, abs_tol=1e-9)  # noqa: E731
        assert b.fills == 3 and b.maker == 2
        assert list(b.hours) == [0, 1] and b.hours[1]["fills"] == 2
        assert close(b.inventory, 0.4995 - 0.3 - 0.1), b.inventory
        assert close(b.cash, -50.0 + 30.6 - 0.0306 + 10.1), b.cash
        assert close(b.fees_quote, 0.0005 * 100 + 0.0306), b.fees_quote
        assert b.fees_other == 1
        # Marked at 101: cash + inventory * 101. The base commission is inside the inventory and
        # must not be subtracted again (the bug of the first session script).
        assert close(b.pnl(101.0), -9.3306 + 0.0995 * 101.0), b.pnl(101.0)

        log = os.path.join(d, "engine.log")
        Path(log).write_text(
            "x INFO fastmm-live: shutting down (signal)\n"
            "x INFO fastmm-live: events=4 book_updates=0 orders=3 cancels=0 replaces=0 fills=3 risk_rejects=0\n"
            "x INFO fastmm-live: realized_pnl=1.5 unrealized_pnl=-0.25 fees=0.0806 tick_to_trade p50=1 ns p99=2 ns\n"
            "x INFO fastmm-live: shutdown took 12 ms (cancel_all ok)\n")
        eng = parse_engine_log(log)
        assert eng["fills"] == 3 and eng["cancel_all"] == "ok" and eng["reason"] == "signal"
        assert close(eng["realized"] + eng["unrealized"] - eng["fees"], 1.1694)

        start = os.path.join(d, "start.json")
        end = os.path.join(d, "end.json")
        Path(start).write_text(json.dumps({"base": 1.0, "quote": 1000.0, "mid": 100.0}))
        # End balances follow the journal exactly; the mid moved to 101.
        Path(end).write_text(json.dumps({"btc": 1.0 + b.inventory, "usdt": 1000.0 + b.cash, "mid": 101.0}))
        s = load_snapshot(start, "BTC", "USDT")
        e = load_snapshot(end, "BTC", "USDT")
        trading = (e["equity"] - s["equity"]) - s["base"] * (e["mid"] - s["mid"])
        assert close(trading, b.pnl(101.0)), (trading, b.pnl(101.0))

        class A:
            journal, verify_crc, base_asset, quote_asset = fmj, True, "BTC", "USDT"
            engine_log = log
        A.start, A.end = start, end
        import io
        buf = io.StringIO()
        assert report(A, out=buf) == 0
        text = buf.getvalue()
        assert "difference to the account +0.0000" in text, text
    print("pnl_report self-test: ok")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("journal", nargs="?", help="session journal (.fmj)")
    ap.add_argument("--start", help="account snapshot JSON taken before the session")
    ap.add_argument("--end", help="account snapshot JSON taken after the session")
    ap.add_argument("--engine-log", help="fastmm-live log with the final summary lines")
    ap.add_argument("--base-asset", default="BTC", help="base asset name for labels and snapshot keys (default BTC)")
    ap.add_argument("--quote-asset", default="USDT", help="quote asset name for labels and snapshot keys (default USDT)")
    ap.add_argument("--verify-crc", action="store_true", help="verify block checksums (slow on large journals)")
    ap.add_argument("--self-test", action="store_true", help="run the built-in checks on a synthetic journal")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.journal:
        ap.error("a journal is required (or --self-test)")
    try:
        return report(args)
    except (jd.JournalError, OSError, KeyError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
