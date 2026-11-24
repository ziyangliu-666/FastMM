#!/usr/bin/env python3
"""Generates src/core/tzdata_table.cpp, the time zones the engine carries (core/time_zone.hpp).

The engine does not read the host's zoneinfo: a window evaluated live, in a replay on another
machine and in a backtest a year later must give the same answer, so the zone data is part of the
binary and its IANA version is fixed with it. Regenerate when tzdata announces a change that
matters (a new rule for a zone a session uses), and say so in the CHANGELOG:

    python3 tools/tzdata_gen.py [/usr/share/zoneinfo] > src/core/tzdata_table.cpp

Each zone keeps its offset on 2000-01-01, its transitions after that up to the point where the
POSIX rule at the end of its TZif file reproduces every later one, and that rule. Zones with equal
data (links, such as US/Eastern for America/New_York) share one entry's transitions. The result
is checked against Python's zoneinfo at every transition and between them, up to 2040.
"""
from __future__ import annotations

import datetime as dt
import struct
import sys
import zoneinfo
from pathlib import Path

START = 946684800  # 2000-01-01T00:00:00Z
CHECK_END = 2240611200  # 2041-01-01T00:00:00Z


def read_tzif(path: Path) -> tuple[list[tuple[int, int]], list[int], str]:
    """(transition time, offset after it) pairs, the offsets of the types, and the footer rule."""
    b = path.read_bytes()
    if b[:4] != b"TZif" or b[4] < ord("2"):
        raise ValueError(f"{path}: not a TZif version 2+ file")

    def counts(o: int) -> tuple[int, ...]:
        return struct.unpack(">6l", b[o + 20 : o + 44])

    isut, isstd, leap, timecnt, typecnt, charcnt = counts(0)
    o = 44 + timecnt * 5 + typecnt * 6 + charcnt + leap * 8 + isstd + isut
    isut, isstd, leap, timecnt, typecnt, charcnt = counts(o)
    o += 44
    times = struct.unpack(f">{timecnt}q", b[o : o + timecnt * 8])
    o += timecnt * 8
    idx = b[o : o + timecnt]
    o += timecnt
    types = []
    for k in range(typecnt):
        utoff, _isdst, _abbr = struct.unpack(">lBB", b[o + 6 * k : o + 6 * k + 6])
        types.append(utoff)
    o += typecnt * 6 + charcnt + leap * 12 + isstd + isut
    footer = b[o:].decode().strip("\n")
    return [(t, types[i]) for t, i in zip(times, idx)], types, footer


# ---- POSIX TZ rules (the same reading as src/core/time_zone.cpp) --------------------------------


def days_from_civil(y: int, m: int, d: int) -> int:
    return (dt.date(y, m, d) - dt.date(1970, 1, 1)).days


def is_leap(y: int) -> bool:
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)


