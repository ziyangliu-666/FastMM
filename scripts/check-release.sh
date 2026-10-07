#!/usr/bin/env bash
# Runs a release tarball (scripts/package-release.sh) on other distributions: in a container of
# each image, unpacks it and runs `fastmm-live --version`, `fastmm-sim-exchange --help` and a 5 s
# session of fastmm-live against fastmm-sim-exchange (configs/sim.toml, configs/sim-local.toml).
# The release workflow runs it before publishing.
#
#   scripts/check-release.sh dist/fastmm-<version>-x86_64.tar.gz [image ...]
#
# Default images: ubuntu:22.04 ubuntu:24.04 debian:12 rockylinux:9 amazonlinux:2023. The tarball
# goes in with `docker cp`, so the images need no tar; DOCKER=docker.exe and similar work.
set -euo pipefail
TARBALL="${1:?usage: check-release.sh <fastmm-*.tar.gz> [image ...]}"
shift
IMAGES=("$@")
[[ ${#IMAGES[@]} -gt 0 ]] || IMAGES=(ubuntu:22.04 ubuntu:24.04 debian:12 rockylinux:9 amazonlinux:2023)
DOCKER="${DOCKER:-docker}"
DIR="$(basename "$TARBALL" .tar.gz)"

# shellcheck disable=SC2016  # expanded in the container
TEST='set -e
cd /opt/'"$DIR"'
bin/fastmm-live --version
bin/fastmm-sim-exchange --help > /dev/null
bin/fastmm-sim-exchange --config configs/sim.toml --duration 30s > sim.log 2>&1 &
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q REST sim.log && break; sleep 0.5; done
FASTMM_SIM_API_KEY=sim-key FASTMM_SIM_API_SECRET=sim-secret \
  bin/fastmm-live --config configs/sim-local.toml --duration 5s --no-status > live.log 2>&1 ||
  { tail -n 30 sim.log live.log; exit 1; }
kill $!
orders=$(sed -n "s/.*fastmm-live: events=[0-9]* book_updates=[0-9]* orders=\([0-9]*\).*/\1/p" live.log)
grep -q "shutdown took .* (cancel_all ok)" live.log && [ "${orders:-0}" -gt 0 ] ||
  { tail -n 30 live.log; exit 1; }
echo "session: $orders orders, cancel_all ok"'

fail=0
for image in "${IMAGES[@]}"; do
  name="fastmm-check-$$"
  "$DOCKER" create --name "$name" "$image" /bin/sh -c "$TEST" > /dev/null
  gzip -dc "$TARBALL" | "$DOCKER" cp - "$name:/opt"
  start=$SECONDS
  if out="$("$DOCKER" start -a "$name" 2>&1)"; then
    echo "check-release: $image ok ($((SECONDS - start)) s): $(head -n 1 <<<"$out"); $(tail -n 1 <<<"$out")"
  else
    echo "check-release: $image FAILED" >&2
    echo "$out" >&2
    fail=1
  fi
  "$DOCKER" rm -f "$name" > /dev/null
done
exit "$fail"
