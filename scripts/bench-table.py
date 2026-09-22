#!/usr/bin/env python3
"""Latency table of one end-to-end run: fastmm-sim-itch's summary JSON and fastmm-top --json.

    scripts/bench-table.py <sim.json> <live.json> <run>

Exit code 1 when the feed was not live, a book did not sync or no order was sent.
"""
import json
import sys

sim = json.load(open(sys.argv[1]))
live = json.load(open(sys.argv[2]))
venue = live["venues"][0]


def row(name, count, p50, p99, p999):
    def f(ns):
        return f"{ns / 1000:.1f}" if ns else "-"

    print(f"| {name:<34} | {count:>9} | {f(p50):>9} | {f(p99):>9} | {f(p999):>9} |")


w = sim["wire_to_wire_ns"]
print(f"\nrun {sys.argv[3]}: {live['orders_sent']} orders, {live['replaces_sent']} replaces, "
      f"{live['fills']} fills, {venue['md_messages']} ITCH messages, feed {venue['feed']['state']}, "
      f"gaps {venue['feed']['gaps']}, tokens {sim['tokens']}, misses {sim['misses']}")
print(f"| {'hop (us)':<34} | {'count':>9} | {'p50':>9} | {'p99':>9} | {'p99.9':>9} |")
print(f"|{'-' * 36}|{'-' * 10}:|{'-' * 10}:|{'-' * 10}:|{'-' * 10}:|")
row("wire to wire (sim)", w["count"], w["p50"], w["p99"], w["p999"])
k = venue["feed"]["kernel_to_t0"]
row("kernel to T0 (net thread)", k["count"], k["p50_ns"], k["p99_ns"], k["p999_ns"])
names = {"decode": "T0 to T1 decode + L3 (net thread)", "book_apply": "T1 to T2 ring + book apply",
         "strategy": "T2 to T3 strategy", "serialize": "T3 to T4 serialize",
         "send": "T4 to T5 hand-off (single: + write)", "tick_to_trade": "T0 to T5 tick to trade (engine)"}
for key, label in names.items():
    lat = live["latency"][key]
    row(label, lat["count"], lat["p50_ns"], lat["p99_ns"], lat["p999_ns"])
t = venue["wire_tick_to_trade"]
row("T0 to OUCH write (net thread)", t["count"], t["p50_ns"], t["p99_ns"], t["p999_ns"])
if venue["feed"]["state"] != "live" or venue["books_synced"] != venue["books_total"] or live["orders_sent"] == 0:
    print("bench: the feed was not live or no order was sent", file=sys.stderr)
    sys.exit(1)
