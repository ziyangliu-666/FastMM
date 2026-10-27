# Benchmarks

<!-- BEGIN bench-setup -->
Intel Xeon Platinum 8488C, 2 cores and 4 threads, Ubuntu 24.04, Linux 7.0, gcc 13.3, preset `release-native`, pinned to CPU 1, 15 repetitions, 2026-09-29.
<!-- END bench-setup -->

Every benchmark, with its spread and throughput: [`bench/full-results.md`](../../bench/full-results.md). Commands: [`bench/README.md`](../../bench/README.md). A live session's latency: [How fast it is](how-fast.md).

## Hot path

Time per operation.

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

`fastmm-sim-itch` sends ITCH over MoldUDP64 multicast to `fastmm-live` in another network namespace, across a veth pair; `basic_mm` answers with OUCH 5.0 orders over TCP. Preset `release`, `configs/nasdaq-itch-sim.toml`, kernel receive, `spin_mode = "busy"`, simulator on CPU 0, engine on CPU 1, network thread on CPU 3 (the engine core's second hyperthread), three runs of 30 s.

<!-- BEGIN bench-e2e -->
| Interval | samples a run | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Wire to wire: market data sent to order received | 206 to 337 | 11.3 to 11.8 µs | 19.5 to 23.6 µs | 21.3 to 33.2 µs |
| Kernel receive to T0 (network thread) | 138k to 139k | 2.3 µs | 5.4 to 5.6 µs | 14.8 to 32.8 µs |
| T0 to T5: engine, tick to order in the ring | 129 to 191 | 2.6 to 2.7 µs | 12.8 to 14.3 µs | 14.4 to 18.3 µs |
| T0 to the order's `write` returning | 210 to 337 | 7.7 to 8.1 µs | 15.4 to 19.6 µs | 20.0 to 21.7 µs |

Range over 3 runs.
<!-- END bench-e2e -->

## What each number times

Tick to order (`BM_TickToOrder_Sim`) starts when a book delta is pushed into the engine's feed and ends when the last order it caused is serialised into the transport. In between: the L2 book, the strategy hook, the quote diff, the pre-trade risk checks, the OMS and the encoding of one cancel per side. In the simulator the transport copies each message into a 192-byte scheduler slot, the stand-in for the live engine's outbound ring. The venue, acks and the untimed settle loop between ticks are outside it. The simulator's outbound SHA-256, a determinism check the live engine does not compute, is off; `BM_TickToOrder_SimHash` turns it on.

Wire to wire is measured by the simulator with its own TSC: from right before the `sendmmsg` that sent an ITCH datagram to right after the read that returned the order it caused. Each order carries the MoldUDP64 sequence number that triggered it in its ClOrdID, so the simulator matches them. `fastmm-live` measures the other rows: the kernel receive timestamp to T0 on the network thread, T0 to T5 on the engine ([Status file](../reference/status-file.md#latency-intervals)), and T0 to the return of the `write` that carried the order. On veth that `write` runs the simulator's TCP receive path inside the system call, which is most of the gap between T5 and wire to wire.

## Method

`scripts/bench.sh` pins each benchmark to one core, except the few that need a second thread (`FASTMM_BENCH_NEEDS_CORES`, `bench/bench_pin.hpp`). The machine ran nothing else, without `isolcpus` or a fixed CPU frequency. Each benchmark ran five repetitions in each of three rounds, and `tools/bench_table.py` pools them.

Where a benchmark keeps a per-operation `rdtsc` histogram (buckets about 2 % wide), the tables give its percentiles; otherwise p50 is the median over repetitions of Google Benchmark's mean time. The codec benchmarks time 64 operations per `rdtsc` pair. Two processes of one binary differ by 2 to 5 %, so compare a change over several interleaved processes per side.

Across two hosts: [Two-host benchmark](../how-to/operations/two-host-benchmark.md).
