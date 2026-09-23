#!/usr/bin/env bash
# Run all micro-benchmarks and regenerate bench/README.md.
#   ./scripts/bench.sh [--preset release-native] [--cpu 2] [--min-time 0.5s] [--tag mytag]
#                      [--repetitions 5] [--rounds 3] [--only bench_x]
# --only (repeatable) reruns just those executables and keeps the other results in bench/results/latest.
#
# --rounds runs the whole suite that many times, one after the other, and keeps every round
# (<name>.rN.json). tools/bench_table.py pools the repetitions of all of them, so the published
# spread covers the drift between rounds as well as the repetitions inside one. On a machine shared
# with other work that difference is the larger of the two.
#
# Each binary runs in two passes. The first is pinned to one core (taskset plus sched_setaffinity in
# the bench main) and skips the benchmarks the binary declares as needing more than one core
# (FASTMM_BENCH_NEEDS_CORES, bench/bench_pin.hpp); the second runs those unpinned, into
# <name>.multicore.rN.json. Pinning a benchmark that waits on a second thread measures the
# scheduler, not the code.
#
# Repetitions are reported raw (no --benchmark_report_aggregates_only): tools/bench_table.py wants
# every repetition so it can show the run-to-run spread instead of Google Benchmark's stddev of the
# repetition means.
set -euo pipefail
cd "$(dirname "$0")/.."
PRESET=release-native; CPU=2; MIN_TIME=0.5s; TAG=""; REPS=5; ROUNDS=1; ONLY=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) PRESET="$2"; shift 2;; --cpu) CPU="$2"; shift 2;; --min-time) MIN_TIME="$2"; shift 2;; --tag) TAG="$2"; shift 2;;
    --repetitions) REPS="$2"; shift 2;; --rounds) ROUNDS="$2"; shift 2;;
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
# Google Benchmark records the host name in the context; results carry no machine identity.
strip_host() {
  [[ -f "$1" ]] || return 0
  python3 -c 'import json, sys
p = sys.argv[1]; d = json.load(open(p)); d.get("context", {}).pop("host_name", None)
json.dump(d, open(p, "w"), indent=2); open(p, "a").write("\n")' "$1"
}
show() { grep -E "_median|^-|Benchmark|ERROR" || true; }
for b in "${BENCHES[@]}"; do rm -f "$OUT/$(basename "$b")".r*.json; done
for ((round = 1; round <= ROUNDS; round++)); do
  for b in "${BENCHES[@]}"; do
    name="$(basename "$b")"
    multicore="$("$b" --fastmm-multicore-filter)"
    echo "==> round $round/$ROUNDS $name (cpu $CPU, min_time $MIN_TIME, $REPS repetitions)"
    TZ=UTC taskset -c "$CPU" "$b" --cpu="$CPU" --benchmark_min_time="$MIN_TIME" --benchmark_repetitions="$REPS" \
      ${multicore:+--benchmark_filter="-($multicore)"} \
      --benchmark_out_format=json --benchmark_out="$OUT/$name.r$round.json" --benchmark_counters_tabular=true 2>&1 | show
    strip_host "$OUT/$name.r$round.json"
    if [[ -n "$multicore" ]]; then
      echo "==> round $round/$ROUNDS $name (unpinned: $multicore needs more than one core)"
      TZ=UTC "$b" --benchmark_min_time="$MIN_TIME" --benchmark_repetitions="$REPS" \
        --benchmark_filter="$multicore" \
        --benchmark_out_format=json --benchmark_out="$OUT/$name.multicore.r$round.json" --benchmark_counters_tabular=true 2>&1 | show
      strip_host "$OUT/$name.multicore.r$round.json"
    fi
  done
done
python3 tools/bench_table.py "$OUT"/*.json --template bench/README.tmpl.md --preset "$PRESET" --cpu "$CPU" > bench/README.md
[[ -n "$TAG" ]] && { mkdir -p bench/results; cp -r "$OUT" "bench/results/$TAG-$STAMP"; }
echo "wrote bench/README.md"
