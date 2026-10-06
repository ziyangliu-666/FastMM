#!/usr/bin/env bash
# Prepares a fresh Ubuntu 24.04 VM (virtio_net) for the two-host benchmark (scripts/bench-2host.sh),
# and tunes a live host. Run as root; info, cstates show and --dry-run need no root.
#
#   scripts/host-setup.sh deps                 runtime and tool packages (apt)
#   scripts/host-setup.sh tune [--hugepages N] [--cstate-max-latency US [--cstate-cpus LIST]]
#                                              hugepages (N x 2 MiB, default 512), irqbalance off,
#                                              NIC interrupts to CPU 0, performance governor where
#                                              there is one, THP and NUMA balancing off; with
#                                              --cstate-max-latency also cstates limit US
#   scripts/host-setup.sh cstates [show] [--cpus LIST]
#                                              each CPU's idle states: exit latency (us), disabled
#   scripts/host-setup.sh cstates limit <US> [--cpus LIST] [--dry-run]
#                                              disable the idle states whose exit latency exceeds
#                                              US on LIST (default: every CPU); until reboot
#   scripts/host-setup.sh cstates restore [--cpus LIST] [--dry-run]
#                                              enable every idle state again
#   scripts/host-setup.sh firewall <iface>     when ufw is active: allow everything from <iface>'s
#                                              subnet (Vultr images enable ufw)
#   scripts/host-setup.sh xdp-prep <iface>     one combined queue and no GRO/LRO on <iface> (af_xdp),
#                                              then firewall <iface>
#   scripts/host-setup.sh dpdk-bind <iface>    vfio (no-IOMMU mode) and vfio-pci loaded, <iface>'s PCI
#                                              function bound to vfio-pci; prints its PCI address
#   scripts/host-setup.sh dpdk-unbind <pci>    back to the kernel driver, addresses restored
#   scripts/host-setup.sh info                 CPUs, NICs, drivers, queues, hugepages, vfio, governor
#   scripts/host-setup.sh all                  deps, tune, info
#
# dpdk-bind refuses the interface of the default route (keep the public NIC for SSH), and runs
# firewall <iface> first. Its state (driver, MAC, addresses) is kept in /var/lib/fastmm for
# dpdk-unbind.
set -euo pipefail

