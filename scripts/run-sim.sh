#!/usr/bin/env bash
# End-to-end demo on localhost: start the Binance-compatible sim exchange, wait until it accepts
# connections, run the live engine against it, print a summary, stop the simulator.
# Artifacts: runs/<timestamp>/{sim.log,engine.log,session.fmj}.
#
#   ./scripts/run-sim.sh [--duration 30s] [--preset release | --build-dir build/<dir>]
#                        [--config configs/sim-local.toml] [--tls] [--sim-config configs/sim.toml]
set -euo pipefail
cd "$(dirname "$0")/.."

DURATION=30s
PRESET=release
BUILD_DIR=""
CONFIG=configs/sim-local.toml
SIM_CONFIG=configs/sim.toml
PORT=9080
while [[ $# -gt 0 ]]; do
  case "$1" in
    --duration) DURATION="$2"; shift 2;;
    --preset) PRESET="$2"; shift 2;;
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --config) CONFIG="$2"; shift 2;;
    --sim-config) SIM_CONFIG="$2"; shift 2;;
    --tls) CONFIG=configs/sim-local-tls.toml; shift;;
    -h|--help) sed -n '2,8p' "$0"; exit 0;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done

export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
BIN="${BUILD_DIR:-build/$PRESET}/bin"
if [[ ! -x "$BIN/fastmm-sim-exchange" || ! -x "$BIN/fastmm-live" ]]; then
  if [[ -n "$BUILD_DIR" ]]; then
    echo "fastmm-sim-exchange / fastmm-live not found in $BIN" >&2
    exit 1
  fi
  ./scripts/build.sh "$PRESET" --target fastmm-sim-exchange fastmm-live
fi

# The engine configs read ${FASTMM_SIM_API_KEY}/${FASTMM_SIM_API_SECRET}; the simulator honours
# the same variables, so both sides agree whatever is exported.
export FASTMM_SIM_API_KEY="${FASTMM_SIM_API_KEY:-sim-key}"
export FASTMM_SIM_API_SECRET="${FASTMM_SIM_API_SECRET:-sim-secret}"

port_open() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
if port_open "$PORT"; then
  echo "port $PORT is already in use (another fastmm-sim-exchange?)" >&2
  exit 1
fi

RUN_DIR="runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN_DIR"
echo "==> starting fastmm-sim-exchange ($SIM_CONFIG), log: $RUN_DIR/sim.log"
"$BIN/fastmm-sim-exchange" --config "$SIM_CONFIG" --stats-interval 5s > "$RUN_DIR/sim.log" 2>&1 &
SIM_PID=$!
stop_sim() {
  if kill -0 "$SIM_PID" 2>/dev/null; then
    kill -INT "$SIM_PID" 2>/dev/null || true
    wait "$SIM_PID" 2>/dev/null || true
  fi
}
trap stop_sim EXIT

for _ in $(seq 1 100); do
  port_open "$PORT" && break
  if ! kill -0 "$SIM_PID" 2>/dev/null; then
    echo "fastmm-sim-exchange exited during startup:" >&2
    cat "$RUN_DIR/sim.log" >&2
    exit 1
  fi
  sleep 0.1
done
port_open "$PORT" || { echo "fastmm-sim-exchange did not open port $PORT" >&2; exit 1; }
grep -E "^  (REST|TLS)" "$RUN_DIR/sim.log" || true

echo "==> running fastmm-live ($CONFIG) for $DURATION, log: $RUN_DIR/engine.log"
set +e
"$BIN/fastmm-live" --config "$CONFIG" --duration "$DURATION" \
  --journal "$RUN_DIR/session.fmj" --log "$RUN_DIR/engine.log"
RC=$?
set -e
stop_sim
trap - EXIT

echo "==> summary (engine exit code $RC)"
grep -hoE "fastmm-live: events=.*" "$RUN_DIR/engine.log" | tail -1 | sed 's/^/  engine: /' || true
grep -hoE "\[[a-z0-9_-]+\] final: .*" "$RUN_DIR/engine.log" | tail -1 | sed 's/^/  venue:  /' || true
grep -hoE "fastmm-live: shutdown took .*" "$RUN_DIR/engine.log" | tail -1 | sed 's/^/  /' || true
WARNINGS=$(grep -cE " (WARN|ERROR) " "$RUN_DIR/engine.log" || true)
echo "  engine warnings/errors: $WARNINGS (see $RUN_DIR/engine.log)"
grep -hE "^\[sim\] final" "$RUN_DIR/sim.log" | sed 's/^/  /' || true
echo "==> artifacts: $RUN_DIR/"
exit "$RC"
