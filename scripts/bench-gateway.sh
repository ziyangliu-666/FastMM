#!/usr/bin/env bash
# Tick-to-trade through fastmm-gateway against in-process fastmm-live, both trading BasicMM on
# fastmm-sim-exchange (configs/sim-local.toml) on this host.
#
#   scripts/bench-gateway.sh [--runs 3] [--duration 60] [--spin busy|adaptive]
#                            [--build build/release] [--out runs/bench-gateway-<time>]
#                            [--account-limits]
#
# Each run starts a fresh simulator, then trades once in-process and once attached to a gateway.
# Per run and mode it prints the engine's tick-to-trade (market-data receive on the network thread
# to the order handed to the transport; through the gateway this crosses the md ring) and the
# network thread's wire tick-to-trade (receive to the order's send returning; through the gateway
# both rings and the extra copy). --account-limits sets [gateway] max_loss, max_gross_notional and
# max_net_notional far out of reach, so every order takes the account checks (the gateway then
# runs under a name of its own). Logs stay in the output directory.
set -euo pipefail
cd "$(dirname "$0")/.."

RUNS=3; DURATION=60; SPIN=busy; BUILD=build/release; OUT=""; LIMITS=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --runs) RUNS="$2"; shift 2;;
    --duration) DURATION="$2"; shift 2;;
    --spin) SPIN="$2"; shift 2;;
    --build) BUILD="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --account-limits) LIMITS=1; shift;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "bench-gateway: unknown argument $1" >&2; exit 2;;
  esac
done
case "$SPIN" in busy|adaptive) ;; *) echo "bench-gateway: --spin busy|adaptive" >&2; exit 2;; esac
for b in fastmm-sim-exchange fastmm-live fastmm-gateway; do
  [[ -x "$BUILD/bin/$b" ]] || { echo "bench-gateway: $BUILD/bin/$b not found" >&2; exit 2; }
done
BUILD="$(cd "$BUILD" && pwd)"
[[ -n "$OUT" ]] || OUT="runs/bench-gateway-$(date -u +%Y%m%d-%H%M%S)-$SPIN"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
export FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret

SIM=""; GW=""
cleanup() {
  [[ -z "$GW" ]] || kill -TERM "$GW" 2>/dev/null || true
  [[ -z "$SIM" ]] || kill -TERM "$SIM" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

# p50 and p99 of the engine's tick_to_trade and of the network thread's final wire_t2t.
engine_t2t() { grep -o 'tick_to_trade p50=[0-9]* ns p99=[0-9]* ns' "$1" | tail -1 | sed 's/tick_to_trade //; s/ ns//g'; }
wire_t2t() { grep -o 'final order latency: wire_t2t p50=[0-9]*ns p99=[0-9]*ns n=[0-9]*' "$1" | tail -1 | sed 's/final order latency: wire_t2t //; s/ns//g'; }

printf '%-4s %-9s %-28s %s\n' run mode "engine t2t (ns)" "wire t2t (ns)"
for ((r = 1; r <= RUNS; r++)); do
  for mode in inproc gateway; do
    d="$OUT/run$r-$mode"
    mkdir -p "$d"
    sed -e "s|journal_dir = \"runs\"|journal_dir = \"$d\"|" \
        -e "s|epoch_file = \"runs/session_epoch\"|epoch_file = \"$d/session_epoch\"|" \
        -e "s|spin_mode = \"adaptive\"|spin_mode = \"$SPIN\"|" \
        configs/sim-local.toml > "$d/config.toml"
    "$BUILD/bin/fastmm-sim-exchange" --config configs/sim.toml > "$d/sim.log" 2>&1 &
    SIM=$!
    sleep 1
    if [[ "$mode" == gateway ]]; then
      gw_config="$d/config.toml"; gw_name=sim-local
      if [[ "$LIMITS" == 1 ]]; then
        gw_config="$d/gateway.toml"; gw_name=sim-local-gw
        sed -e 's|name = "sim-local"|name = "sim-local-gw"|' "$d/config.toml" > "$gw_config"
        printf '\n[gateway]\nmax_loss = "1000000"\nmax_gross_notional = "1000000000"\nmax_net_notional = "1000000000"\n' >> "$gw_config"
      fi
      "$BUILD/bin/fastmm-gateway" --config "$gw_config" --log "$d/gateway.log" > /dev/null 2>&1 &
      GW=$!
      for _ in $(seq 100); do [[ -S "$d/$gw_name.gw" ]] && break; sleep 0.1; done
      sleep 1
      "$BUILD/bin/fastmm-live" --config "$d/config.toml" --gateway "$d/$gw_name.gw" \
        --duration "${DURATION}s" --no-status --no-journal --log "$d/live.log" > /dev/null 2>&1
      kill -TERM "$GW"; wait "$GW" || true; GW=""
      wire="$(wire_t2t "$d/gateway.log")"
    else
      "$BUILD/bin/fastmm-live" --config "$d/config.toml" --duration "${DURATION}s" --no-status \
        --no-journal --log "$d/live.log" > /dev/null 2>&1
      wire="$(wire_t2t "$d/live.log")"
    fi
    kill -TERM "$SIM"; wait "$SIM" || true; SIM=""
    printf '%-4s %-9s %-28s %s\n' "$r" "$mode" "$(engine_t2t "$d/live.log")" "$wire"
  done
done
echo "logs: $OUT"