STATE=/var/lib/fastmm
# Another root for /sys (a test tree); the default is the live one.
SYSFS="${HOST_SETUP_SYSFS:-/sys}"
usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }
die() { echo "host-setup: $*" >&2; exit 1; }
need_root() { [[ $EUID -eq 0 ]] || die "run as root${1:+ $1}"; }
[[ $# -ge 1 ]] || { usage; exit 2; }
[[ "$1" == -h || "$1" == --help ]] && { usage; exit 0; }

# "0-3,6" -> 0 1 2 3 6
expand_cpus() {
  local part lo hi c out=()
  local IFS=,
  for part in $1; do
    [[ "$part" =~ ^([0-9]+)(-([0-9]+))?$ ]] || die "bad CPU list: $1"
    lo="${BASH_REMATCH[1]}"
    hi="${BASH_REMATCH[3]:-$lo}"
    (( lo <= hi )) || die "bad CPU range: $part"
    for ((c = lo; c <= hi; ++c)); do out+=("$c"); done
  done
  printf '%s\n' "${out[@]}" | paste -sd' '
}

# The CPUs with cpuidle states: LIST expanded, or every CPU.
cstate_cpus() {
  if [[ -n "$1" ]]; then
    expand_cpus "$1"
  else
    local d out=()
    for d in "$SYSFS"/devices/system/cpu/cpu[0-9]*; do out+=("${d##*/cpu}"); done
    printf '%s\n' "${out[@]}" | sort -n | paste -sd' '
  fi
}

default_iface() { ip -o route show default 2>/dev/null | awk '{for (i = 1; i < NF; ++i) if ($i == "dev") print $(i + 1)}' | head -1; }

# PCI function of a netdev (virtio_net sits on a virtioN device below the PCI function).
pci_of() {
  local dev
  dev="$(readlink -f "/sys/class/net/$1/device")" || return 1
  [[ "$(basename "$dev")" == virtio* ]] && dev="$(dirname "$dev")"
  basename "$dev"
}

cmd_deps() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -q
  apt-get install -yq libssl3 iproute2 ethtool pciutils python3 numactl rsync util-linux \
    kmod linux-tools-common
  apt-get install -yq "linux-tools-$(uname -r)" "linux-modules-extra-$(uname -r)" ||
    echo "host-setup: linux-tools / linux-modules-extra for $(uname -r) not installed (optional)"
}

cmd_tune() {
  local pages=512 cstate_us="" cstate_cpus=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --hugepages) pages="${2:?--hugepages N}"; shift 2;;
      --cstate-max-latency) cstate_us="${2:?--cstate-max-latency US}"; shift 2;;
      --cstate-cpus) cstate_cpus="${2:?--cstate-cpus LIST}"; shift 2;;
      *) die "tune: unknown option $1";;
    esac
  done
  need_root
  echo "$pages" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
  mountpoint -q /dev/hugepages || { mkdir -p /dev/hugepages; mount -t hugetlbfs nodev /dev/hugepages; }
  echo "hugepages: $(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages) x 2 MiB on /dev/hugepages"
  if systemctl list-unit-files irqbalance.service >/dev/null 2>&1; then
    systemctl disable --now irqbalance >/dev/null 2>&1 || true
    echo "irqbalance: $(systemctl is-active irqbalance 2>/dev/null || true)"
  fi
  # Every device interrupt to CPU 0, away from the benchmark's pinned threads.
  for irq in /proc/irq/[0-9]*; do echo 0 > "$irq/smp_affinity_list" 2>/dev/null || true; done
  echo "interrupts: CPU 0"
  local gov=none
  for g in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
    [[ -f "$g" ]] || continue
    echo performance > "$g" 2>/dev/null && gov=performance
  done
  echo "governor: $gov (a VM usually has no cpufreq)"
  echo never > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
  sysctl -qw kernel.numa_balancing=0 2>/dev/null || true
  sysctl -qw vm.stat_interval=10 kernel.watchdog=0 2>/dev/null || true
  echo "THP: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo n/a)"
  if [[ -n "$cstate_us" ]]; then
    cmd_cstates limit "$cstate_us" ${cstate_cpus:+--cpus "$cstate_cpus"}
  fi
}

# cstates [show|limit <us>|restore] [--cpus LIST] [--dry-run]. Writes
# /sys/devices/system/cpu/cpuN/cpuidle/stateK/disable; a reboot or cstates restore undoes it.
# [engine] cpu_dma_latency_us asks the same of every CPU for as long as fastmm-live runs.
cmd_cstates() {
  local action=show max_us="" cpus="" dry=0
  if [[ $# -gt 0 && "$1" != --* ]]; then action="$1"; shift; fi
  if [[ "$action" == limit ]]; then
    max_us="${1:-}"
    [[ "$max_us" =~ ^[0-9]+$ ]] || die "cstates limit <max exit latency, us>"
    shift
  fi
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --cpus) cpus="${2:?--cpus LIST}"; shift 2;;
      --dry-run) dry=1; shift;;
      *) die "cstates: unknown option $1";;
    esac
  done
  case "$action" in show|limit|restore) ;; *) die "cstates show|limit <us>|restore";; esac
  [[ "$action" == show || $dry -eq 1 ]] || need_root "(--dry-run prints the plan)"
  local c st name lat dis want found=0
  for c in $(cstate_cpus "$cpus"); do
    local dir="$SYSFS/devices/system/cpu/cpu$c/cpuidle"
    [[ -d "$dir" ]] || continue
    for st in "$dir"/state[0-9]*; do
      [[ -d "$st" ]] || continue
      found=1
      name="$(cat "$st/name")"
      lat="$(cat "$st/latency")"
      dis="$(cat "$st/disable")"
      case "$action" in
        show) echo "cpu$c ${st##*/} $name exit_latency_us=$lat disabled=$dis"; continue;;
        limit) want=0; (( lat > max_us )) && want=1;;
        restore) want=0;;
      esac
      [[ "$dis" == "$want" ]] && continue
      if (( dry )); then
        echo "would set cpu$c ${st##*/} ($name, ${lat} us) disable=$want"
      elif echo "$want" > "$st/disable" 2>/dev/null; then
        echo "cpu$c ${st##*/} ($name, ${lat} us) disable=$want"
      else
        echo "host-setup: cpu$c ${st##*/}: cannot write disable" >&2
      fi
    done
  done
  (( found )) || echo "cstates: no cpuidle states under $SYSFS/devices/system/cpu (a VM often has none)"
}

