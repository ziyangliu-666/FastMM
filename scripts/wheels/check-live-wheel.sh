#!/usr/bin/env bash
# Checks a fastmm-live wheel: OpenSSL and liburing are linked into the extension and into
# bin/fastmm-sim-exchange (no libssl, libcrypto or liburing needed or bundled), and the extension
# exports PyInit__live and nothing else.
# The repair step in .github/workflows/wheels.yml runs it.
#
#   scripts/wheels/check-live-wheel.sh <fastmm_engine_live-*.whl>
set -euo pipefail

wheel="$1"
fail() {
  echo "check-live-wheel: $*" >&2
  exit 1
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python -m zipfile -e "$wheel" "$work"
so="$(find "$work/fastmm_live" -maxdepth 1 -name '_live*.so' | head -n 1)"
[[ -n "$so" ]] || fail "no fastmm_live/_live*.so in $wheel"

needed="$(readelf -d "$so" | awk '/NEEDED/ {print $NF}' | tr -d '[]')"
echo "check-live-wheel: $(basename "$so") needs: $(echo "$needed" | tr '\n' ' ')"
if grep -qE '^lib(ssl|crypto)\.' <<<"$needed"; then
  fail "the extension links OpenSSL dynamically"
fi
if grep -qE '^liburing' <<<"$needed"; then
  fail "the extension links liburing dynamically"
fi
if python -m zipfile -l "$wheel" | grep -qE 'lib(ssl|crypto)[-.]'; then
  fail "the wheel bundles an OpenSSL shared library"
fi

sim="$work/fastmm_live/bin/fastmm-sim-exchange"
[[ -f "$sim" ]] || fail "no fastmm_live/bin/fastmm-sim-exchange in $wheel"
# zipfile -e drops the mode bits; the archive's entry says whether pip installs it executable.
python - "$wheel" <<'PY' || fail "fastmm_live/bin/fastmm-sim-exchange is not executable in $wheel"
import sys, zipfile
mode = zipfile.ZipFile(sys.argv[1]).getinfo("fastmm_live/bin/fastmm-sim-exchange").external_attr >> 16
sys.exit(0 if mode & 0o111 else 1)
PY
if readelf -d "$sim" | awk '/NEEDED/ {print $NF}' | tr -d '[]' | grep -qE '^lib(ssl|crypto|uring)'; then
  fail "fastmm-sim-exchange links OpenSSL or liburing dynamically"
fi

exported="$(nm -D --defined-only "$so" | awk '{print $NF}')"
if [[ "$exported" != "PyInit__live" ]]; then
  echo "$exported" | grep -vx 'PyInit__live' | head -n 20 >&2
  fail "the extension exports symbols other than PyInit__live ($(echo "$exported" | wc -l) in total)"
fi
grep -aqE 'OpenSSL 3\.[0-9]+\.[0-9]+' "$so" || fail "no OpenSSL 3 version string in the extension"

if command -v auditwheel >/dev/null; then
  auditwheel show "$wheel" | tee "$work/auditwheel.txt"
  if grep -qE 'lib(ssl|crypto)\.' "$work/auditwheel.txt"; then
    fail "auditwheel lists an OpenSSL library"
  fi
fi
echo "check-live-wheel: ok ($(du -k "$wheel" | cut -f1) KiB wheel, $(du -k "$so" | cut -f1) KiB extension)"
