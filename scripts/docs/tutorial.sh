#!/usr/bin/env bash
# The shell steps of the tutorial "Your first market maker" (docs/tutorials/first-strategy/), in
# order. The pages embed the regions between the `--8<--` markers (tools/doc_snippets.py), so this
# script is what readers copy, and CI runs it (ctest tutorial.script, label tutorial).
#
#   scripts/docs/tutorial.sh [--through <step>] [--skip-build]
#
# Steps: setup, test, backtest, register, cli, sim, demo. --through stops after a step (default sim);
# `demo` trades on Binance Demo and needs FASTMM_BINANCE_API_KEY and FASTMM_BINANCE_API_SECRET.
# --skip-build uses an existing build (FASTMM_BUILD_DIR, default build/release). Output goes to
# runs/tutorial/. The simulated exchange listens on ports 9080 and 9443; FASTMM_SIM_PORT=<port>
# (0 picks a free one) runs the sim step in runs/tutorial-port-<port>/ instead, with a copy of
# configs/tutorial-sim.toml on that port and an ephemeral TLS port (ctest tutorial.script).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD

THROUGH=sim
SKIP_BUILD=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --through) THROUGH="$2"; shift 2;;
    --skip-build) SKIP_BUILD=1; shift;;
    -h|--help) sed -n '2,14p' "$0"; exit 0;;
    *) echo "tutorial.sh: unknown argument $1" >&2; exit 2;;
  esac
done
STEPS=(setup test backtest register cli sim demo)
LAST=-1
for i in "${!STEPS[@]}"; do [[ "${STEPS[$i]}" == "$THROUGH" ]] && LAST=$i; done
[[ $LAST -ge 0 ]] || { echo "tutorial.sh: unknown step '$THROUGH' (${STEPS[*]})" >&2; exit 2; }
wanted() {
  local i
  for i in "${!STEPS[@]}"; do [[ "${STEPS[$i]}" == "$1" ]] && { [[ $i -le $LAST ]]; return; }; done
  return 1
}
step() { echo; echo "==> tutorial: $*"; }
fail() { echo "tutorial.sh: FAILED: $*" >&2; exit 1; }

# --8<-- [start:bin]
BUILD=build/release
BIN=$BUILD/bin
# --8<-- [end:bin]
BUILD=${FASTMM_BUILD_DIR:-$BUILD}
BIN=$BUILD/bin
mkdir -p runs/tutorial

if wanted setup; then
  step "set up"
  if [[ $SKIP_BUILD -eq 0 ]]; then
    # --8<-- [start:build]
    ./scripts/bootstrap.sh
    cmake --build --preset release -j
    # --8<-- [end:build]
  fi
  # --8<-- [start:list]
  "$BIN"/fastmm-backtest --list-strategies
  # --8<-- [end:list]
fi

if wanted test; then
  step "unit-test the strategy"
  # --8<-- [start:unit-test]
  "$BIN"/first_mm_test
  # --8<-- [end:unit-test]
fi

if wanted backtest; then
  step "backtest in C++"
  # --8<-- [start:cpp-backtest]
  "$BIN"/first_mm_backtest
  # --8<-- [end:cpp-backtest]
fi

if wanted register; then
  step "register"
  # --8<-- [start:list-registered]
  "$BIN"/tutorial-backtest --list-strategies --format json | grep -o '"name": "first_mm", "transports": [^]]*]'
  # --8<-- [end:list-registered]
fi

if wanted cli; then
  step "backtest and replay from the command line"
  rm -rf runs/tutorial/backtest runs/tutorial/backtest.fmj
  # --8<-- [start:cli-backtest]
  "$BIN"/tutorial-backtest --config configs/backtest-example.toml --data synthetic \
    --strategy first_mm --param edge_bps=0.002 --seed 7 --duration 60 \
    --out runs/tutorial/backtest --journal-out runs/tutorial/backtest.fmj
  # --8<-- [end:cli-backtest]
  [[ -s runs/tutorial/backtest/summary.json ]] || fail "no runs/tutorial/backtest/summary.json"
  # --8<-- [start:cli-report]
  python3 tools/report.py runs/tutorial/backtest --config configs/backtest-example.toml
  # --8<-- [end:cli-report]
  [[ -s runs/tutorial/backtest/report.html ]] || fail "no runs/tutorial/backtest/report.html"
  # --8<-- [start:cli-replay]
  "$BIN"/tutorial-replay --journal runs/tutorial/backtest.fmj --verify
  # --8<-- [end:cli-replay]
fi