class Posix:
    def __init__(self, s: str):
        self.s, self.i = s, 0
        self.name()
        self.std = -self.offset()
        self.dst = None
        if self.i == len(s):
            return
        self.name()
        self.dst = self.std + 3600
        if self.i < len(s) and s[self.i] != ",":
            self.dst = -self.offset()
        if self.i == len(s):  # no rule: the US rule, as glibc reads it
            self.rules = (("M", 3, 2, 0, 7200), ("M", 11, 1, 0, 7200))
            return
        self.expect(",")
        a = self.rule()
        self.expect(",")
        self.rules = (a, self.rule())
        if self.i != len(s):
            raise ValueError(f"trailing text in {s!r}")

    def expect(self, c: str) -> None:
        if self.s[self.i : self.i + 1] != c:
            raise ValueError(f"{self.s!r}: expected {c!r} at {self.i}")
        self.i += 1

    def name(self) -> None:
        s = self.s
        if s[self.i] == "<":
            self.i = s.index(">", self.i) + 1
        else:
            j = self.i
            while self.i < len(s) and s[self.i].isalpha():
                self.i += 1
            if self.i - j < 3:
                raise ValueError(f"{s!r}: short name")

    def number(self) -> int:
        j = self.i
        while self.i < len(self.s) and self.s[self.i].isdigit():
            self.i += 1
        return int(self.s[j : self.i])

    def offset(self) -> int:
        sign = 1
        if self.s[self.i] in "+-":
            sign = -1 if self.s[self.i] == "-" else 1
            self.i += 1
        v = self.number() * 3600
        for mult in (60, 1):
            if self.i < len(self.s) and self.s[self.i] == ":":
                self.i += 1
                v += self.number() * mult
        return sign * v

    def rule(self):
        s = self.s
        if s[self.i] == "J":
            self.i += 1
            r = ("J", self.number())
        elif s[self.i] == "M":
            self.i += 1
            m = self.number()
            self.expect(".")
            w = self.number()
            self.expect(".")
            r = ("M", m, w, self.number())
        else:
            r = ("N", self.number())
        t = 7200
        if self.i < len(s) and s[self.i] == "/":
            self.i += 1
            t = self.offset()
        return r + (t,)

    @staticmethod
    def day(r, y: int) -> int:
        if r[0] == "J":
            n = r[1]
            return days_from_civil(y, 1, 1) + n - 1 + (1 if is_leap(y) and n >= 60 else 0)
        if r[0] == "N":
            return days_from_civil(y, 1, 1) + r[1]
        _, m, w, wd, _t = r
        first = days_from_civil(y, m, 1)
        first_wd = (first + 4) % 7  # 1970-01-01 was a Thursday
        d = first + (wd - first_wd) % 7 + 7 * (w - 1)
        nxt = days_from_civil(y + (m == 12), m % 12 + 1, 1)
        while d >= nxt:
            d -= 7
        return d

    def offset_at(self, t: int) -> int:
        if self.dst is None:
            return self.std
        y = (dt.datetime(1970, 1, 1) + dt.timedelta(seconds=t + self.std)).year
        a, b = self.rules
        start = self.day(a, y) * 86400 + a[-1] - self.std
        end = self.day(b, y) * 86400 + b[-1] - self.dst
        on = start <= t < end if start < end else not (end <= t < start)
        return self.dst if on else self.std


# ---- one zone ------------------------------------------------------------------------------------


def explicit_offset(trans: list[tuple[int, int]], types: list[int], t: int) -> int:
    off = types[0]
    for when, o in trans:
        if when > t:
            break
        off = o
    return off


def compact(name: str, path: Path):
    trans, types, footer = read_tzif(path)
    rule = Posix(footer) if footer else None
    offset0 = explicit_offset(trans, types, START)
    after = [(t, o) for t, o in trans if t > START]
    if rule is None:
        return offset0, after, footer, 2**63 - 1
    # The earliest cut from which the rule gives every explicit offset (at and just before each
    # explicit transition after it, and at the last one's offset onwards).
    cuts = [START] + [t for t, _ in after]
    last = after[-1][0] if after else START
    rule_times = []
    if rule.dst is not None:
        for y in range(1999, dt.datetime.fromtimestamp(last, dt.timezone.utc).year + 2):
            for r, base in ((rule.rules[0], rule.std), (rule.rules[1], rule.dst)):
                rule_times.append(Posix.day(r, y) * 86400 + r[-1] - base)
    probes = sorted({p for t, _ in after for p in (t - 1, t)} | {p for t in rule_times for p in (t - 1, t)})
    best = None
    for c in reversed(cuts):
        ok = all(
            rule.offset_at(p) == explicit_offset(trans, types, p)
            for p in [c] + [p for p in probes if c < p <= last]
        )
        if not ok:
            break
        best = c
    if best is None:
        # The rule does not even give the last explicit offset: keep every transition, and use the
        # rule only after the last one.
        best = (after[-1][0] + 1) if after else START
    kept = [(t, o) for t, o in after if t < best]
    return offset0, kept, footer, best


