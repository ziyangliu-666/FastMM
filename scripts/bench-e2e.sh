#!/usr/bin/env bash
# End-to-end latency: fastmm-sim-itch and fastmm-live in two network namespaces joined by a veth
# pair (ADR-0015, section 6). Market data (ITCH over MoldUDP64, lines A and B), GLIMPSE,
# re-requests and OUCH orders cross the veth. BasicMM trades on the nasdaq_itch venue with
# order_entry = "sim_ouch"; every order names the ITCH sequence number that triggered it, and the
# simulator times it from the sendmmsg of that datagram to the read that returned the order.
#
#   scripts/bench-e2e.sh [--backend kernel|af_xdp|dpdk] [--order-transport kernel|user_tcp]
#                        [--md multicast|unicast] [--dpdk-exception]
#                        [--user-tcp-ip 10.211.0.3] [--user-tcp-port 0]
#                        [--duration 30] [--runs 3] [--speed 4]
#                        [--spin busy|adaptive] [--threading split|single] [--replay]
#                        [--sim-cpu 2] [--engine-cpu 4] [--net-cpu 6]
#                        [--build build/release] [--out runs/bench-e2e-<time>]
#
# A CPU of -1 leaves that process unpinned (ctest runs it that way). --threading single runs the
# venue on the engine thread ([engine] threading, --net-cpu unused). --replay journals the session
# and requires fastmm-replay --verify to match it. Exit codes: 0 ok, 1 a run
# failed (fastmm-live exit code, feed not live, no orders), 2 usage, 77 no namespaces here.
# kernel runs unprivileged in `unshare -Urn`. af_xdp loads an XDP program and needs root:
#   sudo scripts/bench-e2e.sh --backend af_xdp
# dpdk (a build with -DFASTMM_WITH_DPDK=ON, --spin busy) runs unprivileged too: EAL with
# --no-huge --no-pci --in-memory and the net_af_packet vdev on fmlive. --dpdk-exception takes
# fmlive's address away and gives it to a net_tap interface (fmx0) behind the DPDK port: the
# kernel's traffic (ARP, GLIMPSE, re-requests, kernel OUCH) then goes through fastmm-live, as on a
# NIC bound to vfio-pci. --md unicast sends the ITCH lines to fastmm-live's address instead of
# multicast groups. --order-transport user_tcp sends OUCH through the user-space TCP (UserTcp)
# from 10.211.0.3 (--user-tcp-ip; fastmm-live's own 10.211.0.2 needs --user-tcp-port and af_xdp
# or dpdk) over the backend's device (AF_PACKET ring, XDP socket or DPDK port); the simulator's
# veth then has TSO/GSO (and, off the kernel backend, checksum offload) off.
# Prints p50 / p99 / p99.9 per run: wire to wire (simulator), kernel to T0 (fastmm-live network
# thread), the engine's hops and tick-to-trade, and the network thread's tick-to-trade. The JSON
# inputs stay in the output directory. Two hosts: scripts/bench-2host.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

BACKEND=kernel; TRANSPORT=kernel; MD=multicast; EXCEPTION=0; DURATION=30; RUNS=1; SPEED=4; SPIN=busy
THREADING=split; REPLAY=0; SIM_CPU=2; ENGINE_CPU=4; NET_CPU=6; BUILD=build/release; OUT=""
USER_TCP_IP=10.211.0.3; USER_TCP_PORT=0
usage() { sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend) BACKEND="$2"; shift 2;;
    --order-transport) TRANSPORT="$2"; shift 2;;
    --md) MD="$2"; shift 2;;
    --dpdk-exception) EXCEPTION=1; shift;;
    --user-tcp-ip) USER_TCP_IP="$2"; shift 2;;
    --user-tcp-port) USER_TCP_PORT="$2"; shift 2;;
    --duration) DURATION="$2"; shift 2;;
    --runs) RUNS="$2"; shift 2;;
    --speed) SPEED="$2"; shift 2;;
    --spin) SPIN="$2"; shift 2;;
    --threading) THREADING="$2"; shift 2;;
    --replay) REPLAY=1; shift;;
    --sim-cpu) SIM_CPU="$2"; shift 2;;
    --engine-cpu) ENGINE_CPU="$2"; shift 2;;
    --net-cpu) NET_CPU="$2"; shift 2;;
    --build) BUILD="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "bench-e2e: unknown argument $1" >&2; usage >&2; exit 2;;
  esac
