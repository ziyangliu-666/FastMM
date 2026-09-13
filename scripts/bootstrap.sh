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
CMAKE_MIN=3.25
# version_ge A B: true when version A >= version B (full dotted comparison, so 4.0 > 3.25).
version_ge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" == "$2" ]]; }
cmake_ok() { command -v cmake >/dev/null && version_ge "$(cmake --version | head -1 | awk '{print $3}')" "$CMAKE_MIN"; }
# Inside a virtualenv pip installs into it; outside, use the user site. Distro Pythons that are
# PEP 668 "externally managed" refuse both, so the error message points at apt or pipx instead.
pip_install() {
  if [[ -n "${VIRTUAL_ENV:-}" || -n "${CONDA_PREFIX:-}" ]]; then python3 -m pip install "$@"
  else python3 -m pip install --user "$@"; fi
}
if ! cmake_ok; then
  say "cmake >= $CMAKE_MIN not found; installing via pip"
  pip_install "cmake>=$CMAKE_MIN" || die "install cmake >= $CMAKE_MIN: sudo apt install cmake, or pipx install cmake"
  hash -r
  cmake_ok || die "cmake is still older than $CMAKE_MIN on PATH ($(command -v cmake))"
fi
if ! command -v ninja >/dev/null; then
  say "installing ninja via pip"
  pip_install ninja || die "install ninja: sudo apt install ninja-build, or pipx install ninja"
fi

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
