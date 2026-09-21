#!/usr/bin/env bash
# AF_XDP tests that need root (ADR-0015): verifier load, BPF_PROG_TEST_RUN against crafted frames,
# and receive over a veth pair between two temporary network namespaces in each XDP mode.
#
#   sudo scripts/xdp-test.sh [doctest options, e.g. -tc="veth*"]
#
# Builds the release preset as the invoking user first, then runs build/release's
# fastmm_xdp_tests as root with FASTMM_XDP_REQUIRE=1, so a missing privilege fails instead of
# skipping. Needs Linux 5.11+ and iproute2.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $EUID -ne 0 ]]; then
  echo "run as root: sudo $0 $*" >&2
  exit 1
fi
if [[ -n "${SUDO_USER:-}" ]]; then
  sudo -u "$SUDO_USER" -H ./scripts/build.sh release --target fastmm_xdp_tests
else
  ./scripts/build.sh release --target fastmm_xdp_tests
fi
FASTMM_XDP_REQUIRE=1 exec build/release/tests/fastmm_xdp_tests "$@"