done
case "$BACKEND" in kernel|af_xdp|dpdk) ;; *) echo "bench-e2e: --backend kernel|af_xdp|dpdk" >&2; exit 2;; esac
case "$TRANSPORT" in kernel|user_tcp) ;; *) echo "bench-e2e: --order-transport kernel|user_tcp" >&2; exit 2;; esac
case "$MD" in multicast|unicast) ;; *) echo "bench-e2e: --md multicast|unicast" >&2; exit 2;; esac
[[ "$BACKEND" != dpdk || "$SPIN" == busy ]] || { echo "bench-e2e: --backend dpdk needs --spin busy" >&2; exit 2; }
[[ "$EXCEPTION" == 0 || "$BACKEND" == dpdk ]] || { echo "bench-e2e: --dpdk-exception needs --backend dpdk" >&2; exit 2; }
case "$SPIN" in busy|adaptive) ;; *) echo "bench-e2e: --spin busy|adaptive" >&2; exit 2;; esac
case "$THREADING" in split|single) ;; *) echo "bench-e2e: --threading split|single" >&2; exit 2;; esac
[[ "$DURATION" =~ ^[0-9]+$ && "$RUNS" =~ ^[0-9]+$ ]] || { echo "bench-e2e: --duration and --runs take whole numbers" >&2; exit 2; }
for b in fastmm-sim-itch fastmm-live fastmm-top fastmm-replay; do
  [[ -x "$BUILD/bin/$b" ]] || { echo "bench-e2e: $BUILD/bin/$b not found (cmake --build --preset release)" >&2; exit 2; }
done
BUILD="$(cd "$BUILD" && pwd)"

if [[ -z "${FASTMM_BENCH_E2E_NS:-}" ]]; then
  if [[ "$BACKEND" == af_xdp && "$(id -u)" -ne 0 ]]; then
    echo "bench-e2e: --backend af_xdp loads an XDP program and needs root: sudo $0 --backend af_xdp" >&2
    exit 1
  fi
  [[ -n "$OUT" ]] || OUT="runs/bench-e2e-$(date -u +%Y%m%d-%H%M%S)-$BACKEND-$TRANSPORT-$MD"
  mkdir -p "$OUT"
  OUT="$(cd "$OUT" && pwd)"
  args=(--backend "$BACKEND" --order-transport "$TRANSPORT" --md "$MD" --duration "$DURATION" --runs "$RUNS"
        --speed "$SPEED" --spin "$SPIN" --threading "$THREADING"
        --sim-cpu "$SIM_CPU" --engine-cpu "$ENGINE_CPU" --net-cpu "$NET_CPU" --build "$BUILD" --out "$OUT"
        --user-tcp-ip "$USER_TCP_IP" --user-tcp-port "$USER_TCP_PORT")
  [[ "$EXCEPTION" == 1 ]] && args+=(--dpdk-exception)
  [[ "$REPLAY" == 1 ]] && args+=(--replay)
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
in_live ip link set fmlive up multicast on
if [[ "$EXCEPTION" == 1 ]]; then
  # fmlive keeps no address: fastmm-live puts LIVE_IP on its tap. Strict reverse-path filtering and
  # arp_ignore keep the kernel from also answering on fmlive (it sees every frame there).
  in_live sysctl -qw net.ipv4.conf.all.rp_filter=1 net.ipv4.conf.fmlive.rp_filter=1 \
    net.ipv4.conf.fmlive.arp_ignore=1 >/dev/null
else
  in_live ip addr add "$LIVE_IP/24" dev fmlive
  in_live ip route add 224.0.0.0/4 dev fmlive
  for _ in $(seq 50); do in_live ping -c1 -W1 "$SIM_IP" >/dev/null 2>&1 && break; sleep 0.1; done
fi
if [[ "$TRANSPORT" == user_tcp || "$EXCEPTION" == 1 ]]; then
  command -v ethtool >/dev/null || { echo "bench-e2e: user_tcp and --dpdk-exception need ethtool" >&2; exit 77; }
  # The live side sees the simulator's TCP segments as sent: no TSO/GSO super-segments.
  [[ "$TRANSPORT" == user_tcp ]] && ethtool -K fmsim tso off gso off >/dev/null
  # XDP and DPDK's af_packet PMD do not report a checksum left to offload (nor pass it on to the
  # exception tap): have it computed.
  [[ "$BACKEND" == kernel ]] || ethtool -K fmsim tx off >/dev/null
fi
LINE_A=239.192.0.1:31001; LINE_B=239.192.0.2:31002; MD_IF=fmlive
if [[ "$MD" == unicast ]]; then LINE_A=$LIVE_IP:31001; LINE_B=$LIVE_IP:31002; fi
EXTRA=""
if [[ "$BACKEND" == dpdk ]]; then
  VDEVS="--vdev=net_af_packet0,iface=fmlive,framecnt=4096"
  if [[ "$EXCEPTION" == 1 ]]; then
    VDEVS+=" --vdev=net_tap0,iface=fmx0"
    EXTRA+="dpdk_exception_port = \"net_tap0\"\ndpdk_exception_ip = \"$LIVE_IP/24\"\n"
    MD_IF=fmx0
  fi
  EXTRA+="dpdk_port = \"net_af_packet0\"\n"
  EXTRA+="dpdk_eal_args = \"--no-huge --no-pci --in-memory --no-telemetry -l 0 -m 128 $VDEVS\"\n"
