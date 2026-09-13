#!/usr/bin/env python3
"""Generate synthetic L2 market data in the CsvSource format (pure Python, no dependencies).

Columns: ts_ns,type,inst,side,price,qty,seq
  type  S snapshot level | D delta level (qty 0 deletes) | T trade (side = aggressor) |
        B book ticker (two rows: bid then ask)
  side  B bid / buy aggressor | A ask / sell aggressor

The market: a latent mid random-walks on the tick grid; every `--interval-ms` the book is
re-drawn around it (levels at geometric distances from the touch, lognormal sizes) and the
net level changes against the published book are emitted as one delta batch; trades hit the touch as a Poisson process
and deplete the level they print at. Prices and quantities are integers of ticks / lots
formatted as exact decimals, so the output parses without rounding. Same seed, same file.

usage: gen_synthetic_data.py --out data.csv [--seed 42] [--duration-s 60] [--start-mid 60000]
                             [--tick 0.01] [--lot 0.00001] [--levels 20] [--inst 0]
"""
import argparse
import math
import random
import sys
from decimal import Decimal


def fmt(units: int, step: Decimal) -> str:
    """Exact decimal for units * step with trailing zeros trimmed."""
    v = (Decimal(units) * step).normalize()
    s = format(v, "f")
    return s if s != "-0" else "0"


class Book:
    def __init__(self):
        self.bids = {}  # price ticks -> qty lots
        self.asks = {}

    def side(self, s):
        return self.bids if s == "B" else self.asks

    def best(self, s):
        levels = self.side(s)  # swept levels are deleted, so every stored qty is > 0
        if not levels:
            return None
        return max(levels) if s == "B" else min(levels)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output CSV path ('-' for stdout)")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--duration-s", type=float, default=60.0)
    ap.add_argument("--start-ns", type=int, default=1_700_000_000_000_000_000)
    ap.add_argument("--start-mid", type=Decimal, default=Decimal("60000"))
    ap.add_argument("--tick", type=Decimal, default=Decimal("0.01"))
    ap.add_argument("--lot", type=Decimal, default=Decimal("0.00001"))
    ap.add_argument("--levels", type=int, default=20, help="levels per side")
    ap.add_argument("--inst", type=int, default=0)
    ap.add_argument("--interval-ms", type=int, default=100, help="depth update interval")
    ap.add_argument("--mid-step-rate", type=float, default=5.0, help="latent mid +-1 tick steps per second")
    ap.add_argument("--trade-rate", type=float, default=20.0, help="trades per second")
    ap.add_argument("--size-median-lots", type=float, default=200.0)
    ap.add_argument("--sweep-mult", type=float, default=4.0, help="trade size / level size ratio")
    args = ap.parse_args()
    if args.levels < 1 or args.duration_s <= 0 or args.tick <= 0 or args.lot <= 0:
        ap.error("levels, duration, tick and lot must be positive")

    rng = random.Random(args.seed)
    out = sys.stdout if args.out == "-" else open(args.out, "w", newline="\n")
    tick, lot, inst = args.tick, args.lot, args.inst
    mid2 = int((args.start_mid / tick).to_integral_value()) * 2  # 2 * mid in ticks (half ticks allowed)
    book = Book()
    seq = 0
    rows = 0

    def row(ts, typ, side, price_ticks, qty_lots, s):
        nonlocal rows
        out.write(f"{ts},{typ},{inst},{side},{fmt(price_ticks, tick)},{fmt(qty_lots, lot)},{s}\n")
        rows += 1

    def size():
        return max(1, int(args.size_median_lots * math.exp(0.8 * rng.gauss(0.0, 1.0))))

    def target_book():
        """Desired levels around the current latent mid."""
        bid_top = (mid2 - 1) // 2
        ask_top = bid_top + 1
        want = {"B": {}, "A": {}}
        for s, top, direction in (("B", bid_top, -1), ("A", ask_top, 1)):
            price = top
            for _ in range(args.levels):
                want[s][price] = size()
                gap = 1
                while rng.random() > 0.6 and gap < 10:
                    gap += 1
                price += direction * gap
        return want

    out.write("ts_ns,type,inst,side,price,qty,seq\n")
    ts = args.start_ns
    seq += 1
    for s, levels in target_book().items():
        book.side(s).update(levels)
    for s in ("B", "A"):
        for p in sorted(book.side(s), reverse=(s == "B")):
            row(ts, "S", s, p, book.side(s)[p], seq)
    published = {"B": dict(book.bids), "A": dict(book.asks)}  # the book readers of the CSV hold

    end = args.start_ns + int(args.duration_s * 1e9)
    interval = args.interval_ms * 1_000_000
    next_flush = ts + interval
    next_trade = ts + int(rng.expovariate(args.trade_rate) * 1e9) + 1
    next_step = ts + int(rng.expovariate(args.mid_step_rate) * 1e9) + 1
    while True:
        t = min(next_flush, next_trade, next_step)
        if t > end:
            break
        ts = t
        if t == next_step:
            mid2 += 2 if rng.random() < 0.5 else -2
            next_step = t + int(rng.expovariate(args.mid_step_rate) * 1e9) + 1
        elif t == next_trade:
            # a market order of lognormal size sweeps the opposite side, one print per level
            aggressor = "B" if rng.random() < 0.5 else "A"
            maker = "A" if aggressor == "B" else "B"
            remaining = int(size() * args.sweep_mult)
            while remaining > 0:
                best = book.best(maker)
                if best is None:
                    break
                qty = min(book.side(maker)[best], remaining)
                seq += 1
                row(ts, "T", aggressor, best, qty, seq)
                book.side(maker)[best] -= qty
                if book.side(maker)[best] <= 0:
                    del book.side(maker)[best]
                remaining -= qty
            next_trade = t + int(rng.expovariate(args.trade_rate) * 1e9) + 1
        else:
            # Re-draw part of the internal book around the latent mid (trades have depleted
            # it since the last flush), then publish every level whose quantity differs from
            # what readers of the CSV hold. Diffing against the published view is what keeps
            # their book identical to ours: trades alone never change a reader's depth.
            want = target_book()
            seq += 1
            changes = []
            for s in ("B", "A"):
                cur = book.side(s)
                for p in list(cur):
                    if p not in want[s]:
                        del cur[p]
                for p, q in want[s].items():
                    if p not in cur or rng.random() < 0.3:
                        cur[p] = q
                pub = published[s]
                for p in sorted(set(pub) | set(cur)):
                    new_qty = cur.get(p, 0)
                    if pub.get(p, 0) != new_qty:
                        changes.append((s, p, new_qty))
                published[s] = dict(cur)
            for s, p, q in changes:
                row(ts, "D", s, p, q, seq)
            bb, ba = book.best("B"), book.best("A")
            if bb is not None and ba is not None:
                row(ts, "B", "B", bb, book.bids[bb], seq)
                row(ts, "B", "A", ba, book.asks[ba], seq)
            next_flush = t + interval
    if out is not sys.stdout:
        out.close()
    print(f"wrote {rows} rows ({args.duration_s:g} s, seed {args.seed}) to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
