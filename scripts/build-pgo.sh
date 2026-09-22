#!/usr/bin/env bash
# Profile-guided build of release-native, optionally followed by BOLT.
#
#   scripts/build-pgo.sh [--compiler gcc|clang] [--bolt | --bolt-only] [--no-e2e] [--build build/pgo]
#                        [-j N]
#
# 1. Instrumented build (gcc -fprofile-generate / clang -fprofile-instr-generate).
# 2. Training: the hot-path benchmarks (tick-to-order, engine step, ITCH bridge, OUCH and JSON
#    order encoders, book, OMS, quote manager), a one-day synthetic backtest and, when network
#    namespaces are available, 20 s of scripts/bench-e2e.sh (fastmm-sim-itch -> fastmm-live).
# 3. Optimised build with the profile, in the same directory (gcc finds its .gcda files by object
#    path). Output: <build>/bin, <build>/bin/bench.
# 4. --bolt: relink with --emit-relocs, instrument fastmm-live, fastmm-sim-itch and the three
#    hot-path benchmarks with llvm-bolt, rerun the training, and rewrite them with the merged
#    profile (block and function layout, hot/cold splitting). The originals are kept as *.pre-bolt.
#    llvm-bolt comes from $LLVM_BOLT, PATH (llvm-bolt, llvm-bolt-18...), or is unpacked without
#    root from the distribution's bolt package into <build>/tools (apt-get download + dpkg -x).
#    BOLT's instrumentation mode is used, so no LBR or perf is needed (WSL2 has neither).
#    --bolt-only redoes step 4 on an existing PGO build.
set -euo pipefail
cd "$(dirname "$0")/.."

COMPILER=gcc; BOLT=0; E2E=1; BUILD=build/pgo; JOBS="$(nproc)"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --compiler) COMPILER="$2"; shift 2;;
    --bolt) BOLT=1; shift;;
    --bolt-only) BOLT=2; shift;;
    --no-e2e) E2E=0; shift;;
    --build) BUILD="$2"; shift 2;;
    -j) JOBS="$2"; shift 2;;
    -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "build-pgo: unknown argument $1" >&2; exit 2;;
  esac
done
case "$COMPILER" in gcc|clang) ;; *) echo "build-pgo: --compiler gcc|clang" >&2; exit 2;; esac
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
mkdir -p "$BUILD"
BUILD="$(cd "$BUILD" && pwd)"
PROF="$BUILD/profile"
BENCHES=(bench_tick_to_order bench_codecs_nasdaq bench_order_encoders bench_book bench_oms
         bench_quote_manager bench_strategies bench_json)
APPS=(fastmm-live fastmm-sim-itch fastmm-top fastmm-backtest)
BOLT_BINS=(bin/fastmm-live bin/fastmm-sim-itch bin/bench/bench_tick_to_order
           bin/bench/bench_codecs_nasdaq bin/bench/bench_order_encoders)

cc_args=()
if [[ "$COMPILER" == clang ]]; then
  cc_args=(-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang)
  PROFDATA="$(command -v llvm-profdata || command -v llvm-profdata-18 || ls /usr/lib/llvm-*/bin/llvm-profdata 2>/dev/null | tail -1 || true)"
  [[ -x "$PROFDATA" ]] || { echo "build-pgo: llvm-profdata not found" >&2; exit 1; }
fi

configure() {  # configure <extra CXX flags> <extra linker flags>
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTMM_NATIVE_ARCH=ON \
    -DFASTMM_BUILD_TESTS=OFF -DFASTMM_BUILD_EXAMPLES=OFF -DFASTMM_USE_CCACHE=OFF \
    "${cc_args[@]}" -DCMAKE_CXX_FLAGS="$1" -DCMAKE_EXE_LINKER_FLAGS="$2" >/dev/null
}
build() { cmake --build "$BUILD" -j"$JOBS" --target "${BENCHES[@]}" "${APPS[@]}" >/dev/null; }

train() {  # train <bin dir>: every workload once
  local b="$1"
  for x in bench_tick_to_order bench_codecs_nasdaq bench_order_encoders bench_book bench_oms \
           bench_quote_manager bench_strategies bench_json; do
    "$b/bench/$x" --benchmark_min_time=0.2s >/dev/null 2>&1 || echo "build-pgo: $x failed" >&2
  done
  "$b/fastmm-backtest" --config configs/backtest-example.toml --data synthetic --duration 86400 \
    --out "$BUILD/train-backtest" >/dev/null 2>&1 || echo "build-pgo: backtest failed" >&2
  if [[ "$E2E" == 1 ]]; then
    if unshare -Urn true 2>/dev/null; then
      scripts/bench-e2e.sh --build "$(dirname "$b")" --duration 20 --out "$BUILD/train-e2e" \
        >/dev/null 2>&1 || echo "build-pgo: bench-e2e failed (continuing)" >&2
    else
      echo "build-pgo: no user/network namespaces: skipping the bench-e2e workload" >&2
    fi
  fi
}

