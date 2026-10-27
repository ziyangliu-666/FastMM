# Sweep performance

What a sweep's points cost when they share their passes over the data, how a sweep scales with cores, and what a GPU would take on. How the sharing works, and how each point's result is kept equal to its own backtest's, is in [Many runs in one pass](backtesting.md#many-runs-in-one-pass).

| Machine | Processor | GPU |
|---|---|---|
| Desktop | AMD Ryzen 7 7800X3D, 8 cores | NVIDIA GeForce RTX 3090 |
| Server | AMD EPYC (Zen 4), 64 cores | |
| GPU server | | NVIDIA H100 80GB HBM3 |

Builds use gcc 13.3 and the preset `release-nolto`. A section that names no machine was measured on the desktop.

## Data

| Data | Fill model | Market data a run | A backtest alone |
|---|---|---:|---:|
| Tardis `incremental_book_L2` and `trades`, Binance USDⓈ-M BTCUSDT, 2026-09-01 00:00 to 04:00 UTC | `l2_queue` | 939,569 events | 3.3 s |
| Tardis `incremental_book_L2` and `trades`, Binance USDⓈ-M BTCUSDT and Bybit BTCUSDT, 2026-09-01 00:00 to 04:00 UTC | `l2_queue` | 1,643,662 events | 4.4 s |
| Nasdaq TotalView-ITCH, 2019-07-30 09:30 to 16:00, AAPL MSFT AMZN FB INTC CSCO NVDA TSLA, every order | `l3_queue` | 5,353,515 events | 3.6 s |

The ITCH file is the day's file reduced to the messages of the eight stocks (186 MB, 6.1 million of its 282 million messages). Fetch the day first ([Market-data sources](../reference/data-sources.md#tardis)); the first day of a month needs no key. Configurations: [`configs/backtest-binance.toml`](../../configs/backtest-binance.toml) (5 ms latency with 2 ms jitter, our orders in the feed), [`configs/backtest-xmm.toml`](../../configs/backtest-xmm.toml) (the same to Binance, 15 ms to Bybit) and [`configs/backtest-itch.toml`](../../configs/backtest-itch.toml) (50 µs, our orders not in the feed).

## One thread

Each row is one pass of 64 variants on one thread, against a backtest of its own per variant; the variants are a grid over the strategy's parameters and the order and ack latency (`--latency-us`). A variant in the pass is the pass's time over its variants. Every pass also ran 2 of its variants again alone, and each had the same fills, orders, equity bars, counts and outbound hash.

| Data | Strategy | A variant in the pass | Against alone |
|---|---|---:|---:|
| BTCUSDT | `basic_mm`, our orders in the feed | 137 ms | 24x |
| BTCUSDT | `basic_mm`, our orders not in the feed | 90 ms | 37x |
| BTCUSDT | `avellaneda_stoikov`, our orders in the feed | 195 ms | 17x |
| BTCUSDT on Binance and Bybit | `xmm`, quoting on Binance and hedging on Bybit, our orders in the feed | 386 ms | 11x |
| BTCUSDT | the strategy `fastmm init` writes, `@fastmm.hot` hooks, our orders in the feed | 158 ms | 20x |
| ITCH | `basic_mm`, 1-tick requote | 502 ms | 7x |
| ITCH | `avellaneda_stoikov` | 931 ms | 4x |

Grids: BTCUSDT `basic_mm` `half_spread_bps` 0.007 to 2, `quote_qty` 0.002, 0.004, `requote_threshold_ticks` 1, 3, latency 2 to 20 ms; `avellaneda_stoikov` `gamma` 0.01 to 10, `kappa` 0.5 to 15, `quote_qty` 0.002, 0.004, latency 5, 20 ms; `xmm` `edge_bps` 0 to 2, `slippage_bps` 0, 0.5, `quote_qty` 0.002, 0.004, `requote_threshold_ticks` 1, 3, `basis_halflife_s` 30, 120, latency 2 to 20 ms; the `fastmm init` strategy `half_spread_bps` 0.007 to 2, `quote_qty` 0.002, 0.004, `requote_ticks` 1, 3, latency 2 to 20 ms. ITCH `basic_mm` `half_spread_bps` 0.5 to 4, `requote_threshold_ticks` 1, 3, `quote_qty` 100, 200, latency 20 µs to 1 ms; `avellaneda_stoikov` `kappa` 10 to 1000, latency 50 µs, 1 ms.

The `fastmm init` strategy quotes as `basic_mm` does, with 7 % fewer fills, and like it declares the band the mid can move in before it requotes, so its engine is handed a book only when the mid leaves it. In the same run `basic_mm` took 97 ms a variant; the `fastmm init` strategy without its band took 248 ms (13x), its hook called for each of the 939,569 messages in every variant.

## Where a variant's time goes

The share of a pass's time in what it does, from `perf record -g` of the passes above (inclusive: a function's share counts what it calls).

