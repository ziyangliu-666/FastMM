#!/usr/bin/env bash
# End-to-end latency: fastmm-sim-itch and fastmm-live in two network namespaces joined by a veth
# pair (ADR-0015, section 6). Market data (ITCH over MoldUDP64 multicast, lines A and B), GLIMPSE,
# re-requests and OUCH orders cross the veth. BasicMM trades on the nasdaq_itch venue with
# order_entry = "sim_ouch"; every order names the ITCH sequence number that triggered it, and the
# simulator times it from the sendmmsg of that datagram to the read that returned the order.
#
#   scripts/bench-e2e.sh [--backend kernel|af_xdp] [--duration 30] [--runs 3] [--speed 4]
#                        [--spin busy|adaptive] [--sim-cpu 2] [--engine-cpu 4] [--net-cpu 6]
#                        [--build build/release] [--out runs/bench-e2e-<time>]
#
# A CPU of -1 leaves that process unpinned (ctest runs it that way). Exit codes: 0 ok, 1 a run
# failed (fastmm-live exit code, feed not live, no orders), 2 usage, 77 no namespaces here.
# kernel runs unprivileged in `unshare -Urn`. af_xdp loads an XDP program and needs root:
#   sudo scripts/bench-e2e.sh --backend af_xdp
# Prints p50 / p99 / p99.9 per run: wire to wire (simulator), kernel to T0 (fastmm-live network
# thread), the engine's hops and tick-to-trade, and the network thread's tick-to-trade. The JSON
# inputs stay in the output directory.
set -euo pipefail
cd "$(dirname "$0")/.."

BACKEND=kernel; DURATION=30; RUNS=1; SPEED=4; SPIN=busy
SIM_CPU=2; ENGINE_CPU=4; NET_CPU=6; BUILD=build/release; OUT=""
usage() { sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend) BACKEND="$2"; shift 2;;
    --duration) DURATION="$2"; shift 2;;
    --runs) RUNS="$2"; shift 2;;
    --speed) SPEED="$2"; shift 2;;
    --spin) SPIN="$2"; shift 2;;
    --sim-cpu) SIM_CPU="$2"; shift 2;;
    --engine-cpu) ENGINE_CPU="$2"; shift 2;;
    --net-cpu) NET_CPU="$2"; shift 2;;
    --build) BUILD="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "bench-e2e: unknown argument $1" >&2; usage >&2; exit 2;;
  esac
done
case "$BACKEND" in kernel|af_xdp) ;; *) echo "bench-e2e: --backend kernel|af_xdp" >&2; exit 2;; esac
case "$SPIN" in busy|adaptive) ;; *) echo "bench-e2e: --spin busy|adaptive" >&2; exit 2;; esac
[[ "$DURATION" =~ ^[0-9]+$ && "$RUNS" =~ ^[0-9]+$ ]] || { echo "bench-e2e: --duration and --runs take whole numbers" >&2; exit 2; }
for b in fastmm-sim-itch fastmm-live fastmm-top; do
  [[ -x "$BUILD/bin/$b" ]] || { echo "bench-e2e: $BUILD/bin/$b not found (cmake --build --preset release)" >&2; exit 2; }
done
BUILD="$(cd "$BUILD" && pwd)"

if [[ -z "${FASTMM_BENCH_E2E_NS:-}" ]]; then
  if [[ "$BACKEND" == af_xdp && "$(id -u)" -ne 0 ]]; then
    echo "bench-e2e: --backend af_xdp loads an XDP program and needs root: sudo $0 --backend af_xdp" >&2
    exit 1
  fi
  [[ -n "$OUT" ]] || OUT="runs/bench-e2e-$(date -u +%Y%m%d-%H%M%S)-$BACKEND"
  mkdir -p "$OUT"
  OUT="$(cd "$OUT" && pwd)"
  args=(--backend "$BACKEND" --duration "$DURATION" --runs "$RUNS" --speed "$SPEED" --spin "$SPIN"
        --sim-cpu "$SIM_CPU" --engine-cpu "$ENGINE_CPU" --net-cpu "$NET_CPU" --build "$BUILD" --out "$OUT")
  # Root keeps its capabilities in a plain network namespace; everyone else maps to root in a new
  # user namespace, which is enough for veth pairs, addresses, routes and multicast.
  if [[ "$(id -u)" -eq 0 ]]; then
    exec env FASTMM_BENCH_E2E_NS=1 unshare -n "$0" "${args[@]}"
  fi
  if ! unshare -Urn true 2>/dev/null; then
    echo "bench-e2e: cannot create a user and network namespace (unshare -Urn); see kernel.unprivileged_userns_clone" >&2
    exit 77
  fi
  exec env FASTMM_BENCH_E2E_NS=1 unshare -Urn "$0" "${args[@]}"
fi

