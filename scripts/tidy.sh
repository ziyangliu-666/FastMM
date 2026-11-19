#!/usr/bin/env bash
# clang-tidy over src/ and apps/ using compile_commands from build/debug (configure it first).
#   scripts/tidy.sh [build-dir] [--errors-only]
# --errors-only runs just the checks .clang-tidy makes errors (WarningsAsErrors, less the ones it
# disables), which is what decides CI: the same findings, in less than half the time.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${1:-build/debug}"
[[ -f "$BUILD/compile_commands.json" ]] || { echo "configure first: cmake --preset debug"; exit 1; }
RCT="${RUN_CLANG_TIDY:-$(command -v run-clang-tidy-18 || command -v run-clang-tidy)}"
checks=()
if [[ "${2:-}" == --errors-only ]]; then
  errors=$(sed -nE "s/^WarningsAsErrors: '(.*)'$/\1/p" .clang-tidy)
  groups=$(sed -E 's/-\*//g; s/,/|/g' <<<"$errors")
  disabled=$(grep -oE "^\s*-($groups)-[a-z0-9.-]+" .clang-tidy | tr -d ' ' | paste -sd, -)
  checks=("-checks=-*,$errors${disabled:+,$disabled}")
fi
# gcc-only warning flags (-Wlogical-op, -Wuseless-cast, ...) in a gcc compile database are
# unknown to clang; with -Werror they would become errors in every file.
# The file regex is anchored at the repository root: dependency sources in the CPM cache
# (~/.cache/CPM/<pkg>/<hash>/src/...) would otherwise match "src/" too.
"$RCT" -p "$BUILD" -j"$(nproc)" -quiet -extra-arg=-Wno-unknown-warning-option "${checks[@]}" \
  "^$(pwd)/(src|apps)/"
