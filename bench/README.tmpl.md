# Benchmarks

The results, the machine and the method are on [Benchmarks](../docs/explanation/benchmarks.md); every benchmark is in [full-results.md](full-results.md). `scripts/bench.sh` regenerates both and this file.

```bash
scripts/bench-e2e.sh --duration 30 --runs 3 --sim-cpu 0 --engine-cpu 1 --net-cpu 3 --out bench/results/e2e
./scripts/bench.sh --preset release-native --cpu 1 --rounds 3 --e2e bench/results/e2e
python3 tools/check_budgets.py bench/results/latest
python3 tools/bench_compare.py bench/results/baseline bench/results/latest
```

`check_budgets.py` fails when a benchmark exceeds its budget in `bench/ci_budget.toml` by more than 10 %. `scripts/build-pgo.sh [--bolt]` builds `release-native` with PGO, and BOLT, trained on these benchmarks; shipped builds do not use it.

For an ITCH change, also replay a real session: NASDAQ publishes full days of TotalView-ITCH 5.0 (`emi.nasdaq.com/ITCH`). `fastmm-bench-l3-itch` times every book message, `--from 09:30` times the open on books warmed by the morning, and `--check` compares every touch with a map book. `BM_ItchL2Bridge_Message_MultiSymbol` interleaves 1, 10 and 100 books, so a datagram publishes several of them.

```bash
build/release/bin/bench/fastmm-bench-l3-itch day.itch --top 100 --from 09:30 --check
```

For a change to the queue models, `fastmm-bench-queue-truth` scores `l2_queue` and `l3_queue` against the same file, where every order's outcome is known.

```bash
build/release/bin/bench/fastmm-bench-queue-truth day.itch --top 100
```
