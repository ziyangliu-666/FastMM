#!/usr/bin/env bash
# End-to-end latency across two hosts: fastmm-sim-itch on a remote host (over ssh), fastmm-live
# here. The same measurement and table as scripts/bench-e2e.sh; run it on the live host as root.
#
#   scripts/bench-2host.sh --remote-sim root@<sim host> --sim-ip <sim VPC ip> --live-ip <live VPC ip>
#       [--iface <live VPC nic>] [--sim-iface <sim VPC nic>] [--prefix-len 24]
#       [--backend kernel|af_xdp|dpdk] [--order-transport kernel|user_tcp]
#       [--md unicast|multicast] [--user-tcp-ip <ip>] [--user-tcp-port 61001]
#       [--dpdk-pci 0000:06:00.0] [--dpdk-eal "<extra EAL args>"] [--queues 0,1]
#       [--duration 30] [--runs 3] [--speed 4] [--spin busy|adaptive] [--threading split|single]
#       [--sim-cpu 1] [--engine-cpu 1] [--net-cpu 2] [--remote-dir /opt/fastmm] [--push]
#       [--build <dir with bin/>] [--out runs/bench-2host-<time>]
#
# Market data is unicast by default: the simulator sends lines A and B to <live ip>:31001/31002
# (cloud VPCs rarely carry multicast). GLIMPSE, re-requests and OUCH go to the simulator's
# <sim ip> ports 31010, 31000 and 31020.
#
# Backends on this host:
#   kernel  sockets on --iface (which has --live-ip)
#   af_xdp  the XDP program on --iface (native mode on virtio_net) and a socket on each of its RX
#           queues, or on the ones listed with --queues; scripts/host-setup.sh xdp-prep keeps
#           them few (virtio_net adds a queue pair per CPU while the program is attached)
#   dpdk    the NIC bound to vfio-pci (scripts/host-setup.sh dpdk-bind <iface>); fastmm-live gives
#           the kernel a tap (fmx0) with --live-ip/--prefix-len through the DPDK port for ARP,
#           GLIMPSE, re-requests and kernel TCP; hugepages required
# --order-transport user_tcp: OUCH over UserTcp on the backend's device. Its address is
# --user-tcp-ip (not assigned anywhere, on the VPC subnet) or, by default on af_xdp and dpdk,
# --live-ip with the fixed local port --user-tcp-port (VPCs may drop addresses they did not
# assign). The simulator host's NIC gets TSO/GSO off (so segments arrive unmerged).
#
# --push copies fastmm-sim-itch and configs/ to --remote-dir first. Output: the live config, logs,
# sim.json and live.json per run in --out. Exit codes as bench-e2e.sh (0 ok, 1 a run failed,
# 2 usage).
set -euo pipefail
cd "$(dirname "$0")/.."

REMOTE=""; SIM_IP=""; LIVE_IP=""; IFACE=""; SIM_IFACE=""; PREFIX_LEN=24
BACKEND=kernel; TRANSPORT=kernel; MD=unicast; USER_TCP_IP=""; USER_TCP_PORT=61001
DPDK_PCI=""; DPDK_EAL=""; QUEUES=""; DURATION=30; RUNS=1; SPEED=4; SPIN=busy; THREADING=split
SIM_CPU=1; ENGINE_CPU=1; NET_CPU=2; REMOTE_DIR=/opt/fastmm; PUSH=0; BUILD=build/release; OUT=""
usage() { sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    --remote-sim) REMOTE="$2"; shift 2;;
    --sim-ip) SIM_IP="$2"; shift 2;;
    --live-ip) LIVE_IP="$2"; shift 2;;
    --iface) IFACE="$2"; shift 2;;
    --sim-iface) SIM_IFACE="$2"; shift 2;;
    --prefix-len) PREFIX_LEN="$2"; shift 2;;
    --backend) BACKEND="$2"; shift 2;;
    --order-transport) TRANSPORT="$2"; shift 2;;
    --md) MD="$2"; shift 2;;
    --user-tcp-ip) USER_TCP_IP="$2"; shift 2;;
    --user-tcp-port) USER_TCP_PORT="$2"; shift 2;;
    --dpdk-pci) DPDK_PCI="$2"; shift 2;;
    --dpdk-eal) DPDK_EAL="$2"; shift 2;;
    --queues) QUEUES="$2"; shift 2;;
    --duration) DURATION="$2"; shift 2;;
    --runs) RUNS="$2"; shift 2;;
    --speed) SPEED="$2"; shift 2;;
    --spin) SPIN="$2"; shift 2;;
    --threading) THREADING="$2"; shift 2;;
    --sim-cpu) SIM_CPU="$2"; shift 2;;
    --engine-cpu) ENGINE_CPU="$2"; shift 2;;
    --net-cpu) NET_CPU="$2"; shift 2;;
    --remote-dir) REMOTE_DIR="$2"; shift 2;;
    --push) PUSH=1; shift;;
    --build) BUILD="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "bench-2host: unknown argument $1" >&2; usage >&2; exit 2;;
  esac
