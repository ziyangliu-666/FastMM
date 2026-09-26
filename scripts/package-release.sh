#!/usr/bin/env bash
# Portable release build with the DPDK backend linked in statically, packed for copying to other
# hosts (x86-64-v2, no -march=native; needs glibc >= the build host's and libssl3, as on Ubuntu
# 24.04).
#
#   scripts/package-release.sh [--preset release-dpdk] [--out dist]
#
# Builds the preset (DPDK comes from pkg-config's libdpdk, or scripts/build-dpdk.sh builds it
# into build/<preset>/_deps/dpdk the first time) and writes dist/fastmm-<version>-x86_64.tar.gz
# with dist/fastmm-<version>-x86_64.tar.gz.sha256 beside it:
#   bin/      fastmm-live fastmm-sim-itch fastmm-top fastmm-replay
#   tests/    fastmm_xdp_tests fastmm_dpdk_tests (scripts/xdp-test.sh --build .)
#   configs/  scripts/  (bench-2host.sh, bench-e2e.sh, host-setup.sh, xdp-test.sh, bench-table.py)
#   deploy/   fastmm-live.service, prometheus/fastmm-alerts.yml
#   LICENSE   README.md
# On the target: tar xzf fastmm-*.tar.gz -C /opt && ln -sfn /opt/fastmm-<version> /opt/fastmm
set -euo pipefail
cd "$(dirname "$0")/.."
PRESET=release-dpdk; OUT=dist
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) PRESET="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
    *) echo "package-release: unknown argument $1" >&2; exit 2;;
  esac
done
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
[[ -f "build/$PRESET/CMakeCache.txt" ]] || cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j"$(nproc)" --target fastmm-live fastmm-sim-itch fastmm-top \
  fastmm-replay fastmm_xdp_tests
B="build/$PRESET"
grep -q '^FASTMM_NATIVE_ARCH:BOOL=ON' "$B/CMakeCache.txt" &&
  { echo "package-release: $B is built with -march=native: not portable" >&2; exit 1; }
if grep -q '^FASTMM_WITH_DPDK:BOOL=ON' "$B/CMakeCache.txt"; then
  cmake --build --preset "$PRESET" -j"$(nproc)" --target fastmm_dpdk_tests
fi
VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$B/CMakeCache.txt")"
NAME="fastmm-${VERSION:-dev}-x86_64"
STAGE="$OUT/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/tests" "$STAGE/scripts" "$STAGE/configs"
cp "$B"/bin/fastmm-live "$B"/bin/fastmm-sim-itch "$B"/bin/fastmm-top "$B"/bin/fastmm-replay "$STAGE/bin/"
cp "$B/tests/fastmm_xdp_tests" "$STAGE/tests/"
[[ -x "$B/tests/fastmm_dpdk_tests" ]] && cp "$B/tests/fastmm_dpdk_tests" "$STAGE/tests/"
strip --strip-debug "$STAGE"/bin/* "$STAGE"/tests/*
cp scripts/bench-2host.sh scripts/bench-e2e.sh scripts/bench-table.py scripts/host-setup.sh \
  scripts/xdp-test.sh "$STAGE/scripts/"
cp configs/nasdaq-itch-sim.toml configs/sim-itch.toml "$STAGE/configs/"
mkdir -p "$STAGE/deploy"
cp deploy/fastmm-live.service "$STAGE/deploy/"
mkdir -p "$STAGE/deploy/prometheus"
cp deploy/prometheus/fastmm-alerts.yml "$STAGE/deploy/prometheus/"
cp LICENSE "$STAGE/"
cat > "$STAGE/README.md" <<EOF
# FastMM ${VERSION:-dev} (x86_64)

Portable build of \`fastmm-live\`, \`fastmm-sim-itch\`, \`fastmm-top\` and \`fastmm-replay\`, compiled for
x86-64-v2. Needs glibc >= the build host's and libssl 3 (Ubuntu 24.04: \`apt install libssl3\`).

## Install

\`\`\`bash
tar xzf fastmm-${VERSION:-dev}-x86_64.tar.gz -C /opt
ln -sfn /opt/fastmm-${VERSION:-dev}-x86_64 /opt/fastmm
/opt/fastmm/bin/fastmm-live --version
\`\`\`

## Run

The Nasdaq-style simulator and the engine, in a network namespace whose \`lo\` carries multicast:

\`\`\`bash
cd /opt/fastmm
unshare -Urn sh -c 'ip link set lo up multicast on && ip route add 224.0.0.0/4 dev lo &&
  (bin/fastmm-sim-itch --config configs/sim-itch.toml &) && sleep 1 &&
  bin/fastmm-live --config configs/nasdaq-itch-sim.toml --duration 30s'
\`\`\`

\`bin/fastmm-top --name nasdaq-itch-sim\` watches a running session from the same namespace.

\`deploy/fastmm-live.service\` is the systemd unit; copy it to \`/etc/systemd/system/\` and edit the
paths. \`deploy/prometheus/fastmm-alerts.yml\` holds Prometheus alerting rules for the metrics
\`fastmm-top --metrics\` serves. Read the deployment guide before pointing this at a venue:
<https://ziy.bio/FastMM/how-to/operations/deploy/> and
<https://ziy.bio/FastMM/how-to/operations/go-live-checklist/>.

Licence: MIT (\`LICENSE\`). Source: <https://github.com/ziyangliu-666/FastMM>.
EOF
tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
(cd "$OUT" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
echo "package-release: $OUT/$NAME.tar.gz ($(du -h "$OUT/$NAME.tar.gz" | cut -f1)); DPDK: $(grep -c '^FASTMM_WITH_DPDK:BOOL=ON' "$B/CMakeCache.txt" | sed 's/1/linked/;s/0/not built/')"
