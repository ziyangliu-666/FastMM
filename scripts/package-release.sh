#!/usr/bin/env bash
# Portable release build, packed for copying to other hosts: x86-64-v2 (no -march=native), OpenSSL
# (scripts/wheels/build-openssl.sh), libstdc++ and libgcc linked statically, so the binaries need
# only glibc. --container builds in manylinux_2_28 (glibc 2.28: Ubuntu 20.04, Debian 10, RHEL 8,
# Amazon Linux 2023 and newer), as the release workflow does; without it the tarball needs the
# build host's glibc or newer.
#
#   scripts/package-release.sh [--preset release-dpdk] [--out dist] [--container]
#
# Builds the preset into build/package-<preset> (DPDK comes from pkg-config's libdpdk, or
# scripts/build-dpdk.sh builds it there the first time) and writes
# dist/fastmm-<version>-x86_64.tar.gz with dist/fastmm-<version>-x86_64.tar.gz.sha256 beside it:
#   bin/      fastmm-live fastmm-gateway fastmm-ctl fastmm-top fastmm-pnl fastmm-replay
#             fastmm-sim-exchange fastmm-sim-itch
#   tests/    fastmm_xdp_tests fastmm_dpdk_tests (scripts/xdp-test.sh --build .),
#             fixtures/tls/ (the simulator's self-signed certificate)
#   configs/  scripts/  (bench-2host.sh, bench-e2e.sh, host-setup.sh, xdp-test.sh, bench-table.py)
#   deploy/   fastmm-live.service, fastmm-gateway.service, fastmm-live@.service,
#             prometheus/fastmm-alerts.yml
#   LICENSE   README.md
# On the target: tar xzf fastmm-*.tar.gz -C /opt && ln -sfn /opt/fastmm-<version> /opt/fastmm
#
# --container runs this script in FASTMM_RELEASE_IMAGE (quay.io/pypa/manylinux_2_28_x86_64) with
# `docker` (DOCKER=docker.exe and similar work: nothing is bind-mounted). The source goes in as a
# tar of the checkout's tracked and untracked, not ignored, files; the build directory, OpenSSL and
# the CPM cache stay in the docker volume fastmm-release-cache between runs.
# scripts/check-release.sh runs the result on other distributions.
set -euo pipefail
cd "$(dirname "$0")/.."
PRESET=release-dpdk; OUT=dist; CONTAINER=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) PRESET="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --container) CONTAINER=1; shift;;
    -h|--help) sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "package-release: unknown argument $1" >&2; exit 2;;
  esac
done

if [[ $CONTAINER == 1 ]]; then
  DOCKER="${DOCKER:-docker}"
  IMAGE="${FASTMM_RELEASE_IMAGE:-quay.io/pypa/manylinux_2_28_x86_64}"
  name="fastmm-package-$$"
  trap '"$DOCKER" rm -f "$name" >/dev/null 2>&1 || true' EXIT
  # gcc-toolset (manylinux_2_28's default compiler); build/ is a link into the cache volume.
  "$DOCKER" create -i --name "$name" -v fastmm-release-cache:/cache -w /src \
    -e CPM_SOURCE_CACHE=/cache/cpm -e OPENSSL_ROOT_DIR=/cache/openssl "$IMAGE" bash -euc '
      tar -x -C /src
      command -v ninja >/dev/null || pipx install -q ninja >/dev/null
      mkdir -p /cache/build && ln -sfn /cache/build build
      scripts/package-release.sh --preset "$0" --out /out' "$PRESET" >/dev/null
  git ls-files -z -co --exclude-standard | tar -c --null -T - | "$DOCKER" start -ai "$name"
  mkdir -p "$OUT"
  "$DOCKER" cp "$name:/out" - | tar -x -C "$OUT" --strip-components=1
  exit 0
fi

export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
B="build/package-$PRESET"
OPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR:-$PWD/build/_openssl}"
scripts/wheels/build-openssl.sh "$OPENSSL_ROOT_DIR"
[[ -f "$B/CMakeCache.txt" ]] || cmake --preset "$PRESET" -B "$B" \
  -DFASTMM_OPENSSL_STATIC=ON -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR" \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
TOOLS=(fastmm-live fastmm-gateway fastmm-ctl fastmm-top fastmm-pnl fastmm-replay
  fastmm-sim-exchange fastmm-sim-itch)
cmake --build "$B" -j"$(nproc)" --target "${TOOLS[@]}" fastmm_xdp_tests
grep -q '^FASTMM_NATIVE_ARCH:BOOL=ON' "$B/CMakeCache.txt" &&
  { echo "package-release: $B is built with -march=native: not portable" >&2; exit 1; }
if grep -q '^FASTMM_WITH_DPDK:BOOL=ON' "$B/CMakeCache.txt"; then
  cmake --build "$B" -j"$(nproc)" --target fastmm_dpdk_tests
fi
VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$B/CMakeCache.txt")"
NAME="fastmm-${VERSION:-dev}-x86_64"
STAGE="$OUT/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/tests/fixtures/tls" "$STAGE/scripts" "$STAGE/configs"
for tool in "${TOOLS[@]}"; do
  cp "$B/bin/$tool" "$STAGE/bin/"
done
cp "$B/tests/fastmm_xdp_tests" "$STAGE/tests/"
[[ -x "$B/tests/fastmm_dpdk_tests" ]] && cp "$B/tests/fastmm_dpdk_tests" "$STAGE/tests/"
strip --strip-debug "$STAGE"/bin/* "$STAGE"/tests/fastmm_*
# Only glibc, the loader and the vDSO: no libssl, libcrypto, libstdc++ or libgcc_s.
for f in "$STAGE"/bin/* "$STAGE"/tests/fastmm_*; do
  extra="$(readelf -d "$f" | awk '/NEEDED/ {print $NF}' | tr -d '[]' |
    grep -vxE 'lib(c|m|pthread|dl|rt)\.so\.[0-9]+|ld-linux-x86-64\.so\.2' || true)"
  [[ -z "$extra" ]] || { echo "package-release: $f needs $extra" >&2; exit 1; }
done
GLIBC="$(objdump -T "$STAGE"/bin/* "$STAGE"/tests/fastmm_* | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
cp scripts/bench-2host.sh scripts/bench-e2e.sh scripts/bench-table.py scripts/host-setup.sh \
  scripts/xdp-test.sh "$STAGE/scripts/"
cp configs/nasdaq-itch-sim.toml configs/sim-itch.toml configs/sim.toml configs/sim-local.toml \
  configs/sim-local-tls.toml "$STAGE/configs/"
cp tests/fixtures/tls/cert.pem tests/fixtures/tls/key.pem "$STAGE/tests/fixtures/tls/"
mkdir -p "$STAGE/deploy/prometheus"
cp deploy/fastmm-live.service deploy/fastmm-gateway.service deploy/fastmm-live@.service "$STAGE/deploy/"
cp deploy/prometheus/fastmm-alerts.yml "$STAGE/deploy/prometheus/"
cp LICENSE "$STAGE/"
sed "s/@VERSION@/${VERSION:-dev}/g; s/@GLIBC@/${GLIBC#GLIBC_}/g" scripts/release-readme.md > "$STAGE/README.md"
tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
(cd "$OUT" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
echo "package-release: $OUT/$NAME.tar.gz ($(du -h "$OUT/$NAME.tar.gz" | cut -f1)); needs $GLIBC; DPDK: $(grep -c '^FASTMM_WITH_DPDK:BOOL=ON' "$B/CMakeCache.txt" | sed 's/1/linked/;s/0/not built/')"
