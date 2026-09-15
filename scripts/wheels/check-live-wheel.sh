#!/usr/bin/env bash
# Checks a fastmm-live wheel: OpenSSL is linked into the extension (no libssl or libcrypto needed or
# bundled) and the extension exports PyInit__live and nothing else.
# The repair step in .github/workflows/wheels.yml runs it.
#
#   scripts/wheels/check-live-wheel.sh <fastmm_live-*.whl>
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
if python -m zipfile -l "$wheel" | grep -qE 'lib(ssl|crypto)[-.]'; then
  fail "the wheel bundles an OpenSSL shared library"
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
