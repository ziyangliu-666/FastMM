#!/usr/bin/env python3
"""Records BTC/USD on Bitstamp (every order) and Coinbase (L2 and trades) from their public feeds
and writes the two market-data files of this directory.

  python3 tests/fixtures/btcusd/make_fixtures.py record RAW_DIR SECONDS   (needs `websockets`)
  python3 tests/fixtures/btcusd/make_fixtures.py convert RAW_DIR [START_OFFSET_S] [SECONDS]

record keeps every frame of wss://ws.bitstamp.net (live_orders_btcusd, live_trades_btcusd), one
REST order book with order ids (/api/v2/order_book/btcusd/?group=2) taken 3 s in, and every frame
of wss://ws-feed.exchange.coinbase.com (level2_batch, matches on BTC-USD), each line a JSON object
with the receive time.

convert writes, for the window that starts START_OFFSET_S (default 380) after the Bitstamp book
and lasts SECONDS (default 120), in units of 0.001 BTC (one ITCH share; prices in USD per 0.001
BTC):

* bitstamp-btcusd.itch: Nasdaq TotalView-ITCH 5.0 (a 2-byte big-endian length before each
  message), the symbol BTCUSD, the orders within 10 bps of the book's first mid. The book at the
  start of the window is added first, in queue order; each order the stream creates is an Add
  Order, a fill (amount_traded grows) an Order Executed, a reduction an Order Cancel, a deletion
  an Order Delete. A bid's price is rounded down and an ask's up to
  0.0001 USD a share (0.1 USD a BTC), and quantities to whole shares; an order of under half a
  share, an order that took liquidity (an aggressor of live_trades) and an order that would cross
  the book are left out with all their events. Timestamps: the venue's microtimestamp, as
  nanoseconds after midnight New York.
* coinbase-btcusd.csv: the CSV source's rows (ts_ns,type,inst,side,price,qty,seq), instrument 1:
  the five best levels a side as a snapshot at the start and their changes, and the trades with
  the taker's side, at the venue's times.
"""
import json
import pathlib
import struct
import sys
import time
from datetime import datetime, timezone
from decimal import Decimal
from zoneinfo import ZoneInfo

HERE = pathlib.Path(__file__).resolve().parent
BAND = Decimal("0.001")
SHARE = Decimal("0.001")  # BTC
DEPTH = 5  # Coinbase levels a side


def record(out_dir: pathlib.Path, seconds: float) -> None:
    import asyncio
    import urllib.request

    import websockets

    def frame(f, obj):
        f.write(json.dumps({"rx": time.time_ns(), **obj}) + "\n")

    async def bitstamp(f):
        async with websockets.connect("wss://ws.bitstamp.net", max_size=2**26) as ws:
            for ch in ("live_orders_btcusd", "live_trades_btcusd"):
                await ws.send(json.dumps({"event": "bts:subscribe", "data": {"channel": ch}}))
            t0 = time.time()
            book = False
            while time.time() - t0 < seconds:
                try:
                    m = await asyncio.wait_for(ws.recv(), 5)
                except asyncio.TimeoutError:
                    continue
                frame(f, {"m": json.loads(m)})
                if not book and time.time() - t0 > 3:
                    book = True
                    url = "https://www.bitstamp.net/api/v2/order_book/btcusd/?group=2"
                    body = await asyncio.get_running_loop().run_in_executor(
                        None, lambda: urllib.request.urlopen(url, timeout=20).read())
                    frame(f, {"snapshot": json.loads(body)})

    async def coinbase(f):
        async with websockets.connect("wss://ws-feed.exchange.coinbase.com", max_size=2**26) as ws:
            await ws.send(json.dumps({"type": "subscribe", "product_ids": ["BTC-USD"],
                                      "channels": ["level2_batch", "matches"]}))
            t0 = time.time()
            while time.time() - t0 < seconds:
                try:
                    m = await asyncio.wait_for(ws.recv(), 5)
                except asyncio.TimeoutError:
                    continue
                frame(f, {"m": json.loads(m)})

    async def main():
        with open(out_dir / "bitstamp.jsonl", "w") as a, open(out_dir / "coinbase.jsonl", "w") as b:
            await asyncio.gather(bitstamp(a), coinbase(b))

    asyncio.run(main())


def lines(path: pathlib.Path):
    for line in open(path):
        try:
            yield json.loads(line)
        except json.JSONDecodeError:  # the last line of a recording cut short
            return


def shares(amount: str) -> int:
    return int((Decimal(amount) / SHARE).to_integral_value())


