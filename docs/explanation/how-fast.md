# How fast it is

One `fastmm-live` session on Binance: five accounts, 13 hours, a 4 vCPU VM in the exchange's region, every thread pinned to its own core. The engine alone: [Benchmarks](benchmarks.md).

## Market data to order

From the market-data packet read (T0) to the order handed to the network thread (T5), status after 11 hours ([latency intervals](../reference/status-file.md#latency-intervals)):

| Stage | Count | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Decode (T0 to T1) | 35,702,701 | 0.67 µs | 4.4 µs | 11.3 µs |
| Book apply (T1 to T2) | 35,702,701 | 0.83 µs | 13.8 µs | 45.1 µs |
| Strategy (T2 to T3) | 35,702,701 | 1.4 µs | 3.2 µs | 12.8 µs |
| Serialize (T3 to T4) | 474,145 | 2.3 µs | 51.2 µs | 69.6 µs |
| Send (T4 to T5) | 526,782 | 2.7 µs | 12.8 µs | 23.6 µs |
| **Tick to trade (T0 to T5)** | 373,107 | **11.3 µs** | **69.6 µs** | **110.6 µs** |

## On the wire

From T0 to the signed, TLS-encrypted order written to the socket, per account, whole session:

| Account | Orders | p50 | p99 |
|---|---:|---:|---:|
| 1 | 113,745 | 29.0 µs | 81.9 µs |
| 2 | 114,812 | 41.0 µs | 122.9 µs |
| 3 | 114,647 | 42.7 µs | 122.9 µs |
| 4 | 114,509 | 42.7 µs | 122.9 µs |
| 5 | 114,853 | 44.4 µs | 129.7 µs |

Encoding and signing: about 2 µs at p50.

## Load

| | |
|---|---|
| Market-data messages | 134 million, about 2,900 a second; 0 resyncs, 0 dropped |
| Orders | 429,754 placed, 428,679 cancelled |
| Rate-limit refusals | 0 |
| Reconnects | 0 |
