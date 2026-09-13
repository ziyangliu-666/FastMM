#!/usr/bin/env bash
# End-to-end demo on localhost: start the Binance-compatible sim exchange, run the live engine
# against it for N seconds, print the summary, keep journals under runs/<timestamp>/.
#   ./scripts/run-sim.sh [--duration 60s] [--preset release] [--config configs/sim-local.toml] [--tls]
set -euo pipefail
cd "$(dirname "$0")/.."
DURATION=60s; PRESET=release; CONFIG=configs/sim-local.toml; SIM_CONFIG=configs/sim.toml
while [[ $# -gt 0 ]]; do
  case "$1" in
    --duration) DURATION="$2"; shift 2;; --preset) PRESET="$2"; shift 2;; --config) CONFIG="$2"; shift 2;;
    --tls) CONFIG=configs/sim-local-tls.toml; shift;; *) echo "unknown arg $1"; exit 2;;
  esac
done
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
BIN="build/$PRESET/bin"
[[ -x "$BIN/fastmm-sim-exchange" && -x "$BIN/fastmm-live" ]] || ./scripts/build.sh "$PRESET"
RUN_DIR="runs/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$RUN_DIR"
echo "==> starting sim exchange ($SIM_CONFIG), logs in $RUN_DIR/sim.log"
"$BIN/fastmm-sim-exchange" --config "$SIM_CONFIG" > "$RUN_DIR/sim.log" 2>&1 &
SIM_PID=$!
trap 'echo "==> stopping"; kill $SIM_PID 2>/dev/null || true; wait $SIM_PID 2>/dev/null || true' EXIT
sleep 0.5
kill -0 $SIM_PID 2>/dev/null || { echo "sim exchange failed to start:"; cat "$RUN_DIR/sim.log"; exit 1; }
echo "==> running engine ($CONFIG) for $DURATION"
"$BIN/fastmm-live" --config "$CONFIG" --duration "$DURATION" --journal "$RUN_DIR/session.fmj" --log "$RUN_DIR/engine.log"
echo "==> done. artifacts: $RUN_DIR/"
