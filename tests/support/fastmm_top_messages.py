#!/usr/bin/env python3
"""fastmm-top --once without a readable status segment: the message and exit code 3.

usage: fastmm_top_messages.py <fastmm-top> <include/fastmm/core/status_segment.hpp> <tmp dir>

Cases: no file, a file without a published segment, and segments of another version (the smaller
version 1 layout that open() refuses, and a newer version at least as large as this layout).
"""
import os
import re
import struct
import subprocess
import sys


def main():
    top, header, tmp = sys.argv[1:4]
    text = open(header, encoding="utf-8").read()
    version = int(re.search(r"kStatusVersion = (\d+);", text).group(1))
    magic = int(re.search(r"kStatusMagic = (0x[0-9A-Fa-f]+)ULL;", text).group(1), 16)
    os.makedirs(tmp, exist_ok=True)
    failures = []

    def run(path):
        p = subprocess.run([top, "--once", "--no-color", "--path", path], capture_output=True, text=True)
        return p.returncode, p.stderr.strip()

    def expect(name, path, want):
        rc, err = run(path)
        ok = rc == 3 and (want.fullmatch(err) if isinstance(want, re.Pattern) else err == want)
        print(f"{'ok  ' if ok else 'FAIL'} {name}: exit {rc}: {err}")
        if not ok:
            failures.append(name)

    def write(name, payload):
        path = os.path.join(tmp, name)
        with open(path, "wb") as f:
            f.write(payload)
        return path

    missing = os.path.join(tmp, "fastmm-top-missing.status")
    if os.path.exists(missing):
        os.remove(missing)
    expect("missing file", missing,
           re.compile(re.escape(f"fastmm-top: no status segment at {missing} (") + r".+" +
                      re.escape("); is fastmm-live running?")))

    empty = write("fastmm-top-empty.status", bytes(1 << 20))
    expect("unpublished segment", empty, f"fastmm-top: no status segment at {empty} yet")

    def other_build(path, v):
        return (f"fastmm-top: {path} was written by a different FastMM build (status segment version {v}, "
                f"this fastmm-top reads version {version}); use fastmm-top from the same build as fastmm-live")

    old = write("fastmm-top-v1.status", struct.pack("<QQI", 2, magic, 1) + bytes(512))
    expect("older version (smaller layout)", old, other_build(old, 1))
    new = write("fastmm-top-newer.status", struct.pack("<QQI", 2, magic, version + 1) + bytes(1 << 20))
    expect("newer version", new, other_build(new, version + 1))

    for p in (empty, old, new):
        os.remove(p)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