# The subnets of <iface>'s IPv4 addresses (the kernel's link routes), e.g. 10.77.0.0/24.
subnets_of() { ip -o -4 route show dev "$1" proto kernel scope link 2>/dev/null | awk '{print $1}'; }

cmd_firewall() {
  local ifc="${1:?firewall <iface>}"
  [[ -d "/sys/class/net/$ifc" ]] || die "no interface $ifc"
  if ! command -v ufw >/dev/null 2>&1 || ! ufw status 2>/dev/null | grep -q '^Status: active'; then
    echo "firewall: ufw not active; nothing to do"
    return 0
  fi
  local nets
  nets="$(subnets_of "$ifc")"
  [[ -n "$nets" ]] || die "$ifc has no IPv4 subnet"
  for n in $nets; do
    ufw allow from "$n" comment "fastmm $ifc" >/dev/null
    echo "firewall: ufw allows everything from $n"
  done
}

cmd_xdp_prep() {
  local ifc="${1:?xdp-prep <iface>}"
  [[ -d "/sys/class/net/$ifc" ]] || die "no interface $ifc"
  ethtool -L "$ifc" combined 1 2>/dev/null || echo "host-setup: $ifc: queue count unchanged (ethtool -L)"
  ethtool -K "$ifc" gro off lro off 2>/dev/null || true
  ethtool -l "$ifc" 2>/dev/null | sed -n '/Current/,$p' || true
  ip -d link show "$ifc" | head -3
  cmd_firewall "$ifc"
}

cmd_dpdk_bind() {
  local ifc="${1:?dpdk-bind <iface>}"
  [[ -d "/sys/class/net/$ifc" ]] || die "no interface $ifc"
  [[ "$ifc" != "$(default_iface)" ]] || die "$ifc carries the default route (SSH); pick the VPC NIC"
  local pci drv mac
  pci="$(pci_of "$ifc")" || die "no PCI device behind $ifc"
  [[ -e "/sys/bus/pci/devices/$pci" ]] || die "$pci is not a PCI function"
  drv="$(basename "$(readlink -f "/sys/bus/pci/devices/$pci/driver")")"
  mac="$(cat "/sys/class/net/$ifc/address")"
  cmd_firewall "$ifc"
  mkdir -p "$STATE"
  {
    echo "IFACE=$ifc"
    echo "DRIVER=$drv"
    echo "MAC=$mac"
    echo "MTU=$(cat "/sys/class/net/$ifc/mtu")"
    echo "ADDRS=\"$(ip -o -4 addr show dev "$ifc" | awk '{print $4}' | tr '\n' ' ')\""
  } > "$STATE/dpdk-bind-$pci"
  if [[ -d /sys/module/vfio ]]; then
    echo Y > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
  else
    modprobe vfio enable_unsafe_noiommu_mode=1
  fi
  modprobe vfio-pci
  ip link set "$ifc" down
  echo vfio-pci > "/sys/bus/pci/devices/$pci/driver_override"
  echo "$pci" > "/sys/bus/pci/devices/$pci/driver/unbind"
  echo "$pci" > /sys/bus/pci/drivers_probe
  [[ "$(basename "$(readlink -f "/sys/bus/pci/devices/$pci/driver")")" == vfio-pci ]] ||
    die "$pci did not bind to vfio-pci (dmesg | tail)"
  echo "host-setup: $ifc ($pci, $drv, $mac) bound to vfio-pci (no-IOMMU)"
  echo "  bench:   scripts/bench-2host.sh --backend dpdk --dpdk-pci $pci ..."
  echo "  restore: scripts/host-setup.sh dpdk-unbind $pci"
}