def check(name: str, offset0, kept, footer, rule_from) -> None:
    rule = Posix(footer) if footer else None
    z = zoneinfo.ZoneInfo(name)

    def ours(t: int) -> int:
        if rule is not None and t >= rule_from:
            return rule.offset_at(t)
        off = offset0
        for when, o in kept:
            if when > t:
                break
            off = o
        return off

    def theirs(t: int) -> int:
        d = dt.datetime.fromtimestamp(t, dt.timezone.utc).astimezone(z)
        return int(d.utcoffset().total_seconds())

    probes = set(range(START, CHECK_END, 86400 * 7 + 3600 * 5))
    for t, _ in kept:
        probes.update((t - 1, t))
    if rule is not None and rule.dst is not None:
        for y in range(2000, 2041):
            for r, base in ((rule.rules[0], rule.std), (rule.rules[1], rule.dst)):
                t = Posix.day(r, y) * 86400 + r[-1] - base
                probes.update((t - 1, t))
    for t in sorted(probes):
        if START <= t < CHECK_END and ours(t) != theirs(t):
            raise SystemExit(f"{name}: {ours(t)} != zoneinfo {theirs(t)} at {t}")


def main() -> None:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "/usr/share/zoneinfo")
    zoneinfo.reset_tzpath([str(root)])  # the check reads the same files, not Python's own copy
    version = "unknown"
    zi = root / "tzdata.zi"
    if zi.exists():
        first = zi.read_text().splitlines()[0]
        if first.startswith("# version "):
            version = first.split()[-1]
    names = sorted(
        n
        for n in zoneinfo.available_timezones()
        if n not in ("Factory", "localtime", "posixrules") and (root / n).is_file()
    )
    # Links tzdata.zi lists but the directory lacks (Debian moves the old names to tzdata-legacy):
    # the same data under the old name, so US/Eastern or Asia/Calcutta still names a zone.
    links: dict[str, str] = {}
    if zi.exists():
        for line in zi.read_text().splitlines():
            if line.startswith("L "):
                _, target, link = line.split()
                if link not in names and (root / target).is_file():
                    links[link] = target
    transitions: list[tuple[int, int]] = []
    shared: dict[tuple, int] = {}
    zones = []
    for n in names:
        offset0, kept, footer, rule_from = compact(n, root / n)
        check(n, offset0, kept, footer, rule_from)
        key = tuple(kept)
        if key not in shared:
            shared[key] = len(transitions)
            transitions.extend(kept)
        zones.append((n, footer, shared[key], len(kept), offset0, rule_from))
    by_name = {z[0]: z for z in zones}
    zones += [(link, *by_name[target][1:]) for link, target in links.items() if target in by_name]
    zones.sort()

    out = sys.stdout
    out.write(
        f"// Generated by tools/tzdata_gen.py from IANA tzdata {version}; do not edit.\n"
        "// Regenerate: python3 tools/tzdata_gen.py /usr/share/zoneinfo > src/core/tzdata_table.cpp\n"
        '#include "tzdata_table.hpp"\n\n'
        "namespace fastmm::tzdata {\n\n"
        f'const char kVersion[] = "{version}";\n\n'
        "// clang-format off\n"
        "const Transition kTransitions[] = {\n"
    )
    for k in range(0, len(transitions), 4):
        row = transitions[k : k + 4]
        out.write("    " + " ".join(f"{{{t}, {o}}}," for t, o in row) + "\n")
    if not transitions:
        out.write("    {0, 0},\n")
    out.write("};\n\nconst Zone kZones[] = {\n")
    for n, footer, first, count, offset0, rule_from in zones:
        rf = "kNoRule" if rule_from == 2**63 - 1 else str(rule_from)
        out.write(f'    {{"{n}", "{footer}", {first}, {count}, {offset0}, {rf}}},\n')
    out.write(
        "};\n// clang-format on\n\n"
        "const std::size_t kZoneCount = sizeof(kZones) / sizeof(kZones[0]);\n\n"
        "}  // namespace fastmm::tzdata\n"
    )
    print(
        f"tzdata {version}: {len(zones)} zones, {len(transitions)} transitions",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
