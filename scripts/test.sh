#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
PRESET="${1:-release}"; shift || true
./scripts/build.sh "$PRESET"
ctest --preset "$PRESET" -j"$(nproc)" "$@"
