# Benchmarks

Numbers live in `bench/README.md` (generated) and the README headline table.

Methodology:
- `scripts/bench.sh --preset release-native --cpu 2` pins each benchmark to one core (`taskset` plus
  `sched_setaffinity` in the bench main), runs 5 repetitions and reports the median.
- Latency-oriented benches also feed a `LogLinearHistogram` and export `p50`/`p99` counters, because
  Google Benchmark's mean hides tails.
- `allocs/op` is reported from the counting allocator and must be 0 on hot-path benches.
- `bench/ci_budget.toml` holds the p50 budgets; `tools/check_budgets.py` fails if a median exceeds
  budget + 25 %. `tools/bench_compare.py` diffs two result directories.

Caveats: WSL2 has no CPU isolation, so p99 tails include hypervisor noise; bare-metal numbers with
`isolcpus` will be lower. TLS + JSON dominate crypto tick-to-trade; the `BM_TickToOrder_Sim` bench
isolates the engine (T1 to T4) so it is comparable to binary-feed setups.
