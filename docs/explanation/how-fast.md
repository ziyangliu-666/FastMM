# How fast it is

<video class="fastmm-video" controls preload="none" poster="../../assets/intro-poster.jpg" src="https://github.com/ziyangliu-666/FastMM/releases/download/v0.5.1/fastmm-intro.mp4"></video>

One `fastmm-live` session trading on Binance for 17 hours 44 minutes, through a full US trading day: five accounts on one 4 vCPU VM in the exchange's region, every thread pinned to its own core. The engine alone: [Benchmarks](benchmarks.md).

## Market data to order

From the market-data packet read (T0) to the order handed to the network thread (T5) ([latency intervals](../reference/status-file.md#latency-intervals)):

| Stage | Count | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Decode (T0 to T1) | 123,649,985 | 0.54 µs | 3.2 µs | 11.3 µs |
| Book apply (T1 to T2) | 123,649,985 | 0.83 µs | 14.3 µs | 45.1 µs |
| Strategy (T2 to T3) | 123,649,985 | 1.3 µs | 3.1 µs | 12.3 µs |
| Serialize (T3 to T4) | 965,644 | 1.5 µs | 51.2 µs | 65.5 µs |
| Send (T4 to T5) | 1,228,909 | 2.3 µs | 12.3 µs | 24.6 µs |
| **Tick to trade (T0 to T5)** | 1,048,711 | **8.7 µs** | **65.5 µs** | **102.4 µs** |

## On the wire

From T0 to the signed, TLS-encrypted order written to the socket, per account:

| Account | Orders | p50 | p99 |
|---|---:|---:|---:|
| 1 | 241,261 | 25.6 µs | 81.9 µs |
| 2 | 240,582 | 37.5 µs | 136.5 µs |
| 3 | 241,127 | 39.3 µs | 136.5 µs |
| 4 | 242,098 | 39.3 µs | 136.5 µs |
| 5 | 240,909 | 39.3 µs | 136.5 µs |

## Load

| | |
|---|---|
| Market-data messages | 418 million, about 6,500 a second, 0 dropped |
| Engine events | 135 million |
| Orders | 763,349 placed, 759,912 cancelled |
| Rate-limit refusals | 0 |
| Reconnects | 0 |
| Kills | 0 |

Four days of live trading on the same setup: 1.0 billion market-data messages, 2.5 million orders placed and 2.5 million cancelled.