| | BTCUSDT `basic_mm`, our orders in the feed | ITCH `basic_mm`, 1-tick requote |
|---|---:|---:|
| Reading, ordering and applying the data to the venue's and the engines' shared books (once a pass) | 44 % | 8 % |
| Engines handed market data: the strategy, its orders through risk and the OMS to the venue (SHA-256 of each included) | 9 % | 28 % |
| The venue taking each run's orders and the recorded events that reach them | 14 % | 20 % |
| Engines taking their order events: acks, cancel acks, fills | 14 % | 16 % |
| Keeping the runs in step: wake and resting-order indexes, catch-up, next events | 9 % | 20 % |

On BTCUSDT a variant's own cost is mostly the venue and its engine; the rest is the pass's, which a pass of more variants spreads thinner. On ITCH with a 1-tick requote a variant sends up to 260,000 replaces in a day, and each one is a strategy decision, a trip through the engine's risk checks, OMS and quote manager, a place in the simulated venue's queue and two order events back.

## Threads

1,024 BTCUSDT variants of `basic_mm` (the grid of [One thread](#one-thread) with `skew_bps_per_unit` 1 to 8 and `levels` 1 to 4) on the server. The speed-up is against one thread.

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

Two threads gain more than twice because a pass holds at most 128 points: one thread runs the 1,024 points in 8 waves and reads the data 8 times, two threads in 4. Each pass also keeps its own venue and engine books, about 2 s of work here however few points it holds; at 64 threads that is 40 % of the time. The desktop took 24.1 s on 8 threads.

The same sweep with 0.9.0, where `fastmm.sweep` ran each point as a backtest of its own, on at most 32 threads:

| Machine | Threads | 0.9.0 | Now |
|---|---:|---:|---:|
| Desktop | 8 | 476 s | 30 s |
| Server | 64 | 145 s | 5.8 s |

## Thousands of points

A pass holds up to 128 runs, so a sweep on 8 threads has at most 1,024 runs in memory, each with its engine, its venue and its result. A run's engine and venue take about 6 MB. Its result keeps its fills and equity bars but no order rows, which grow with every order sent (up to about 20 MB a day for the 1-tick ITCH variants); `fastmm-backtest --sweep` keeps of each point only the line it prints.

`fastmm-backtest --sweep` of BTCUSDT `basic_mm`, our orders in the feed, on 8 threads; a point is the sweep's wall time times 8 threads over its points.

| Points | Wall | A point | Peak memory |
|---:|---:|---:|---:|
| 16 | 2.4 s | 1,180 ms | 0.25 GB |
| 128 | 2.7 s | 170 ms | 1.0 GB |
| 1,024 | 17.1 s | 134 ms | 6.8 GB |
| 4,096 | 57.9 s | 113 ms | 6.9 GB |
| 16,384 | 228 s | 111 ms | 7.7 GB |

Up to 1,024 points the sweep is one wave: 8 passes that each keep their own books, about 2 s however few points they hold. Past it each wave pays that again, and a point costs what its own run costs.

## GPUs

FastMM runs no part of a sweep on a GPU. A run keeps about 6 MB of state and takes each event through branches that differ from run to run, which a GPU warp runs one after another. Each step decides the next, so handing a GPU part of every step costs a round trip per step: 12 µs on the H100, against 0.1 to 0.2 µs a run spends on a market-data event on the CPU. Running whole runs on a GPU would need a second implementation of the engine for every strategy. The table measures what a GPU gains on the same code: the `basic_mm` lane kernel of the branch `research/gpu-lanes`, a variant as a few hundred bytes of integer state, whose GPU outputs equalled the CPU's bit for bit.

| Code | Hardware | 1,024 variants | 8,192 variants |
|---|---|---:|---:|
| The engine, any strategy | desktop, 8 threads | 72.9 s | |
| | server, 64 threads | 12.7 s | |
| `basic_mm` lanes, CPU | desktop, 8 threads | 5.8 s | 35.2 s |
| | server, 64 threads | 5.9 s | 9.3 s |
| `basic_mm` lanes, GPU | RTX 3090 | 3.5 s | 17.0 s |
| | H100 | 2.3 s | 7.6 s |

The RTX 3090 ran the lanes 1.6 and 2.1 times as fast as the desktop's 8 cores, the H100 2.6 and 1.2 times as fast as the server's 64 cores. The GPU times leave out the 4.2 to 4.6 s the kernel's tape takes to build on one core; counting it, only the RTX 3090 on 8,192 lanes comes out ahead.

## Method

`fastmm-bench-multiverse` builds the variants (the configuration's own first, then the grid in a fixed shuffled order), runs them through `bt::run_multiverse` as a sweep's points (no order rows), and with `--check K` runs K of them, spread over the list, through `run_backtest()`, times them and compares each result with `bt::result_difference`; any difference fails the run. "A variant in the pass" is the pass's wall time times its threads over its variants; "Against alone" divides the checked backtests' mean time by it. Memory is the process's peak resident set (`/usr/bin/time -f %M`). Each row is one run on an otherwise idle machine.

```bash
cmake --preset release-nolto && cmake --build --preset release-nolto --target fastmm-bench-multiverse fastmm-backtest
./build/release-nolto/bin/bench/fastmm-bench-multiverse --config configs/backtest-binance.toml \
    --data "tardis:binance-futures,BTCUSDT,2026-09-01,end=04:00" \
    --sweep half_spread_bps=0.007,0.5,1,2 --sweep quote_qty=0.002,0.004 \
    --sweep requote_threshold_ticks=1,3 --latency-us 2000,5000,10000,20000 --variants 64 --check 2
./build/release-nolto/bin/bench/fastmm-bench-multiverse --config configs/backtest-itch.toml \
    --data "itch:07302019.NASDAQ_ITCH50,start=09:30,end=16:00" \
    --sweep half_spread_bps=0.5,1,2,4 --sweep requote_threshold_ticks=1,3 --sweep quote_qty=100,200 \
    --latency-us 20,50,200,1000 --variants 64 --check 2
./build/release-nolto/bin/bench/fastmm-bench-multiverse --config configs/backtest-binance.toml \
    --data "tardis:binance-futures,BTCUSDT,2026-09-01,end=04:00" \
    --sweep half_spread_bps=0.007,0.1,0.25,0.5,1,1.5,2,3 --sweep quote_qty=0.002,0.004 \
    --sweep requote_threshold_ticks=1,3 --sweep skew_bps_per_unit=1,2,4,8 --sweep levels=1,2,3,4 \
    --latency-us 2000,5000,10000,20000 --variants 1024 --threads 64 --check 4
./build/release-nolto/bin/bench/fastmm-bench-multiverse --config configs/backtest-xmm.toml \
    --data "tardis:binance-futures,BTCUSDT,2026-09-01,end=04:00,inst=0,venue=0; tardis:bybit,BTCUSDT,2026-09-01,end=04:00,inst=1,venue=1" \
    --sweep edge_bps=0,0.5,1,2 --sweep slippage_bps=0,0.5 --sweep quote_qty=0.002,0.004 \
    --sweep requote_threshold_ticks=1,3 --sweep basis_halflife_s=30,120 \
    --latency-us 2000,5000,10000,20000 --variants 64 --check 2
./build/release-nolto/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data "tardis:binance-futures,BTCUSDT,2026-09-01,end=04:00" \
    --sweep half_spread_bps=0.007,0.5,1,2 --sweep levels=1,2 --sweep requote_threshold_ticks=1,3 \
    --sweep skew_bps_per_unit=$(seq -s, 0.01 0.01 10.24) --threads 8 --verify 2
```

`--own-feed 0` turns our orders out of the feed; `--strategy avellaneda_stoikov` runs that strategy from its defaults under the `--sweep` values (the rows above add `--sweep max_inventory=0.02` and `1000`). [Thousands of points](#thousands-of-points) runs the last command with the first 1, 8, 64, 256 and 1,024 values of `skew_bps_per_unit`. [Threads](#threads) runs the third command with `--threads` 1 to 64, and the engine rows of [GPUs](#gpus) the second with `--sweep skew_bps_per_unit=0.5,1,2,4 --sweep max_inventory=500,1000,2000,5000 --variants 1024` on 8 and 64 threads. The `fastmm init` row times `fastmm.sweep(cfg, grid, data=..., strategy=Mm, threads=1, summary_only=True)` against `fastmm.run_backtest` of 2 of its points, which gave the same outbound hashes, with `max_inventory` 0.02 and `skew_bps` 2. The 0.9.0 comparison runs `max_inventory` 0.02, 0.04 in place of the latency grid, through `fastmm-backtest --sweep` now and, for 0.9.0, `pip install "fastmm-engine==0.9.0"` and `fastmm.sweep(fastmm.BacktestConfig.from_toml("configs/backtest-binance.toml"), grid, data="tardis:binance-futures,BTCUSDT,2026-09-01,end=04:00", threads=64)` with that release's configuration.

The lanes of [GPUs](#gpus) run `basic_mm` under `l3_queue` with one level a side, fixed latencies, our orders not in the feed and no risk limit but self-trade prevention, on a grid of `half_spread_bps` 0.25 to 8, `skew_bps_per_unit` 0 to 2, `quote_qty` 100 to 500, `max_inventory` 300 to 2,000 and none, `requote_threshold_ticks` 1 to 3 and latency 20 to 500 µs over the ITCH day. The GPU driver compares every output with the CPU's: fills, cash, fees, position and a hash of the fills. It needs the CUDA toolkit:

```bash
git switch --detach origin/research/gpu-lanes
cmake --preset release-nolto && cmake --build --preset release-nolto --target fastmm-bench-population
./build/release-nolto/bin/bench/fastmm-bench-population --config bench/gpu/itch-lanes.toml \
    --data "itch:07302019.NASDAQ_ITCH50,start=09:30,end=16:00" --lanes 1024 --threads 8 --kernel \
    --tape-out /tmp/lanes
nvcc -O3 -std=c++17 -arch=native -Iinclude -o population_gpu bench/gpu/population_gpu.cu
./population_gpu /tmp/lanes.tape /tmp/lanes.lanes
nvcc -O2 -arch=native -o round_trip bench/gpu/round_trip.cu && ./round_trip
```

`fastmm-bench-population` runs the lanes on the CPU and writes the tape and the CPU's outputs, which the GPU driver checks its own against; `--lanes 8192` gives the larger population.