if [[ "$BOLT" != 2 ]]; then
echo "build-pgo: [1/3] instrumented build ($COMPILER) in $BUILD"
rm -rf "$PROF"; mkdir -p "$PROF"
if [[ "$COMPILER" == gcc ]]; then
  configure "-fprofile-generate=$PROF -fprofile-update=prefer-atomic" "-fprofile-generate=$PROF"
else
  configure "-fprofile-instr-generate" "-fprofile-instr-generate"
fi
build
echo "build-pgo: [2/3] training"
export LLVM_PROFILE_FILE="$PROF/%p-%m.profraw"  # clang; gcc writes .gcda files under $PROF
train "$BUILD/bin"

echo "build-pgo: [3/3] optimised build"
LDEXTRA=""; [[ "$BOLT" != 0 ]] && LDEXTRA="-Wl,--emit-relocs"
if [[ "$COMPILER" == gcc ]]; then
  configure "-fprofile-use=$PROF -fprofile-partial-training -Wno-missing-profile" "$LDEXTRA"
else
  "$PROFDATA" merge -o "$PROF/merged.profdata" "$PROF"/*.profraw
  configure "-fprofile-instr-use=$PROF/merged.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" "$LDEXTRA"
fi
build
echo "build-pgo: PGO binaries in $BUILD/bin"
fi
[[ "$BOLT" != 0 ]] || exit 0

# ---- BOLT --------------------------------------------------------------------------------------
find_bolt() {
  if [[ -n "${LLVM_BOLT:-}" && -x "$LLVM_BOLT" ]]; then echo "$LLVM_BOLT"; return; fi
  local c
  for c in llvm-bolt llvm-bolt-20 llvm-bolt-19 llvm-bolt-18; do
    command -v "$c" >/dev/null && { command -v "$c"; return; }
  done
  local tools="$BUILD/tools"
  if [[ ! -x "$tools/usr/lib/llvm-18/bin/llvm-bolt" || ! -f "$tools/usr/lib/llvm-18/lib/libbolt_rt_instr.a" ]] &&
     command -v apt-get >/dev/null; then
    mkdir -p "$tools/debs"
    (cd "$tools/debs" && apt-get download bolt-18 libbolt-18-dev >/dev/null 2>&1) || true
    for d in "$tools"/debs/*.deb; do [[ -f "$d" ]] && dpkg -x "$d" "$tools"; done
  fi
  [[ -x "$tools/usr/lib/llvm-18/bin/llvm-bolt" ]] && echo "$tools/usr/lib/llvm-18/bin/llvm-bolt"
}
BOLT_BIN="$(find_bolt || true)"
[[ -n "$BOLT_BIN" ]] || { echo "build-pgo: llvm-bolt not found; set LLVM_BOLT" >&2; exit 1; }
BOLT_DIR="$(dirname "$BOLT_BIN")"
# llvm-bolt links the instrumentation runtime (libbolt-18-dev) from ../lib next to itself.
RT_LIB="$BOLT_DIR/../lib/libbolt_rt_instr.a"
[[ -f "$RT_LIB" ]] || { echo "build-pgo: $RT_LIB not found (libbolt-18-dev)" >&2; exit 1; }
MERGE_FDATA="$BOLT_DIR/merge-fdata"; [[ -x "$MERGE_FDATA" ]] || MERGE_FDATA="$(command -v merge-fdata || command -v merge-fdata-18)"
echo "build-pgo: [bolt] instrumenting with $BOLT_BIN"
FDATA="$BUILD/bolt"; rm -rf "$FDATA"; mkdir -p "$FDATA"
for b in "${BOLT_BINS[@]}"; do
  n="$(basename "$b")"
  [[ -f "$BUILD/$b.pre-bolt" ]] || cp "$BUILD/$b" "$BUILD/$b.pre-bolt"
  "$BOLT_BIN" "$BUILD/$b.pre-bolt" -instrument -o "$BUILD/$b" \
    --instrumentation-file="$FDATA/$n.fdata" --instrumentation-file-append-pid >/dev/null
done
train "$BUILD/bin"
echo "build-pgo: [bolt] optimising"
for b in "${BOLT_BINS[@]}"; do
  n="$(basename "$b")"
  shopt -s nullglob
  files=("$FDATA/$n".fdata*)
  shopt -u nullglob
  if [[ ${#files[@]} -eq 0 ]]; then
    echo "build-pgo: no profile for $n; keeping the PGO binary" >&2
    cp "$BUILD/$b.pre-bolt" "$BUILD/$b"
    continue
  fi
  "$MERGE_FDATA" "${files[@]}" > "$FDATA/$n.merged.fdata" 2>/dev/null
  "$BOLT_BIN" "$BUILD/$b.pre-bolt" -o "$BUILD/$b" -data="$FDATA/$n.merged.fdata" \
    -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions -split-all-cold \
    -split-eh -icf=1 -use-gnu-stack -dyno-stats >"$FDATA/$n.log" 2>&1 ||
    { echo "build-pgo: llvm-bolt failed on $n (see $FDATA/$n.log); keeping the PGO binary" >&2
      cp "$BUILD/$b.pre-bolt" "$BUILD/$b"; }
done
echo "build-pgo: PGO + BOLT binaries in $BUILD/bin"