class Itch:
    """ITCH 5.0 messages, stock locate 1."""

    def __init__(self, midnight_ns: int):
        self.midnight = midnight_ns
        self.out = bytearray()
        self.match = 0

    def _put(self, kind: bytes, ts_ns: int, body: bytes) -> None:
        t = ts_ns - self.midnight
        msg = kind + struct.pack(">HH", 1, 0) + t.to_bytes(6, "big") + body
        self.out += struct.pack(">H", len(msg)) + msg

    def directory(self, ts, symbol):
        self._put(b"R", ts, symbol.ljust(8).encode() + b"Q" + b"N" + struct.pack(">I", 1)
                  + b"N" + b"C" + b"Z " + b"P" + b"N" + b" " + b"N" + b" " + b"N"
                  + struct.pack(">I", 0) + b"N")

    def add(self, ts, ref, buy, n, symbol, price):
        self._put(b"A", ts, struct.pack(">QcI8sI", ref, b"B" if buy else b"S", n,
                                        symbol.ljust(8).encode(), price))

    def executed(self, ts, ref, n):
        self.match += 1
        self._put(b"E", ts, struct.pack(">QIQ", ref, n, self.match))

    def cancel(self, ts, ref, n):
        self._put(b"X", ts, struct.pack(">QI", ref, n))

    def delete(self, ts, ref):
        self._put(b"D", ts, struct.pack(">Q", ref))


def price_units(price: str, buy: bool) -> int:
    """USD a BTC -> 0.0001 USD a share (0.1 USD a BTC), bids down and asks up."""
    tenths = Decimal(price) * 10
    return int(tenths.to_integral_value(rounding="ROUND_FLOOR" if buy else "ROUND_CEILING"))


def convert_bitstamp(raw: pathlib.Path, start_off: float, seconds: float):
    frames = list(lines(raw / "bitstamp.jsonl"))
    snap = next(f["snapshot"] for f in frames if "snapshot" in f)
    snap_us = int(snap["microtimestamp"])
    events = []
    aggressors = set()
    for i, f in enumerate(frames):
        m = f.get("m", {})
        ev = m.get("event")
        if ev == "trade":
            d = m["data"]
            aggressors.add(d["buy_order_id"] if d["type"] == 0 else d["sell_order_id"])
        elif ev in ("order_created", "order_changed", "order_deleted"):
            events.append((int(m["data"]["microtimestamp"]), i, ev, m["data"]))
    events.sort(key=lambda e: (e[0], e[1]))
    mid = (Decimal(snap["bids"][0][0]) + Decimal(snap["asks"][0][0])) / 2
    lo, hi = mid * (1 - BAND), mid * (1 + BAND)
    begin_us = snap_us + int(start_off * 1e6)
    end_us = begin_us + int(seconds * 1e6)
    day = datetime.fromtimestamp(begin_us / 1e6, ZoneInfo("America/New_York")).date()
    midnight = datetime(day.year, day.month, day.day, tzinfo=ZoneInfo("America/New_York"))
    itch = Itch(int(midnight.timestamp()) * 1_000_000_000)

    # id -> [buy, price units, shares left, amount traded in shares]
    book = {}

    def touch(buy):
        prices = [o[1] for o in book.values() if o[0] == buy]
        if not prices:
            return None
        return max(prices) if buy else min(prices)

    def crosses(buy, px):
        other = touch(not buy)
        return other is not None and (px >= other if buy else px <= other)

    emitting = False  # the events before the window build the book without messages

    def emit(fn, *args):
        if emitting:
            fn(*args)

    for side, buy in (("bids", True), ("asks", False)):
        for price, amount, oid in snap[side]:
            n = shares(amount)
            if n <= 0 or not (lo <= Decimal(price) <= hi):
                continue
            book[int(oid)] = [buy, price_units(price, buy), n, 0]

    def start(ts_ns):
        itch.directory(ts_ns, "BTCUSD")
        for side in (True, False):
            for oid, o in book.items():
                if o[0] == side:
                    itch.add(ts_ns, oid, o[0], o[2], "BTCUSD", o[1])

    for us, _, ev, d in events:
        if us <= snap_us:
            continue
        if us > end_us:
            break
        ts = us * 1000
        if not emitting and us >= begin_us:
            emitting = True
            start(ts)
        oid = int(d["id"])
        buy = d["order_type"] == 0
        if ev == "order_created":
            n = shares(d["amount_str"])
            px = price_units(d["price_str"], buy)
            if (oid in book or oid in aggressors or n <= 0 or crosses(buy, px)
                    or not (lo <= Decimal(d["price_str"]) <= hi)):
                continue
            book[oid] = [buy, px, n, shares(d["amount_traded"])]
            emit(itch.add, ts, oid, buy, n, "BTCUSD", px)
            continue
        o = book.get(oid)
        if o is None:
            continue
        traded = shares(d["amount_traded"])
        done = min(o[2], max(0, traded - o[3]))
        o[3] = traded
        if done > 0:
            o[2] -= done
            emit(itch.executed, ts, oid, done)
        if ev == "order_deleted":
            if o[2] > 0:
                emit(itch.delete, ts, oid)
            del book[oid]
            continue
        left = shares(d["amount_str"])
        if price_units(d["price_str"], buy) != o[1]:  # a new price: a new order
            if o[2] > 0:
                emit(itch.delete, ts, oid)
            del book[oid]
            continue
        if left < o[2]:
            emit(itch.cancel, ts, oid, o[2] - left)
            o[2] = left
        if o[2] == 0:
            del book[oid]
    return bytes(itch.out), begin_us, end_us


