# Benchmarks

<!-- BEGIN bench-setup -->
Intel Xeon Platinum 8488C, 2 cores and 4 threads, Ubuntu 24.04.5 LTS, Linux 7.0.0, gcc 13.3, preset `release-native`, pinned to CPU 1, 15 repetitions, 2026-10-27.
<!-- END bench-setup -->

Every benchmark, with its spread and throughput: [`bench/full-results.md`](../../bench/full-results.md). Commands: [`bench/README.md`](../../bench/README.md). A live session's latency: [How fast it is](how-fast.md).

## Hot path

Time per operation.

<!-- BEGIN bench-hot -->
| Stage | Operation | p50 | p99 | p99.9 | Benchmark |
|---|---|---:|---:|---:|---|
| Market data | Binance book ticker, JSON decode | 121.7 ns |  |  | `BM_Json_BinanceBookTicker` |
|  | Nasdaq ITCH message into the L2 book | 73.7 ns | 98.3 ns |  | `BM_ItchL2Bridge_Message` |
|  | L2 book, apply a 20-level delta | 367.1 ns |  |  | `BM_L2_ApplyDelta/20` |
| Engine | Tick to order, simulator | 143.0 ns | 191.0 ns | 319.0 ns | `BM_TickToOrder_Sim` |
|  | Strategy: two quotes (`basic_mm`) | 11.7 ns |  |  | `BM_BasicMM_ComputeQuotes` |
|  | Quote diff against resting orders | 50.9 ns |  |  | `BM_QuoteManager_Reconcile_6Orders1Change` |
|  | Pre-trade risk check | 7.1 ns |  |  | `BM_Risk_CheckNew_Pass` |
|  | OMS: submit an order | 30.4 ns |  |  | `BM_Oms_Submit` |
|  | Journal: record a 128-byte event | 5.9 ns |  |  | `BM_Journal_Record/128` |
| Orders | Binance `order.place`, HMAC-signed | 749.3 ns |  |  | `BM_Encode_BinanceOrderPlace` |
|  | Binance `order.place`, Ed25519 session | 282.4 ns |  |  | `BM_Encode_BinanceOrderPlace_Session` |
|  | OUCH 5.0 Enter Order | 18.4 ns | 21.5 ns |  | `BM_Ouch50_EncodeNewIds` |
<!-- END bench-hot -->

## End to end over veth

`fastmm-sim-itch` sends ITCH over multicast to `fastmm-live` in another network namespace, across a veth pair; `basic_mm` answers with OUCH 5.0 orders over TCP. Engine, network thread and simulator each have their own CPU; three runs of 30 s, 2026-09-29.

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

| Number | Starts | Ends |
|---|---|---|
| Tick to order | a book delta pushed into the engine's feed | the last order it caused serialised into the transport: book, strategy, quote diff, risk, OMS and encoding of one cancel per side |
| Wire to wire | the simulator sends an ITCH datagram | the simulator reads the order it caused |
| Kernel receive to T0 | the kernel's receive timestamp | the network thread takes the frame |
| T0 to T5 | the network thread takes the frame | the order is in the engine's outbound ring ([Status file](../reference/status-file.md#latency-intervals)) |
| T0 to `write` returning | the network thread takes the frame | the system call that sent the order returns; on veth it includes the simulator's TCP receive |

Each benchmark runs pinned to one core on an otherwise idle machine; methods and commands: [`bench/README.md`](../../bench/README.md#method-of-the-micro-benchmarks). Two processes of one binary differ by 2 to 5 %. Across two hosts: [Two-host benchmark](../how-to/operations/two-host-benchmark.md).