fi
if [[ "$TRANSPORT" == user_tcp ]]; then
  EXTRA+="order_transport = \"user_tcp\"\nuser_tcp_ip = \"$USER_TCP_IP\"\nuser_tcp_interface = \"fmlive\"\n"
  EXTRA+="user_tcp_port = $USER_TCP_PORT\n"
fi

EXTRA="${EXTRA//\//\\/}"  # a sed replacement below
CFG="$OUT/nasdaq-itch-bench.toml"
NET_CPUS="[$NET_CPU]"; [[ "$NET_CPU" == -1 ]] && NET_CPUS="[]"
sed -e "s/^interface = \"lo\"/interface = \"$MD_IF\"/" \
    -e "s/127\.0\.0\.1:/$SIM_IP:/" \
    -e "s/^line_a = .*/line_a = \"$LINE_A\"/" \
    -e "s/^line_b = .*/line_b = \"$LINE_B\"/" \
    -e "s/^rx_backend = \"kernel\"/rx_backend = \"$BACKEND\"/" \
    -e "s/^spin_mode = .*/spin_mode = \"$SPIN\"/" \
    -e "s/^cpu = -1 .*/cpu = $ENGINE_CPU/" \
    -e "s/^net_cpus = \[\].*/net_cpus = $NET_CPUS/" \
    -e "s/^name = \"nasdaq-itch-sim\"/name = \"nasdaq-itch-bench\"/" \
    -e "s/^\[engine\]$/[engine]\nthreading = \"$THREADING\"/" \
    -e "s/^\(ouch_url = .*\)$/\1\n$EXTRA/" \
    configs/nasdaq-itch-sim.toml > "$CFG"
SIM_OPTS=(--line-a "$LINE_A" --line-b "$LINE_B"); [[ "$SPIN" == busy ]] && SIM_OPTS+=(--busy-poll)
[[ "$SIM_CPU" == -1 ]] || SIM_OPTS+=(--cpu "$SIM_CPU")
LIVE_PIN=(); [[ "$ENGINE_CPU" == -1 || "$NET_CPU" == -1 ]] || LIVE_PIN=(taskset -c "$ENGINE_CPU,$NET_CPU")

echo "bench-e2e: backend=$BACKEND order_transport=$TRANSPORT md=$MD exception=$EXCEPTION spin=$SPIN threading=$THREADING duration=${DURATION}s runs=$RUNS speed=$SPEED cpus sim=$SIM_CPU engine=$ENGINE_CPU net=$NET_CPU"
echo "bench-e2e: veth fmsim ($SIM_IP) <-> fmlive ($LIVE_IP), output $OUT"
for run in $(seq "$RUNS"); do
  d="$OUT/run$run"; mkdir -p "$d"
  "$BUILD/bin/fastmm-sim-itch" --config configs/sim-itch.toml --bind "$SIM_IP" --interface fmsim \
    "${SIM_OPTS[@]}" --speed "$SPEED" --duration "$((DURATION + 30))s" \
    --stats-interval 0 --summary-json "$d/sim.json" > "$d/sim.log" 2>&1 &
  SIM=$!
  sleep 1
  rc=0
  JOURNAL=(--no-journal); [[ "$REPLAY" == 1 ]] && JOURNAL=(--journal "$d/live.fmj")
  in_live "${LIVE_PIN[@]}" "$BUILD/bin/fastmm-live" --config "$CFG" \
    --duration "${DURATION}s" "${JOURNAL[@]}" --status "$d/live.status" > "$d/live.log" 2>&1 || rc=$?
  "$BUILD/bin/fastmm-top" --json --path "$d/live.status" > "$d/live.json" || true
  kill -INT "$SIM" 2>/dev/null || true
  wait "$SIM" || true
  if [[ $rc -ne 0 ]]; then
    echo "bench-e2e: run $run: fastmm-live exited with $rc (see $d/live.log)" >&2
    exit 1
  fi
  python3 scripts/bench-table.py "$d/sim.json" "$d/live.json" "$run"
  if [[ "$REPLAY" == 1 ]]; then
    "$BUILD/bin/fastmm-replay" --journal "$d/live.fmj" --verify > "$d/replay.log" 2>&1 || {
      echo "bench-e2e: run $run: fastmm-replay did not match the journal (see $d/replay.log)" >&2
      exit 1
    }
    grep -m1 -E '^replay (MATCH|MISMATCH)' "$d/replay.log"
  fi
done