done
die() { echo "bench-2host: $*" >&2; exit 2; }
[[ -n "$REMOTE" && -n "$SIM_IP" && -n "$LIVE_IP" ]] || { usage >&2; exit 2; }
case "$BACKEND" in kernel|af_xdp|dpdk) ;; *) die "--backend kernel|af_xdp|dpdk";; esac
case "$TRANSPORT" in kernel|user_tcp) ;; *) die "--order-transport kernel|user_tcp";; esac
case "$MD" in unicast|multicast) ;; *) die "--md unicast|multicast";; esac
case "$SPIN" in busy|adaptive) ;; *) die "--spin busy|adaptive";; esac
case "$THREADING" in split|single) ;; *) die "--threading split|single";; esac
[[ "$BACKEND" != dpdk || "$SPIN" == busy ]] || die "--backend dpdk needs --spin busy"
[[ "$BACKEND" == dpdk || -n "$IFACE" ]] || die "--iface is required with --backend $BACKEND"
[[ "$BACKEND" != dpdk || -n "$DPDK_PCI" ]] || die "--dpdk-pci is required with --backend dpdk"
if [[ "$TRANSPORT" == user_tcp && -z "$USER_TCP_IP" ]]; then
  [[ "$BACKEND" != kernel ]] || die "user_tcp on the kernel backend needs --user-tcp-ip (an unassigned address)"
  USER_TCP_IP="$LIVE_IP"
fi
[[ "$USER_TCP_IP" == "$LIVE_IP" ]] || USER_TCP_PORT=0
[[ "$(id -u)" -eq 0 || "$BACKEND" == kernel ]] || die "--backend $BACKEND needs root"
for b in fastmm-live fastmm-top; do
  [[ -x "$BUILD/bin/$b" ]] || die "$BUILD/bin/$b not found"
done
BUILD="$(cd "$BUILD" && pwd)"
[[ -n "$OUT" ]] || OUT="runs/bench-2host-$(date -u +%Y%m%d-%H%M%S)-$BACKEND-$TRANSPORT-$MD"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "$REMOTE")
TAG="fastmm-bench-$$"

if [[ "$PUSH" == 1 ]]; then
  [[ -x "$BUILD/bin/fastmm-sim-itch" ]] || die "$BUILD/bin/fastmm-sim-itch not found (for --push)"
  "${SSH[@]}" "mkdir -p '$REMOTE_DIR/bin' '$REMOTE_DIR/configs'"
  scp -q "$BUILD/bin/fastmm-sim-itch" "$REMOTE:$REMOTE_DIR/bin/"
  scp -q configs/sim-itch.toml "$REMOTE:$REMOTE_DIR/configs/"
fi
"${SSH[@]}" "test -x '$REMOTE_DIR/bin/fastmm-sim-itch' && test -f '$REMOTE_DIR/configs/sim-itch.toml'" ||
  die "$REMOTE:$REMOTE_DIR has no bin/fastmm-sim-itch or configs/sim-itch.toml (use --push)"
if [[ -z "$SIM_IFACE" ]]; then
  SIM_IFACE="$("${SSH[@]}" "ip -o -4 addr show | awk -v ip='$SIM_IP' '{split(\$4,a,\"/\"); if (a[1]==ip) print \$2}'")"
  [[ -n "$SIM_IFACE" ]] || die "no interface on $REMOTE has $SIM_IP (pass --sim-iface)"
fi
if [[ "$TRANSPORT" == user_tcp ]]; then
  "${SSH[@]}" "ethtool -K '$SIM_IFACE' tso off gso off" >/dev/null 2>&1 ||
    echo "bench-2host: warning: could not turn TSO/GSO off on $REMOTE $SIM_IFACE" >&2
fi

# ---- fastmm-live's configuration -------------------------------------------------------------
if [[ "$MD" == unicast ]]; then
  LINE_A="$LIVE_IP:31001"; LINE_B="$LIVE_IP:31002"
else
  LINE_A="239.192.0.1:31001"; LINE_B="239.192.0.2:31002"
fi
MD_IF="$IFACE"
EXTRA=""
if [[ "$BACKEND" == dpdk ]]; then
  MD_IF=fmx0
  EXTRA+="dpdk_port = \"$DPDK_PCI\"\n"
  ALLOW=""; [[ "$DPDK_PCI" == *:* ]] && ALLOW="-a $DPDK_PCI"  # a PCI address, not a vdev name
  EXTRA+="dpdk_eal_args = \"-l 0 --in-memory --no-telemetry $ALLOW --vdev=net_tap0,iface=fmx0 $DPDK_EAL\"\n"
  EXTRA+="dpdk_exception_port = \"net_tap0\"\ndpdk_exception_ip = \"$LIVE_IP/$PREFIX_LEN\"\n"
  # Kernel TCP (GLIMPSE, kernel OUCH) through the tap: read it on every poll when OUCH uses it.
  [[ "$TRANSPORT" == kernel ]] && EXTRA+="dpdk_exception_interval_us = 0\n"