def iso_ns(s: str) -> int:
    t = datetime.strptime(s.rstrip("Z")[:26], "%Y-%m-%dT%H:%M:%S.%f").replace(tzinfo=timezone.utc)
    return int(t.timestamp()) * 1_000_000_000 + t.microsecond * 1000


def per_share(price: str) -> str:
    return str((Decimal(price) * SHARE).normalize())


def qty_shares(size: str) -> str:
    q = (Decimal(size) / SHARE).normalize()
    return "0" if q == 0 else format(q, "f")


def top(book: dict, side: str) -> dict:
    prices = sorted(book[side], reverse=side == "B")[:DEPTH]
    return {p: book[side][p] for p in prices}


def convert_coinbase(raw: pathlib.Path, begin_us: int, end_us: int) -> str:
    begin, end = begin_us * 1000, end_us * 1000
    book = {"B": {}, "A": {}}
    shown = {"B": {}, "A": {}}
    rows = []
    started = False
    seq = 0
    for f in lines(raw / "coinbase.jsonl"):
        m = f["m"]
        kind = m.get("type")
        if kind == "snapshot":
            for side, key in (("B", "bids"), ("A", "asks")):
                book[side] = {Decimal(p): q for p, q in m[key]}
            continue
        if kind not in ("l2update", "match", "last_match") or not book["B"]:
            continue
        ts = iso_ns(m["time"])
        if ts > end:
            break
        if not started and ts >= begin:
            started = True
            seq += 1
            for side in ("B", "A"):
                shown[side] = top(book, side)
                for p, q in shown[side].items():
                    rows.append(f"{ts},S,1,{side},{per_share(str(p))},{qty_shares(q)},{seq}")
        if kind == "l2update":
            for side_name, p, q in m["changes"]:
                side = "B" if side_name == "buy" else "A"
                if Decimal(q) == 0:
                    book[side].pop(Decimal(p), None)
                else:
                    book[side][Decimal(p)] = q
            if not started:
                continue
            seq += 1
            for side in ("B", "A"):
                now = top(book, side)
                for p in shown[side]:
                    if p not in now:
                        rows.append(f"{ts},D,1,{side},{per_share(str(p))},0,{seq}")
                for p, q in now.items():
                    if shown[side].get(p) != q:
                        rows.append(f"{ts},D,1,{side},{per_share(str(p))},{qty_shares(q)},{seq}")
                shown[side] = now
        elif kind == "match" and started:
            taker = "A" if m["side"] == "buy" else "B"  # side is the maker's
            rows.append(f"{ts},T,1,{taker},{per_share(m['price'])},{qty_shares(m['size'])},0")
    return "ts_ns,type,inst,side,price,qty,seq\n" + "\n".join(rows) + "\n"


def main() -> None:
    if len(sys.argv) >= 4 and sys.argv[1] == "record":
        record(pathlib.Path(sys.argv[2]), float(sys.argv[3]))
        return
    if len(sys.argv) >= 3 and sys.argv[1] == "convert":
        raw = pathlib.Path(sys.argv[2])
        start_off = float(sys.argv[3]) if len(sys.argv) > 3 else 380.0
        seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 120.0
        itch, begin_us, end_us = convert_bitstamp(raw, start_off, seconds)
        (HERE / "bitstamp-btcusd.itch").write_bytes(itch)
        (HERE / "coinbase-btcusd.csv").write_text(convert_coinbase(raw, begin_us, end_us))
        print(f"window {datetime.fromtimestamp(begin_us / 1e6, timezone.utc).isoformat()} + "
              f"{(end_us - begin_us) / 1e6:.0f} s")
        return
    print(__doc__)
    sys.exit(2)


if __name__ == "__main__":
    main()
