#!/usr/bin/env bash
# End-to-end check of examples/external-project against an installed FastMM (CI gcc-release job):
#
#   cmake --install build/release --prefix build/install
#   ./scripts/ci-external-project.sh build/release build/install build/external-project
#
# 1. configures and builds the project against the install prefix (CXX selects the compiler, JOBS
#    the build parallelism) and runs its unit test
# 2. mm-live and mm-backtest --list-strategies show microprice_mm and the built-in basic_mm
# 3. a synthetic backtest of microprice_mm has fills
# 4. mm-live trades microprice_mm for 15 s against the build tree's fastmm-sim-exchange, journaling
# 5. mm-replay --verify reproduces that journal's outbound stream
# 6. an unknown --strategy exits with code 3
# 7. examples/external-venue, a venue connector in its own project: builds against the same prefix,
#    its unit test passes, and echo-live loads a configuration whose `kind` names that connector
# Needs ports 9080 and 9443 free.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${1:?usage: ci-external-project.sh <fastmm build dir> <install prefix> <project build dir>}
PREFIX=$(realpath "${2:?install prefix}")
EXT=${3:?project build dir}
LIVE_SECONDS=${LIVE_SECONDS:-15}

fail() {
  echo "ci-external-project: FAILED: $*" >&2
  exit 1
}
step() { echo "==> $*"; }

step "configure and build examples/external-project against $PREFIX"
cmake -S examples/external-project -B "$EXT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PREFIX" ${CXX:+-DCMAKE_CXX_COMPILER=$CXX}
cmake --build "$EXT" ${JOBS:+-j "$JOBS"}
ctest --test-dir "$EXT" --output-on-failure

step "strategy lists"
for app in mm-live mm-backtest; do
  list=$("$EXT/$app" --list-strategies)
  for name in microprice_mm basic_mm; do
    grep -qx "$name" <<<"$list" || fail "$app --list-strategies does not list $name"
  done
  "$EXT/$app" --list-strategies --format json |
    grep -q '"name": "microprice_mm", "transports": \["sim", "replay", "live"\]' ||
    fail "$app --list-strategies --format json: microprice_mm is not registered for every transport"
done
echo "microprice_mm and basic_mm listed by mm-live and mm-backtest"

step "synthetic backtest"
rm -rf "$EXT/bt"
"$EXT/mm-backtest" --config configs/backtest-example.toml --data synthetic \
  --strategy microprice_mm --param edge_ticks=1 --duration 30 --out "$EXT/bt"
fills=$(sed -n 's/.*"fills": \([0-9]*\).*/\1/p' "$EXT/bt/summary.json" | head -1)
[[ "${fills:-0}" -gt 0 ]] || fail "the synthetic backtest has no fills"
echo "backtest fills: $fills"

step "live session against fastmm-sim-exchange (${LIVE_SECONDS} s)"
SIM="$BUILD/bin/fastmm-sim-exchange"
[[ -x "$SIM" ]] || fail "$SIM not found (build the fastmm-sim-exchange target)"
"$SIM" --config configs/sim.toml --duration "$((LIVE_SECONDS + 30))s" >"$EXT/sim.log" 2>&1 &
sim_pid=$!
trap 'kill "$sim_pid" 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do
  (echo >/dev/tcp/127.0.0.1/9080) 2>/dev/null && break
  kill -0 "$sim_pid" 2>/dev/null || { cat "$EXT/sim.log" >&2; fail "fastmm-sim-exchange exited"; }
  sleep 0.1
done
rm -f "$EXT/live.fmj"
rc=0
FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret \
  "$EXT/mm-live" --config configs/sim-local.toml --strategy microprice_mm \
  --duration "${LIVE_SECONDS}s" --no-status --journal "$EXT/live.fmj" >"$EXT/live.log" 2>&1 || rc=$?
grep -E "fastmm-live: (session|events=|realized_pnl=|shutdown took)|note:" "$EXT/live.log" || true
[[ $rc -eq 0 ]] || { tail -40 "$EXT/live.log" >&2; fail "mm-live exited with $rc"; }
orders=$(sed -n 's/.*fastmm-live: events=[0-9]* book_updates=[0-9]* orders=\([0-9]*\).*/\1/p' "$EXT/live.log" | tail -1)
[[ "${orders:-0}" -gt 0 ]] || fail "mm-live sent no orders"
grep -q "fastmm-live: shutdown took .* (cancel_all ok)" "$EXT/live.log" || fail "cancel_all did not succeed"

step "replay the live journal"
"$EXT/mm-replay" --journal "$EXT/live.fmj" --verify | tee "$EXT/replay.log"
grep -q "^replay MATCH" "$EXT/replay.log" || fail "the live journal does not replay"

step "unknown strategy"
rc=0
"$EXT/mm-backtest" --config configs/backtest-example.toml --strategy nope >/dev/null 2>&1 || rc=$?
[[ $rc -eq 3 ]] || fail "mm-backtest --strategy nope exited with $rc, expected 3"
rc=0
"$EXT/mm-live" --config configs/sim-local.toml --dry-run --no-journal --no-status \
  --strategy nope >"$EXT/unknown.log" 2>&1 || rc=$?
[[ $rc -eq 3 ]] || fail "mm-live --strategy nope exited with $rc, expected 3"
grep -q "mm-live: unknown strategy 'nope' (available: .*microprice_mm" "$EXT/unknown.log" ||
  fail "mm-live --strategy nope does not list the available strategies"
echo "unknown strategy: exit 3"

step "configure and build examples/external-venue against $PREFIX"
VENUE_EXT="$EXT-venue"
cmake -S examples/external-venue -B "$VENUE_EXT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PREFIX" ${CXX:+-DCMAKE_CXX_COMPILER=$CXX}
cmake --build "$VENUE_EXT" ${JOBS:+-j "$JOBS"}
ctest --test-dir "$VENUE_EXT" --output-on-failure

step "echo-live runs the out-of-tree connector"
rc=0
"$VENUE_EXT/echo-live" --config examples/external-venue/configs/echo.toml --dry-run --no-journal \
  --no-status --duration 2s >"$VENUE_EXT/live.log" 2>&1 || rc=$?
[[ $rc -eq 0 ]] || { tail -40 "$VENUE_EXT/live.log" >&2; fail "echo-live exited with $rc"; }
! grep -q "unsupported kind" "$VENUE_EXT/live.log" || fail "echo-live did not resolve kind = \"echo\""
echo "echo-live: a venue registered outside FastMM ran a session"

echo "ci-external-project: OK"