cmd_dpdk_unbind() {
  local pci="${1:?dpdk-unbind <pci>}"
  local st="$STATE/dpdk-bind-$pci"
  [[ -f "$st" ]] || die "no saved state for $pci ($st)"
  # shellcheck disable=SC1090
  source "$st"
  echo > "/sys/bus/pci/devices/$pci/driver_override"
  [[ -e "/sys/bus/pci/devices/$pci/driver" ]] && echo "$pci" > "/sys/bus/pci/devices/$pci/driver/unbind"
  echo "$pci" > /sys/bus/pci/drivers_probe
  local ifc=""
  for _ in $(seq 50); do
    ifc="$(grep -lx "$MAC" /sys/class/net/*/address 2>/dev/null | head -1 | xargs -r dirname | xargs -r basename)"
    [[ -n "$ifc" ]] && break
    sleep 0.1
  done
  [[ -n "$ifc" ]] || die "$pci is back on $(basename "$(readlink -f "/sys/bus/pci/devices/$pci/driver")") but no interface has $MAC yet"
  ip link set "$ifc" mtu "$MTU" up
  for a in $ADDRS; do ip addr replace "$a" dev "$ifc"; done
  rm -f "$st"
  echo "host-setup: $pci back on $DRIVER as $ifc ($ADDRS)"
}

cmd_info() {
  echo "== $(hostname) $(uname -r) $(. /etc/os-release && echo "$PRETTY_NAME")"
  lscpu | grep -E '^(Model name|CPU\(s\)|Thread|Core|Socket|NUMA node\(s\)|Hypervisor)' || true
  echo "cmdline: $(cat /proc/cmdline)"
  echo "== interfaces (default route: $(default_iface))"
  ip -br addr
  for n in /sys/class/net/*; do
    local ifc; ifc="$(basename "$n")"
    [[ "$ifc" == lo || ! -e "$n/device" ]] && continue
    echo "-- $ifc: pci $(pci_of "$ifc") driver $(ethtool -i "$ifc" 2>/dev/null | awk '/^driver/{print $2}') mtu $(cat "$n/mtu")"
    ethtool -l "$ifc" 2>/dev/null | sed -n '/Current/,$p' | grep -E 'Combined|RX|TX' | tr -s ' \t' ' ' | paste -sd' ' || true
    ethtool -k "$ifc" 2>/dev/null | grep -E '^(generic-receive-offload|tcp-segmentation-offload|large-receive-offload):' | paste -sd' ' || true
    ip -d link show "$ifc" | grep -o 'prog/xdp.*' || true
  done
  echo "== PCI network functions"
  lspci -Dnnk 2>/dev/null | grep -A3 -Ei 'ethernet|network' | grep -E '^[0-9a-f]{4}:|driver in use' || true
  echo "== hugepages: $(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages) x 2 MiB, free $(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)"
  echo "== vfio: $(lsmod | awk '$1 ~ /^vfio/ {print $1}' | paste -sd' ') noiommu=$(cat /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || echo -) iommu_groups=$(ls /sys/kernel/iommu_groups 2>/dev/null | wc -l)"
  echo "== governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo none) irqbalance: $(systemctl is-active irqbalance 2>/dev/null || true)"
  ls "$STATE"/dpdk-bind-* 2>/dev/null | sed 's/^/== bound: /' || true
}

case "$1" in
  deps) need_root; cmd_deps;;
  tune) shift; cmd_tune "$@";;
  cstates) shift; cmd_cstates "$@";;
  firewall) shift; need_root; cmd_firewall "$@";;
  xdp-prep) shift; need_root; cmd_xdp_prep "$@";;
  dpdk-bind) shift; need_root; cmd_dpdk_bind "$@";;
  dpdk-unbind) shift; need_root; cmd_dpdk_unbind "$@";;
  info) cmd_info;;
  all) need_root; cmd_deps; cmd_tune; cmd_info;;
  *) usage >&2; exit 2;;
esac
