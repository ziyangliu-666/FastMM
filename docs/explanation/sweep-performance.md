# Sweep performance

A point of a sweep costs 4 to 37 times less than the same backtest run alone, because the points share their passes over the data ([Many runs in one pass](backtesting.md#many-runs-in-one-pass)). The gain is largest for a strategy that is cheap per message and smallest for one that sends many orders.

| Machine | Processor | GPU |
|---|---|---|
| Desktop | AMD Ryzen 7 7800X3D, 8 cores | NVIDIA GeForce RTX 3090 |
| Server | AMD EPYC (Zen 4), 64 cores | |
| GPU server | | NVIDIA H100 80GB HBM3 |

A section that names no machine ran on the desktop. Grids, commands and method: [`bench/README.md`](../../bench/README.md#sweeps).

## Data

| Data | Fill model | Market data a run | A backtest alone |
|---|---|---:|---:|
| Binance USDⓈ-M BTCUSDT, 2026-09-01 00:00 to 04:00 UTC, Tardis book and trades | `l2_queue` | 939,569 events | 3.3 s |
| The same with Bybit BTCUSDT | `l2_queue` | 1,643,662 events | 4.4 s |
| Nasdaq TotalView-ITCH, 2019-07-30 09:30 to 16:00, 8 large stocks, every order | `l3_queue` | 5,353,515 events | 3.6 s |

The configurations are [`configs/backtest-binance.toml`](../../configs/backtest-binance.toml), [`configs/backtest-xmm.toml`](../../configs/backtest-xmm.toml) and [`configs/backtest-itch.toml`](../../configs/backtest-itch.toml). Fetching the data: [Market-data sources](../reference/data-sources.md#tardis).

## One thread

Each row is one pass of 64 variants on one thread, against a backtest of its own per variant. The variants are a grid over the strategy's parameters and the order latency.

| Data | Strategy | A variant in the pass | Against alone |
|---|---|---:|---:|
| BTCUSDT | `basic_mm`, our orders in the feed | 137 ms | 24x |
| BTCUSDT | `basic_mm`, our orders not in the feed | 90 ms | 37x |
| BTCUSDT | `avellaneda_stoikov`, our orders in the feed | 195 ms | 17x |
| BTCUSDT on Binance and Bybit | `xmm`, quoting on Binance and hedging on Bybit | 386 ms | 11x |
| BTCUSDT | the `fastmm init` strategy, `@fastmm.hot` hooks | 158 ms | 20x |
| ITCH | `basic_mm`, 1-tick requote | 502 ms | 7x |
| ITCH | `avellaneda_stoikov` | 931 ms | 4x |

A strategy that declares the mid band it watches is handed a book only when the mid leaves it. The `fastmm init` strategy does, and costs about what `basic_mm` does. Without its band it took 248 ms a variant, 13x, because its hook ran for each of the 939,569 messages in every variant.

## Where a variant's time goes

Share of a pass's time, from `perf record -g` of the passes above:

| | BTCUSDT `basic_mm` | ITCH `basic_mm`, 1-tick requote |
|---|---:|---:|
| Reading, ordering and applying the data to the shared books, once a pass | 44 % | 8 % |
| Engines handling market data: strategy, risk, OMS | 9 % | 28 % |
| The venue taking orders and the recorded events that reach them | 14 % | 20 % |
| Engines taking acks, cancel acks and fills | 14 % | 16 % |
| Keeping the runs in step | 9 % | 20 % |

On BTCUSDT most of the time is the pass's own, which more variants spread thinner. On ITCH a 1-tick requote sends up to 260,000 replaces a day per variant. Each is a strategy decision, a trip through risk and the OMS, a place in the venue's queue and two order events back.

## Threads

1,024 BTCUSDT variants of `basic_mm` on the server:

| Threads | Wall | Speed-up | Peak memory |
|---:|---:|---:|---:|
| 1 | 196.0 s | 1.0 | 1.5 GB |
| 2 | 86.9 s | 2.3 | 2.3 GB |
| 4 | 43.2 s | 4.5 | 3.9 GB |
| 8 | 22.0 s | 8.9 | 7.0 GB |
| 16 | 12.2 s | 16.1 | 7.0 GB |
| 32 | 7.1 s | 27.5 | 7.1 GB |
| 48 | 5.6 s | 34.9 | 7.2 GB |
| 64 | 4.9 s | 40.0 | 7.2 GB |

Two threads gain more than twice because a pass holds at most 128 points. One thread runs the 1,024 points in 8 waves and reads the data 8 times; two threads need 4. Each pass also keeps its own books, about 2 s of work however few points it holds, which is 40 % of the time at 64 threads. The desktop took 24.1 s on 8 threads.

## Thousands of points

A run's engine and venue take about 6 MB, so a sweep on 8 threads holds at most 1,024 runs, about 7 GB. A result keeps its fills and equity bars but no order rows. `fastmm-backtest --sweep` of BTCUSDT `basic_mm` on 8 threads; a point is the wall time times 8 threads over the points:

| Points | Wall | A point | Peak memory |
|---:|---:|---:|---:|
| 16 | 2.4 s | 1,180 ms | 0.25 GB |
| 128 | 2.7 s | 170 ms | 1.0 GB |
| 1,024 | 17.1 s | 134 ms | 6.8 GB |
| 4,096 | 57.9 s | 113 ms | 6.9 GB |
| 16,384 | 228 s | 111 ms | 7.7 GB |

Up to 1,024 points the sweep is one wave of 8 passes, each paying about 2 s for its books. Past that, each wave pays it again, and a point costs what its own run costs.

## GPUs

FastMM runs no part of a sweep on a GPU, for three reasons:

| Obstacle | Why |
|---|---|
| Divergent branches | each run takes each event through branches that differ from run to run, which a GPU warp runs one after another |
| Round trip per step | each step decides the next, so handing a GPU part of every step costs 12 µs on an H100, against 0.1 to 0.2 µs a run spends on an event on the CPU |
| Two engines | running whole runs on a GPU needs a second engine for every strategy |

A `basic_mm` kernel that holds a variant in a few hundred bytes shows what a GPU gains on the same work. Its outputs equal the CPU's bit for bit:

| Code | Hardware | 1,024 variants | 8,192 variants |
|---|---|---:|---:|
| The engine, any strategy | desktop, 8 threads | 72.9 s | |
| | server, 64 threads | 12.7 s | |
| `basic_mm` kernel, CPU | desktop, 8 threads | 5.8 s | 35.2 s |
| | server, 64 threads | 5.9 s | 9.3 s |
| `basic_mm` kernel, GPU | RTX 3090 | 3.5 s | 17.0 s |
| | H100 | 2.3 s | 7.6 s |

The RTX 3090 ran 1.6 and 2.1 times as fast as the desktop's 8 cores, the H100 2.6 and 1.2 times as fast as the server's 64 cores. Preparing the GPU's input takes another 4.2 to 4.6 s on one core. Counting it, only the RTX 3090 on 8,192 variants comes out ahead.
