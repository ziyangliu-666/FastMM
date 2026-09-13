#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
PRESET="${1:-release}"; shift || true
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
[[ -d "build/$PRESET" ]] || cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j"$(nproc)" "$@"
