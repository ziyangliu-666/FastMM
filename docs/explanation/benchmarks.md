# Benchmarks

Every number here was measured on one machine: an AWS c7i.xlarge (Intel Xeon Platinum 8488C, 2 cores and 4 threads), Ubuntu 24.04, Linux 7.0, gcc 13.3. Every benchmark, with its spread and throughput: [`bench/full-results.md`](../../bench/full-results.md).

## Hot path

Time per operation, preset `release-native`, pinned to one core.

<!-- BEGIN bench-hot -->
| Stage | Operation | p50 | p99 | p99.9 | Benchmark |
|---|---|---:|---:|---:|---|
| Market data | Binance book ticker, JSON decode | 116.6 ns |  |  | `BM_Json_BinanceBookTicker` |
|  | Nasdaq ITCH message into the L2 book | 81.9 ns | 90.1 ns |  | `BM_ItchL2Bridge_Message` |
|  | L2 book, apply a 20-level delta | 353.3 ns |  |  | `BM_L2_ApplyDelta/20` |
| Engine | Tick to order, simulator | 159.0 ns | 199.0 ns | 319.0 ns | `BM_TickToOrder_Sim` |
|  | Strategy: two quotes (`basic_mm`) | 11.2 ns |  |  | `BM_BasicMM_ComputeQuotes` |
|  | Quote diff against resting orders | 49.4 ns |  |  | `BM_QuoteManager_Reconcile_6Orders1Change` |
|  | Pre-trade risk check | 6.7 ns |  |  | `BM_Risk_CheckNew_Pass` |
|  | OMS: submit an order | 30.9 ns |  |  | `BM_Oms_Submit` |
|  | Journal: record a 128-byte event | 5.7 ns |  |  | `BM_Journal_Record/128` |
| Orders | Binance `order.place`, HMAC-signed | 714.6 ns |  |  | `BM_Encode_BinanceOrderPlace` |
|  | Binance `order.place`, Ed25519 session | 268.2 ns |  |  | `BM_Encode_BinanceOrderPlace_Session` |
|  | OUCH 5.0 Enter Order | 17.4 ns | 19.5 ns |  | `BM_Ouch50_EncodeNewIds` |
<!-- END bench-hot -->

## End to end over veth

`fastmm-sim-itch` and `fastmm-live` in two network namespaces joined by a veth pair, `basic_mm` on `configs/nasdaq-itch-sim.toml`, preset `release`, busy polling, simulator on CPU 0, engine on CPU 1 and network thread on CPU 3, three runs of 30 s on 2026-09-29. Each cell is the range over the runs; the engine's own intervals are in [`bench/README.md`](../../bench/README.md#end-to-end-over-veth).

| Interval | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| Wire to wire | 11.3 to 11.8 µs | 19.5 to 23.6 µs | 21.3 to 33.2 µs |
| Kernel receive to T0 | 2.3 µs | 5.4 to 5.6 µs | 14.8 to 32.8 µs |
| T0 to T5: engine, tick to order | 2.6 to 2.7 µs | 12.8 to 14.3 µs | 14.4 to 18.3 µs |

## What each number times

Tick to order (`BM_TickToOrder_Sim`) starts when a book delta is pushed into the engine's feed and ends when the last order it caused is serialised into the transport. In between: the L2 book, the strategy hook, the quote diff, the pre-trade risk checks, the OMS and the encoding of one cancel per side. In the simulator the transport copies each message into a 192-byte scheduler slot, the stand-in for the live engine's outbound ring. The venue, acks and the untimed settle loop between ticks are outside it. The simulator's outbound SHA-256, a determinism check the live engine does not compute, is off; `BM_TickToOrder_SimHash` turns it on.

Wire to wire is measured by the simulator with its own TSC: from right before the `sendmmsg` that sent an ITCH datagram to right after the read that returned the order it caused. Each order carries the MoldUDP64 sequence number that triggered it in its ClOrdID, so the simulator matches them. `fastmm-live` measures the other rows: the kernel receive timestamp to T0 on the network thread, T0 to T5 on the engine ([Status file](../reference/status-file.md#latency-intervals)), and T0 to the return of the `write` that carried the order. On veth that `write` runs the simulator's TCP receive path inside the system call, which is most of the gap between T5 and wire to wire.

## Method

- `scripts/bench.sh --preset release-native --cpu 1` runs each benchmark binary twice. The first pass is pinned to one core (`taskset` and `sched_setaffinity`) and skips the benchmarks that declare they need more cores (`FASTMM_BENCH_NEEDS_CORES`, `bench/bench_pin.hpp`); the second runs those unpinned. A benchmark that finds fewer cores in its affinity mask than it declared fails instead of reporting a number.
- Five repetitions per benchmark, reported raw; `--rounds N` repeats the suite and `tools/bench_table.py` pools every repetition of every round.
- Where a benchmark times each operation with `rdtsc` into a `LogLinearHistogram` (tick to order, the reactor, UDP, the ITCH and OUCH codecs), the tables give its p50 and p99, and p99.9 for tick to order. The buckets are about 2 % wide. Elsewhere Google Benchmark reports one mean per repetition, and the tables give the median of those means.
- Tick to order uses manual timing (`UseManualTime`): the reported time is the `rdtsc` interval around the tick. Between timed ticks the rig delivers acks until both quotes rest at the venue, so each timed tick sends orders; the benchmark fails when fewer than half do (`order_events_pct`).
- The codec benchmarks time 64 operations between two `rdtsc` readings and report the median per-operation time; the `*_NewIds` variants change the ClOrdID on every call so the compiler cannot hoist the work.
- Loops that measure latency chain each result into the next iteration's input (`BM_Risk_CheckNew_Pass`, `BM_Fixed_Mul`, `BM_Fixed_RoundToTick`, `BM_L2_UpdateNearTop`). `BM_Risk_CheckNew_Killed`, `BM_Fixed_FromDecimal`, `BM_Fixed_ToDecimal` and `BM_L2_InsertEraseTop` measure throughput and say so in their source.
- `FASTMM_ALIGN_CODE` (default on, gcc) aligns functions to 64 bytes and loops and jump targets to 32 bytes on the FastMM targets, so an unrelated edit does not move the tick by changing code layout. Two processes of one binary still differ by 2 to 5 %: compare a change over several interleaved processes per side.
- The instance ran nothing else, without `isolcpus` or a fixed CPU frequency. `bench/full-results.md` gives each row's range over all repetitions next to its median.
- `bench/ci_budget.toml` holds time budgets and counter floors; `python3 tools/check_budgets.py bench/results/latest` exits 1 when a benchmark's fastest repetition exceeds its budget by more than 10 %, reported an error, or fell below a counter floor. CI runs a subset on a hosted runner with `--slack 2`. `tools/bench_compare.py` diffs two result directories.
- `scripts/build-pgo.sh [--bolt]` builds `release-native` with PGO (and BOLT) trained on the hot-path benchmarks, a backtest and `scripts/bench-e2e.sh`. Shipped builds do not use it.

Across two hosts: [Two-host benchmark](../how-to/operations/two-host-benchmark.md).
