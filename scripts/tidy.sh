#!/usr/bin/env bash
# clang-tidy over src/ and apps/ using compile_commands from build/debug (configure it first).
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${1:-build/debug}"
[[ -f "$BUILD/compile_commands.json" ]] || { echo "configure first: cmake --preset debug"; exit 1; }
RCT="${RUN_CLANG_TIDY:-$(command -v run-clang-tidy-18 || command -v run-clang-tidy)}"
# gcc-only warning flags (-Wlogical-op, -Wuseless-cast, ...) in a gcc compile database are
# unknown to clang; with -Werror they would become errors in every file.
"$RCT" -p "$BUILD" -j"$(nproc)" -quiet -extra-arg=-Wno-unknown-warning-option "src/|apps/"