fi
[[ -z "$QUEUES" ]] || EXTRA+="queues = [$QUEUES]\n"
if [[ "$TRANSPORT" == user_tcp ]]; then
  EXTRA+="order_transport = \"user_tcp\"\nuser_tcp_ip = \"$USER_TCP_IP\"\nuser_tcp_port = $USER_TCP_PORT\n"
  [[ -z "$IFACE" ]] || EXTRA+="user_tcp_interface = \"$IFACE\"\n"
fi
EXTRA="${EXTRA//\//\\/}"
CFG="$OUT/nasdaq-itch-2host.toml"
NET_CPUS="[$NET_CPU]"; [[ "$NET_CPU" == -1 ]] && NET_CPUS="[]"
sed -e "s/^interface = \"lo\"/interface = \"$MD_IF\"/" \
    -e "s/127\.0\.0\.1:/$SIM_IP:/" \
    -e "s/^line_a = .*/line_a = \"$LINE_A\"/" \
    -e "s/^line_b = .*/line_b = \"$LINE_B\"/" \
    -e "s/^rx_backend = \"kernel\"/rx_backend = \"$BACKEND\"/" \
    -e "s/^spin_mode = .*/spin_mode = \"$SPIN\"/" \
    -e "s/^cpu = -1 .*/cpu = $ENGINE_CPU/" \
    -e "s/^net_cpus = \[\].*/net_cpus = $NET_CPUS/" \
    -e "s/^name = \"nasdaq-itch-sim\"/name = \"nasdaq-itch-2host\"/" \
    -e "s/^\[engine\]$/[engine]\nthreading = \"$THREADING\"/" \
    -e "s/^\(ouch_url = .*\)$/\1\n$EXTRA/" \
    configs/nasdaq-itch-sim.toml > "$CFG"

SIM_OPTS=(--config configs/sim-itch.toml --bind "$SIM_IP" --interface "$SIM_IFACE"
          --line-a "$LINE_A" --line-b "$LINE_B" --speed "$SPEED" --stats-interval 0)
[[ "$MD" == multicast ]] && SIM_OPTS+=(--ttl 8)
[[ "$SPIN" == busy ]] && SIM_OPTS+=(--busy-poll)
[[ "$SIM_CPU" == -1 ]] || SIM_OPTS+=(--cpu "$SIM_CPU")
LIVE_PIN=(); [[ "$ENGINE_CPU" == -1 || "$NET_CPU" == -1 ]] || LIVE_PIN=(taskset -c "$ENGINE_CPU,$NET_CPU")
RDIR="/tmp/$TAG"
stop_sim() { "${SSH[@]}" "test -f $RDIR/sim.pid && kill -INT \$(cat $RDIR/sim.pid) 2>/dev/null; true" || true; }
trap stop_sim EXIT

echo "bench-2host: backend=$BACKEND order_transport=$TRANSPORT md=$MD spin=$SPIN threading=$THREADING duration=${DURATION}s runs=$RUNS speed=$SPEED"
echo "bench-2host: sim $REMOTE ($SIM_IP on $SIM_IFACE) -> live $(hostname) ($LIVE_IP${IFACE:+ on $IFACE}), output $OUT"
[[ "$TRANSPORT" == kernel ]] || echo "bench-2host: user_tcp from $USER_TCP_IP (local port ${USER_TCP_PORT/#0/random})"
for run in $(seq "$RUNS"); do
  d="$OUT/run$run"; mkdir -p "$d"
  # The simulator runs until stopped (SIGINT after fastmm-live exits) or DURATION + 60 s.
  "${SSH[@]}" "mkdir -p $RDIR && cd '$REMOTE_DIR' && { bin/fastmm-sim-itch ${SIM_OPTS[*]} --duration $((DURATION + 60))s --summary-json $RDIR/sim.json > $RDIR/sim.log 2>&1 < /dev/null & echo \$! > $RDIR/sim.pid; }"
  sleep 2
  "${SSH[@]}" "kill -0 \$(cat $RDIR/sim.pid)" 2>/dev/null || {
    "${SSH[@]}" "cat $RDIR/sim.log" >&2 || true
    echo "bench-2host: the simulator did not start" >&2; exit 1; }
  rc=0
  "${LIVE_PIN[@]}" "$BUILD/bin/fastmm-live" --config "$CFG" --duration "${DURATION}s" --no-journal \
    --status "$d/live.status" > "$d/live.log" 2>&1 || rc=$?
  "$BUILD/bin/fastmm-top" --json --path "$d/live.status" > "$d/live.json" || true
  stop_sim
  for _ in $(seq 50); do
    "${SSH[@]}" "kill -0 \$(cat $RDIR/sim.pid) 2>/dev/null" || break
    sleep 0.2
  done
  scp -q "$REMOTE:$RDIR/sim.json" "$d/sim.json" || true
  scp -q "$REMOTE:$RDIR/sim.log" "$d/sim.log" || true
  if [[ $rc -ne 0 ]]; then
    echo "bench-2host: run $run: fastmm-live exited with $rc (see $d/live.log)" >&2
    exit 1
  fi
  [[ -s "$d/sim.json" ]] || { echo "bench-2host: run $run: no sim.json (see $d/sim.log)" >&2; exit 1; }
  python3 scripts/bench-table.py "$d/sim.json" "$d/live.json" "$run"
done
