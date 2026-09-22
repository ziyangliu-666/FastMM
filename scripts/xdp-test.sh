#!/usr/bin/env bash
# AF_XDP tests that need root (ADR-0015): verifier load, BPF_PROG_TEST_RUN against crafted frames,
# receive over a veth pair between two temporary network namespaces in each XDP mode, and UserTcp
# on the XDP socket (TX ring, unicast line, own or shared address).
#
#   sudo scripts/xdp-test.sh [--build <dir>] [--no-build] [--e2e] [doctest options, e.g. -tc="veth*"]
#
# Builds the release preset as the invoking user first (skipped with --no-build or --build, e.g.
# on a VM with the binaries copied over), then runs fastmm_xdp_tests as root with
# FASTMM_XDP_REQUIRE=1, so a missing privilege fails instead of skipping. --e2e also runs
# scripts/bench-e2e.sh with --backend af_xdp over veth: multicast and unicast lines, OUCH over
# kernel TCP and over UserTcp (own address, and fastmm-live's address with a fixed port).
# Needs Linux 5.11+, iproute2 and ethtool.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $EUID -ne 0 ]]; then
  echo "run as root: sudo $0 $*" >&2
  exit 1
fi
BUILD=build/release; DO_BUILD=1; E2E=0; ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) BUILD="$2"; DO_BUILD=0; shift 2;;
    --no-build) DO_BUILD=0; shift;;
    --e2e) E2E=1; shift;;
    *) ARGS+=("$1"); shift;;
  esac
done
if [[ "$DO_BUILD" == 1 ]]; then
  if [[ -n "${SUDO_USER:-}" ]]; then
    sudo -u "$SUDO_USER" -H ./scripts/build.sh release --target fastmm_xdp_tests
  else
    ./scripts/build.sh release --target fastmm_xdp_tests
  fi
fi
FASTMM_XDP_REQUIRE=1 "$BUILD/tests/fastmm_xdp_tests" "${ARGS[@]}"
[[ "$E2E" == 1 ]] || exit 0
common=(--backend af_xdp --duration 5 --sim-cpu -1 --engine-cpu -1 --net-cpu -1 --build "$BUILD")
scripts/bench-e2e.sh "${common[@]}" --out "$BUILD/tmp/xdp-e2e-mcast"
scripts/bench-e2e.sh "${common[@]}" --md unicast --out "$BUILD/tmp/xdp-e2e-ucast"
scripts/bench-e2e.sh "${common[@]}" --md unicast --order-transport user_tcp \
  --out "$BUILD/tmp/xdp-e2e-user-tcp"
scripts/bench-e2e.sh "${common[@]}" --md unicast --order-transport user_tcp \
  --user-tcp-ip 10.211.0.2 --user-tcp-port 61001 --out "$BUILD/tmp/xdp-e2e-user-tcp-shared"
echo "xdp-test: all passed"
