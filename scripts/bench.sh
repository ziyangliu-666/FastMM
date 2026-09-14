#!/usr/bin/env bash
# Run all micro-benchmarks pinned to one core and regenerate bench/README.md.
#   ./scripts/bench.sh [--preset release-native] [--cpu 2] [--min-time 0.5s] [--tag mytag]
set -euo pipefail
cd "$(dirname "$0")/.."
PRESET=release-native; CPU=2; MIN_TIME=0.5s; TAG=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) PRESET="$2"; shift 2;; --cpu) CPU="$2"; shift 2;; --min-time) MIN_TIME="$2"; shift 2;; --tag) TAG="$2"; shift 2;;
    *) echo "unknown arg $1"; exit 2;;
  esac
done
export CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-$HOME/.cache/CPM}"
[[ -d "build/$PRESET" ]] || cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j"$(nproc)" >/dev/null
OUT="bench/results/latest"; rm -rf "$OUT"; mkdir -p "$OUT"
STAMP="$(date -u +%Y%m%d-%H%M%S)"
for b in build/"$PRESET"/bin/bench/bench_*; do
  name="$(basename "$b")"
  echo "==> $name (cpu $CPU, min_time $MIN_TIME)"
  taskset -c "$CPU" "$b" --cpu="$CPU" --benchmark_min_time="$MIN_TIME" --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true --benchmark_out_format=json --benchmark_out="$OUT/$name.json" \
    --benchmark_counters_tabular=true 2>&1 | tail -n +1 | grep -E "_median|^-|Benchmark" || true
done
python3 tools/bench_table.py "$OUT"/*.json --template bench/README.tmpl.md --preset "$PRESET" --cpu "$CPU" > bench/README.md
[[ -n "$TAG" ]] && { mkdir -p bench/results; cp -r "$OUT" "bench/results/$TAG-$STAMP"; }
echo "wrote bench/README.md"
