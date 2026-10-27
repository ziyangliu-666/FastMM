# How fast it is

<video class="fastmm-video" controls preload="none" poster="../../assets/intro-poster.jpg" src="https://github.com/ziyangliu-666/FastMM/releases/download/v0.5.1/fastmm-intro.mp4"></video>

One `fastmm-live` session on Binance, 45 hours 39 minutes without a restart, on 2 physical cores ([Cores](../how-to/operations/running-in-production.md#cores)). The engine alone: [Benchmarks](benchmarks.md).

## Market data to order

From the market-data frame read (T0) to the order handed to the network thread (T5) ([latency intervals](../reference/status-file.md#latency-intervals)):

| Interval | Count | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Decode (T0 to T1) | 306,617,750 | 0.61 µs | 5.6 µs | 12.3 µs |
| Book apply (T1 to T2) | 306,617,750 | 0.96 µs | 30.7 µs | 4.46 ms |
| Strategy (T2 to T3) | 306,617,750 | 1.15 µs | 3.5 µs | 24.6 µs |
| Serialize (T3 to T4) | 1,386,841 | 0.96 µs | 47.1 µs | 77.8 µs |
| Send (T4 to T5) | 1,728,607 | 0.50 µs | 1.7 µs | 23.6 µs |
| Tick to trade (T0 to T5) | 1,706,236 | 6.7 µs | 81.9 µs | 4.06 ms |

T1 is stamped on a network thread and T2 on the engine thread, so book apply and tick to trade include the time an event waits in the ring while the engine is busy or descheduled.

## On the wire

From T0 to the return of the `write` that carried the signed, TLS-encrypted order, per account:

| Account | Samples | p50 | p99 |
|---|---:|---:|---:|
| 1 | 358,396 | 23.9 µs | 136.5 µs |
| 2 | 356,564 | 23.9 µs | 273.1 µs |
| 3 | 357,678 | 23.9 µs | 259.4 µs |
| 4 | 355,920 | 24.7 µs | 259.4 µs |
| 5 | 357,773 | 24.7 µs | 259.4 µs |

## Load

| | |
|---|---|
| Market-data messages | 1.03 billion, about 6,300 a second; 0 dropped, 0 malformed |
| Engine events | 328 million |
| Orders | 952,762 placed, 949,953 cancelled |
| Orders refused for a rate limit by a venue | 0 |
| Reconnects | 3 |
| Book resynchronisations | 72 |
| Kill switch trips | 0 |
| Store records dropped | 0 |
