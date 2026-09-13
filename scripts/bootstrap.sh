#!/usr/bin/env bash
# One-shot developer setup: checks toolchain, installs cmake via pip if missing,
# creates the CPM cache and configures the release (and optionally debug) preset.
set -euo pipefail
cd "$(dirname "$0")/.."

DEV=0
for a in "$@"; do [[ "$a" == "--dev" ]] && DEV=1; done

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# --- compiler ---
if command -v g++ >/dev/null; then
  GXX_VER=$(g++ -dumpfullversion -dumpversion | cut -d. -f1)
  [[ "$GXX_VER" -ge 13 ]] || say "g++ $GXX_VER found; gcc >= 13 recommended"
elif command -v clang++ >/dev/null; then
  say "using clang++ $(clang++ --version | head -1)"
else
  die "no C++ compiler found (install g++ >= 13 or clang >= 16)"
fi

# --- cmake / ninja ---
if ! command -v cmake >/dev/null || [[ "$(cmake --version | head -1 | awk '{print $3}' | cut -d. -f2)" -lt 25 ]]; then
  say "cmake >= 3.25 not found; installing via pip (user site)"
  python3 -m pip install --user "cmake>=3.25" || die "install cmake manually: sudo apt install cmake"
fi
command -v ninja >/dev/null || { say "installing ninja via pip"; python3 -m pip install --user ninja || die "sudo apt install ninja-build"; }

# --- system libs ---
[[ -f /usr/include/openssl/ssl.h || -n "${OPENSSL_ROOT_DIR:-}" ]] || die "OpenSSL headers missing: sudo apt install libssl-dev"
[[ -f /usr/include/zlib.h ]] || die "zlib headers missing: sudo apt install zlib1g-dev"

# conda can shadow the system OpenSSL for CMake's find_package; warn if it is first in PATH.
if [[ "$(command -v python3)" == *conda* && -z "${OPENSSL_ROOT_DIR:-}" ]]; then
  say "note: conda python detected. If CMake picks conda's OpenSSL, export OPENSSL_ROOT_DIR=/usr"
fi

# --- CPM cache ---
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
mkdir -p "$CPM_SOURCE_CACHE"
say "CPM_SOURCE_CACHE=$CPM_SOURCE_CACHE"

# --- configure ---
say "configuring preset: release"
cmake --preset release
if [[ $DEV -eq 1 ]]; then
  say "configuring preset: debug"
  cmake --preset debug
  command -v pre-commit >/dev/null && pre-commit install || say "pre-commit not installed (pip install pre-commit)"
fi

say "done. next:"
echo "  cmake --build --preset release -j\$(nproc) && ctest --preset release"
echo "  ./scripts/run-sim.sh          # sim exchange + engine on localhost"
