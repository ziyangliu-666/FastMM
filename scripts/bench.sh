#!/usr/bin/env bash
# Run all micro-benchmarks pinned to one core and regenerate bench/README.md.
#   ./scripts/bench.sh [--preset release-native] [--cpu 2] [--min-time 0.5s] [--tag mytag] [--only bench_x]
# --only (repeatable) reruns just those executables and keeps the other results in bench/results/latest.
set -euo pipefail
cd "$(dirname "$0")/.."
PRESET=release-native; CPU=2; MIN_TIME=0.5s; TAG=""; ONLY=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) PRESET="$2"; shift 2;; --cpu) CPU="$2"; shift 2;; --min-time) MIN_TIME="$2"; shift 2;; --tag) TAG="$2"; shift 2;;
    --only) ONLY+=("$2"); shift 2;;
    *) echo "unknown arg $1"; exit 2;;
  esac
done
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
[[ -d "build/$PRESET" ]] || cmake --preset "$PRESET"
OUT="bench/results/latest"
if [[ ${#ONLY[@]} -eq 0 ]]; then
  cmake --build --preset "$PRESET" -j"$(nproc)" >/dev/null
  rm -rf "$OUT"; mkdir -p "$OUT"
  BENCHES=(build/"$PRESET"/bin/bench/bench_*)
else
  cmake --build --preset "$PRESET" -j"$(nproc)" --target "${ONLY[@]}" >/dev/null
  mkdir -p "$OUT"
  BENCHES=(); for n in "${ONLY[@]}"; do BENCHES+=("build/$PRESET/bin/bench/$n"); done
fi
STAMP="$(date -u +%Y%m%d-%H%M%S)"
for b in "${BENCHES[@]}"; do
  name="$(basename "$b")"
  echo "==> $name (cpu $CPU, min_time $MIN_TIME)"
  TZ=UTC taskset -c "$CPU" "$b" --cpu="$CPU" --benchmark_min_time="$MIN_TIME" --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true --benchmark_out_format=json --benchmark_out="$OUT/$name.json" \
    --benchmark_counters_tabular=true 2>&1 | tail -n +1 | grep -E "_median|^-|Benchmark|ERROR" || true
  # Google Benchmark records the host name in the context; results carry no machine identity.
  [[ -f "$OUT/$name.json" ]] && python3 -c 'import json, sys
p = sys.argv[1]; d = json.load(open(p)); d.get("context", {}).pop("host_name", None)
json.dump(d, open(p, "w"), indent=2); open(p, "a").write("\n")' "$OUT/$name.json"
done
python3 tools/bench_table.py "$OUT"/*.json --template bench/README.tmpl.md --preset "$PRESET" --cpu "$CPU" > bench/README.md
[[ -n "$TAG" ]] && { mkdir -p bench/results; cp -r "$OUT" "bench/results/$TAG-$STAMP"; }
echo "wrote bench/README.md"
