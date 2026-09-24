#!/usr/bin/env bash
# Autobahn|Testsuite (RFC 6455 conformance) against the WebSocket client or server.
#
#   scripts/autobahn.sh client <fastmm_autobahn_client> <report dir>
#   scripts/autobahn.sh server <fastmm_autobahn_server> <report dir>
#
# client: `wstest -m fuzzingserver` listens and the testee connects to it. server: the testee
# listens and `wstest -m fuzzingclient` connects to it. Cases 1-7, 9 and 10; 12 and 13 test
# permessage-deflate, which is not negotiated. wstest is $FASTMM_WSTEST when set, else the pinned
# crossbario/autobahn-testsuite image; with neither the script exits 77 (ctest: skipped). The
# JSON and HTML reports land in <report dir>; tools/autobahn_check.py fails on any FAILED case.
set -euo pipefail
if [[ $# -ne 3 || ( $1 != client && $1 != server ) ]]; then
  echo "usage: $0 client|server <testee> <report dir>" >&2
  exit 2
fi
mode=$1; testee=$2; out=$3
root=$(cd "$(dirname "$0")/.." && pwd)
image=${FASTMM_AUTOBAHN_IMAGE:-crossbario/autobahn-testsuite:25.10.1@sha256:519915fb568b04c9383f70a1c405ae3ff44ab9e35835b085239c258b6fac3074}
cases='["1.*", "2.*", "3.*", "4.*", "5.*", "6.*", "7.*", "9.*", "10.*"]'

if [[ -n ${FASTMM_WSTEST:-} ]]; then
  runner=local
elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  runner=docker
else
  echo "autobahn: no working docker and FASTMM_WSTEST unset; skipped" >&2
  exit 77
fi

rm -rf "$out"
mkdir -p "$out"
out=$(cd "$out" && pwd)
dir=$out  # the report directory as wstest sees it
[[ $runner == docker ]] && dir=/reports
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
container=fastmm-autobahn-$mode-$$
pids=()

cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  [[ $runner == docker ]] && docker rm -f "$container" >/dev/null 2>&1
  return 0
}
trap cleanup EXIT

wstest() {
  if [[ $runner == docker ]]; then
    docker run --rm --name "$container" --network host -u "$(id -u):$(id -g)" -e HOME=/tmp \
      -v "$out:/reports" "$image" wstest "$@"
  else
    "$FASTMM_WSTEST" "$@"
  fi
}

wait_for_port() {
  for _ in $(seq 1 300); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then return 0; fi
    sleep 0.2
  done
  echo "autobahn: nothing listening on 127.0.0.1:$port" >&2
  return 1
}

if [[ $mode == client ]]; then
  cat > "$out/fuzzingserver.json" <<EOF
{"url": "ws://127.0.0.1:$port", "outdir": "$dir/clients", "cases": $cases,
 "exclude-cases": [], "exclude-agent-cases": {}}
EOF
  wstest -m fuzzingserver -s "$dir/fuzzingserver.json" --webport 0 > "$out/wstest.log" 2>&1 &
  pids+=($!)
  wait_for_port
  "$testee" 127.0.0.1 "$port" fastmm-ws-client
  report=$out/clients/index.json
else
  "$testee" "$port" > "$out/testee.log" 2>&1 &
  pids+=($!)
  wait_for_port
  cat > "$out/fuzzingclient.json" <<EOF
{"outdir": "$dir/servers",
 "servers": [{"agent": "fastmm-ws-server", "url": "ws://127.0.0.1:$port"}],
 "cases": $cases, "exclude-cases": [], "exclude-agent-cases": {}}
EOF
  wstest -m fuzzingclient -s "$dir/fuzzingclient.json" > "$out/wstest.log" 2>&1
  report=$out/servers/index.json
fi

python3 "$root/tools/autobahn_check.py" "$report"