# ---- inside the simulator's namespace -------------------------------------------------------
SIM_IP=10.211.0.1; LIVE_IP=10.211.0.2
ip link set lo up
unshare -n sleep infinity &   # holds fastmm-live's namespace
PEER=$!
cleanup() { kill "$PEER" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.2
ip link add fmsim type veth peer name fmlive
ip link set fmlive netns "$PEER"
ip addr add "$SIM_IP/24" dev fmsim
ip link set fmsim up multicast on
ip route add 224.0.0.0/4 dev fmsim
in_live() { nsenter -t "$PEER" -n "$@"; }
in_live ip link set lo up
in_live ip addr add "$LIVE_IP/24" dev fmlive
in_live ip link set fmlive up multicast on
in_live ip route add 224.0.0.0/4 dev fmlive
for _ in $(seq 50); do in_live ping -c1 -W1 "$SIM_IP" >/dev/null 2>&1 && break; sleep 0.1; done

CFG="$OUT/nasdaq-itch-bench.toml"
NET_CPUS="[$NET_CPU]"; [[ "$NET_CPU" == -1 ]] && NET_CPUS="[]"
sed -e "s/^interface = \"lo\"/interface = \"fmlive\"/" \
    -e "s/127\.0\.0\.1:/$SIM_IP:/" \
    -e "s/^rx_backend = \"kernel\"/rx_backend = \"$BACKEND\"/" \
    -e "s/^spin_mode = .*/spin_mode = \"$SPIN\"/" \
    -e "s/^cpu = -1 .*/cpu = $ENGINE_CPU/" \
    -e "s/^net_cpus = \[\].*/net_cpus = $NET_CPUS/" \
    -e "s/^name = \"nasdaq-itch-sim\"/name = \"nasdaq-itch-bench\"/" \
    configs/nasdaq-itch-sim.toml > "$CFG"
SIM_OPTS=(); [[ "$SPIN" == busy ]] && SIM_OPTS+=(--busy-poll)
[[ "$SIM_CPU" == -1 ]] || SIM_OPTS+=(--cpu "$SIM_CPU")
LIVE_PIN=(); [[ "$ENGINE_CPU" == -1 || "$NET_CPU" == -1 ]] || LIVE_PIN=(taskset -c "$ENGINE_CPU,$NET_CPU")

echo "bench-e2e: backend=$BACKEND spin=$SPIN duration=${DURATION}s runs=$RUNS speed=$SPEED cpus sim=$SIM_CPU engine=$ENGINE_CPU net=$NET_CPU"
echo "bench-e2e: veth fmsim ($SIM_IP) <-> fmlive ($LIVE_IP), output $OUT"
for run in $(seq "$RUNS"); do
  d="$OUT/run$run"; mkdir -p "$d"
  "$BUILD/bin/fastmm-sim-itch" --config configs/sim-itch.toml --bind "$SIM_IP" --interface fmsim \
    "${SIM_OPTS[@]}" --speed "$SPEED" --duration "$((DURATION + 30))s" \
    --stats-interval 0 --summary-json "$d/sim.json" > "$d/sim.log" 2>&1 &
  SIM=$!
  sleep 1
  rc=0
  in_live "${LIVE_PIN[@]}" "$BUILD/bin/fastmm-live" --config "$CFG" \
    --duration "${DURATION}s" --no-journal --status "$d/live.status" > "$d/live.log" 2>&1 || rc=$?
  "$BUILD/bin/fastmm-top" --json --path "$d/live.status" > "$d/live.json" || true
  kill -INT "$SIM" 2>/dev/null || true
  wait "$SIM" || true
  if [[ $rc -ne 0 ]]; then
    echo "bench-e2e: run $run: fastmm-live exited with $rc (see $d/live.log)" >&2
    exit 1
  fi
  python3 - "$d/sim.json" "$d/live.json" "$run" <<'PY'
import json, sys
sim = json.load(open(sys.argv[1]))
live = json.load(open(sys.argv[2]))
venue = live["venues"][0]
def row(name, count, p50, p99, p999):
    f = lambda ns: f"{ns / 1000:.1f}" if ns else "-"
    print(f"| {name:<34} | {count:>9} | {f(p50):>9} | {f(p99):>9} | {f(p999):>9} |")
w = sim["wire_to_wire_ns"]
print(f"\nrun {sys.argv[3]}: {live['orders_sent']} orders, {live['replaces_sent']} replaces, "
      f"{live['fills']} fills, {venue['md_messages']} ITCH messages, feed {venue['feed']['state']}, "
      f"gaps {venue['feed']['gaps']}, tokens {sim['tokens']}, misses {sim['misses']}")
print(f"| {'hop (us)':<34} | {'count':>9} | {'p50':>9} | {'p99':>9} | {'p99.9':>9} |")
print(f"|{'-' * 36}|{'-' * 10}:|{'-' * 10}:|{'-' * 10}:|{'-' * 10}:|")
row("wire to wire (sim)", w["count"], w["p50"], w["p99"], w["p999"])
k = venue["feed"]["kernel_to_t0"]
row("kernel to T0 (net thread)", k["count"], k["p50_ns"], k["p99_ns"], k["p999_ns"])
names = {"decode": "T0 to T1 decode + L3 (net thread)", "book_apply": "T1 to T2 ring + book apply",
         "strategy": "T2 to T3 strategy", "serialize": "T3 to T4 serialize",
         "send": "T4 to T5 outbound ring", "tick_to_trade": "T0 to T5 tick to trade (engine)"}
for key, label in names.items():
    l = live["latency"][key]
    row(label, l["count"], l["p50_ns"], l["p99_ns"], l["p999_ns"])
t = venue["wire_tick_to_trade"]
row("T0 to OUCH write (net thread)", t["count"], t["p50_ns"], t["p99_ns"], t["p999_ns"])
if venue["feed"]["state"] != "live" or venue["books_synced"] != venue["books_total"] or live["orders_sent"] == 0:
    print("bench-e2e: the feed was not live or no order was sent", file=sys.stderr)
    sys.exit(1)
PY
done