port_open() { (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }

if wanted sim; then
  step "trade on the simulated exchange"
  SIM_PORT=${FASTMM_SIM_PORT:-9080}
  if [[ "$SIM_PORT" != 9080 ]]; then
    if [[ "$SIM_PORT" == 0 ]]; then
      SIM_PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    fi
    # The commands below are the tutorial's, so they read configs/tutorial-sim.toml and write
    # runs/tutorial/: run them in a directory whose copy of that file uses SIM_PORT.
    WORK=runs/tutorial-port-$SIM_PORT
    rm -rf "$WORK"
    mkdir -p "$WORK/configs" "$WORK/runs/tutorial" "$WORK/tests/fixtures"
    ln -s "$ROOT/tests/fixtures/tls" "$WORK/tests/fixtures/tls"
    sed -e "s/127\.0\.0\.1:9080/127.0.0.1:$SIM_PORT/g" -e "s/^port = 9080\b/port = $SIM_PORT/" \
      -e "s/^tls_port = 9443\b/tls_port = 0/" -e "s/^name = \"tutorial-sim\"/name = \"tutorial-sim-$SIM_PORT\"/" \
      configs/tutorial-sim.toml > "$WORK/configs/tutorial-sim.toml"
    grep -q "^port = $SIM_PORT\b" "$WORK/configs/tutorial-sim.toml" || fail "configs/tutorial-sim.toml has no port = 9080"
    BIN=$(realpath "$BIN")
    cd "$WORK"
  fi
  port_open "$SIM_PORT" && fail "port $SIM_PORT is in use (another fastmm-sim-exchange? set FASTMM_SIM_PORT)"
  rm -f runs/tutorial/sim.fmj runs/tutorial/sim-live.log
  # --8<-- [start:sim-exchange]
  "$BIN"/fastmm-sim-exchange --config configs/tutorial-sim.toml --duration 90s > runs/tutorial/sim-exchange.log 2>&1 &
  # --8<-- [end:sim-exchange]
  SIM_PID=$!
  trap 'kill "$SIM_PID" 2>/dev/null || true' EXIT
  for _ in $(seq 1 100); do
    port_open "$SIM_PORT" && break
    kill -0 "$SIM_PID" 2>/dev/null || { cat runs/tutorial/sim-exchange.log >&2; fail "fastmm-sim-exchange exited"; }
    sleep 0.1
  done
  # --8<-- [start:sim-live]
  export FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret
  "$BIN"/tutorial-live --config configs/tutorial-sim.toml --duration 40s \
    --journal runs/tutorial/sim.fmj --log runs/tutorial/sim-live.log
  # --8<-- [end:sim-live]
  # --8<-- [start:sim-log]
  grep -E "first_mm: |fastmm-live: (events|realized_pnl|shutdown took)" runs/tutorial/sim-live.log
  # --8<-- [end:sim-log]
  grep -q "first_mm: venue 0 channel 0 is Disconnected; quotes pulled" runs/tutorial/sim-live.log ||
    fail "the market-data disconnect did not reach on_connection"
  grep -q "fastmm-live: shutdown took .* (cancel_all ok)" runs/tutorial/sim-live.log ||
    fail "the session did not end with cancel_all ok"
  orders=$(sed -n 's/.*fastmm-live: events=[0-9]* book_updates=[0-9]* orders=\([0-9]*\).*/\1/p' runs/tutorial/sim-live.log | tail -1)
  [[ "${orders:-0}" -gt 0 ]] || fail "tutorial-live sent no orders"
  # --8<-- [start:sim-replay]
  "$BIN"/tutorial-replay --journal runs/tutorial/sim.fmj --verify
  # --8<-- [end:sim-replay]
  kill "$SIM_PID" 2>/dev/null || true
  wait "$SIM_PID" 2>/dev/null || true
  trap - EXIT
  cd "$ROOT"
fi

if wanted demo; then
  step "Binance Demo"
  # --8<-- [start:demo-dry-run]
  "$BIN"/tutorial-live --config configs/tutorial-binance-demo.toml --dry-run --duration 60s
  # --8<-- [end:demo-dry-run]
  [[ -n "${FASTMM_BINANCE_API_KEY:-}" && -n "${FASTMM_BINANCE_API_SECRET:-}" ]] ||
    fail "export FASTMM_BINANCE_API_KEY and FASTMM_BINANCE_API_SECRET (Binance Demo Trading keys)"
  # --8<-- [start:demo-run]
  "$BIN"/tutorial-live --config configs/tutorial-binance-demo.toml --duration 5m \
    --journal runs/tutorial/demo.fmj --log runs/tutorial/demo.log
  # --8<-- [end:demo-run]
  grep "fastmm-live: shutdown took" runs/tutorial/demo.log
  # --8<-- [start:demo-report]
  python3 tools/pnl_report.py runs/tutorial/demo.fmj --engine-log runs/tutorial/demo.log
  # --8<-- [end:demo-report]
fi

echo
echo "tutorial.sh: OK (through $THROUGH)"
