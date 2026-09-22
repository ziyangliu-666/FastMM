#!/usr/bin/env bash
# Builds a minimal static DPDK into <prefix> for -DFASTMM_WITH_DPDK=ON when no system libdpdk is
# installed: EAL, mbuf, mempool, ethdev, the vdev bus (af_packet, tap, ring PMDs) and the PCI bus
# with a few common NIC PMDs (ENA, Intel, virtio, vmxnet3). meson, ninja and pyelftools go into a
# venv under <prefix>; nothing is installed system-wide and no root is needed.
#
#   scripts/build-dpdk.sh <prefix> [version]
#
# cmake/Dpdk.cmake runs it (FASTMM_DPDK_FETCH=ON) and points pkg-config at <prefix>/lib/pkgconfig.
set -euo pipefail
PREFIX="${1:?usage: build-dpdk.sh <prefix> [version]}"
VERSION="${2:-25.11.3}"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
WORK="$PREFIX/src"
mkdir -p "$WORK"
PY=/usr/bin/python3
[[ -x "$PY" ]] || PY="$(command -v python3)"
if [[ ! -x "$WORK/venv/bin/meson" ]]; then
  "$PY" -m venv "$WORK/venv"
  "$WORK/venv/bin/pip" install --quiet meson ninja pyelftools
fi
export PATH="$WORK/venv/bin:$PATH"
TARBALL="$WORK/dpdk-$VERSION.tar.xz"
[[ -f "$TARBALL" ]] || curl -fsSL -o "$TARBALL" "https://fast.dpdk.org/rel/dpdk-$VERSION.tar.xz"
SRC="$WORK/dpdk-$VERSION"
if [[ ! -d "$SRC" ]]; then
  mkdir -p "$SRC"
  tar -C "$SRC" --strip-components=1 -xJf "$TARBALL"
fi
cd "$SRC"
DRIVERS="bus/pci,bus/vdev,mempool/ring,net/af_packet,net/tap,net/ring,net/ena,net/ixgbe,net/i40e,net/ice,net/iavf,net/virtio,net/vmxnet3"
NUMA=()
[[ -f /usr/include/numa.h ]] || NUMA=(-Dmax_numa_nodes=1)  # no libnuma-dev: single-node build
if [[ ! -f build/build.ninja ]]; then
  rm -rf build
  meson setup build --prefix="$PREFIX" --libdir=lib --buildtype=release --default-library=static \
    -Dplatform=generic -Denable_apps= -Dtests=false -Denable_docs=false -Dexamples= \
    -Denable_drivers="$DRIVERS" -Dmax_lcores=128 "${NUMA[@]}"
fi
ninja -C build
meson install -C build --quiet
echo "build-dpdk: DPDK $VERSION in $PREFIX (pkg-config: $PREFIX/lib/pkgconfig)"
