# Sweep performance

What a sweep's points cost when they share their passes over the data, in time and memory, measured. How the sharing works, and how a point's result is kept equal to its own backtest's, is in [Many runs in one pass](backtesting.md#many-runs-in-one-pass).

Every number here was measured on one machine: an AMD Ryzen 7 7800X3D (8 cores, 8 threads under WSL2) with 19 GB of memory, Linux 6.6, gcc 13.3, preset `release-nolto`, on 2026-10-27. "Before" is the same command built from `1054eb6b`, the commit before this work.

## Data

| Data | Fill model | Market data a run | A backtest alone |
|---|---|---:|---:|
| Tardis `incremental_book_L2` and `trades`, Binance USDⓈ-M BTCUSDT, 2026-09-01 00:00 to 04:00 UTC | `l2_queue` | 939,569 events | 3.3 s |
| Nasdaq TotalView-ITCH, 2019-07-30 09:30 to 16:00, AAPL MSFT AMZN FB INTC CSCO NVDA TSLA, every order | `l3_queue` | 5,353,515 events | 3.6 s |

The ITCH file was the day's file reduced to the messages of the eight stocks (186 MB, 6.1 million of its 282 million messages). The `tardis` source downloads the day on first use (the first day of a month needs no key). Configurations: [`configs/backtest-binance.toml`](../../configs/backtest-binance.toml) (5 ms latency with 2 ms jitter, our orders in the feed) and [`configs/backtest-itch.toml`](../../configs/backtest-itch.toml) (50 µs, our orders not in the feed).

## One thread

Each row is one pass of 64 or 128 variants on one thread, against a backtest of its own per variant; the variants are a grid over the strategy's parameters and the order and ack latency (`--latency-us`). A variant in the pass is the pass's time over its variants. Every pass also ran 2 of its variants again alone, and each had the same fills, orders, equity bars, counts and outbound hash.

| Data | Strategy | Variants | A variant in the pass | Against alone | Before | Before, against alone |
|---|---|---:|---:|---:|---:|---:|
| BTCUSDT | `basic_mm`, our orders in the feed | 64 | 137 ms | 24x | 836 ms | 4x |
| BTCUSDT | `basic_mm`, our orders in the feed, market data jittered 2 ms | 64 | 130 ms | 26x | each variant alone | 1x |
| BTCUSDT | `basic_mm`, our orders in the feed | 128 | 91 ms | 38x | | |
| BTCUSDT | `basic_mm`, our orders not in the feed | 64 | 90 ms | 37x | 98 ms | 35x |
| BTCUSDT | `avellaneda_stoikov`, our orders in the feed | 64 | 195 ms | 17x | 855 ms | 4x |
| ITCH | `basic_mm`, 1-tick requote | 64 | 502 ms | 7x | 528 ms | 7x |
| ITCH | `basic_mm`, 3 to 5 ticks | 64 | 216 ms | 16x | 243 ms | 14x |
| ITCH | `avellaneda_stoikov` | 64 | 931 ms | 4x | 1,122 ms | 4x |

Grids: BTCUSDT `basic_mm` `half_spread_bps` 0.007 to 2, `quote_qty` 0.002, 0.004, `requote_threshold_ticks` 1, 3, latency 2 to 20 ms, and for 128 variants `skew_bps_per_unit` 1 to 8; `avellaneda_stoikov` `gamma` 0.01 to 10, `kappa` 0.5 to 15, `quote_qty` 0.002, 0.004, latency 5, 20 ms. ITCH `basic_mm` `half_spread_bps` 0.5 to 4 with `requote_threshold_ticks` 1, 3 (or 2 to 16 with 3, 5), `quote_qty` 100, 200, latency 20 µs to 1 ms; `avellaneda_stoikov` `kappa` 10 to 1000, latency 50 µs, 1 ms.

## Where a variant's time goes

The share of a pass's time in what it does, from `perf record -g` of the 64-variant passes above (inclusive: a function's share counts what it calls).

| | BTCUSDT `basic_mm`, our orders in the feed | ITCH `basic_mm`, 1-tick requote |
|---|---:|---:|
| Reading, ordering and applying the data to the venue's and the engines' shared books (once a pass) | 44 % | 8 % |
| Engines handed market data: the strategy, its orders through risk and the OMS to the venue (SHA-256 of each included) | 9 % | 28 % |
| The venue taking each run's orders and the recorded events that reach them | 14 % | 20 % |
| Engines taking their order events: acks, cancel acks, fills | 14 % | 16 % |
| Keeping the runs in step: wake and resting-order indexes, catch-up, next events | 9 % | 20 % |

On BTCUSDT a variant's own cost is mostly the venue and its engine; the rest is the pass's, which a pass of more variants spreads thinner (128 variants: 91 ms each against 137 ms). On ITCH with a 1-tick requote a variant sends up to 260,000 replaces in a day, and each one is a strategy decision, a trip through the engine's risk checks, OMS and quote manager, a place in the simulated venue's queue and two order events back. A kernel specialised to `basic_mm` without the engine took about 20 ms a variant on the same day; the general path runs the engine every live session runs.

## Eight threads

The passes of a sweep run on its threads, each with its own cursor over the data. The separate backtests are an estimate: the mean time of the checked variants alone, times the variants, over 8 threads.

| Data | Strategy | Variants | Wall | Peak memory | Before, wall | Before, peak memory | Separate backtests, wall |
|---|---|---:|---:|---:|---:|---:|---:|
| BTCUSDT, our orders in the feed | `basic_mm` | 1,025 | 53 s | 7.3 GB | 310 s | 10.1 GB | about 480 s |
| ITCH | `basic_mm` | 257 | 20 s | 5.5 GB | 31 s | 8.1 GB | about 116 s |

The BTCUSDT grid adds `skew_bps_per_unit` 1 to 8 and `levels` 1 to 4, the ITCH grid `skew_bps_per_unit` 0.5 to 4. Each pass ran 8 variants again alone; all were the same.

## Thousands of points

A pass holds up to 128 runs, so a sweep on 8 threads has at most 1,024 runs in memory, each with its engine, its venue and its result. A run's engine and venue take about 6 MB. Its result keeps its fills and equity bars but no order rows, which grow with every order sent (up to about 20 MB a day for the 1-tick ITCH variants); `fastmm-backtest --sweep` keeps of each point only the line it prints.

| Command | Points | Wall | Peak memory |
|---|---:|---:|---:|
| `fastmm-backtest --sweep`, BTCUSDT, `basic_mm`, our orders in the feed, 8 threads | 2,048 | 62 s | 7.5 GB |

Before this work a pass held the order rows of every run, and the results of every point stayed until the sweep ended.

## Method

`fastmm-bench-multiverse` builds the variants (the configuration's own first, then the grid in a fixed shuffled order), runs them through `bt::run_multiverse` as a sweep's points (no order rows), and with `--check K` runs K of them, spread over the list, through `run_backtest()`, times them and compares each result with `bt::result_difference`; any difference fails the run. "A variant in the pass" is the pass's wall time times its threads over its variants; "Against alone" divides the checked backtests' mean time by it. Memory is the process's peak resident set (`/usr/bin/time -f %M`), the checked backtests included. Each row is one run on an otherwise idle machine.

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
./build/release-nolto/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data "tardis:binance-futures,BTCUSDT,2026-09-01,end=04:00" \
    --sweep half_spread_bps=0.007,0.1,0.25,0.5,1,1.5,2,3 --sweep quote_qty=0.002,0.004 \
    --sweep requote_threshold_ticks=1,3 --sweep skew_bps_per_unit=1,2,4,8 --sweep levels=1,2,3,4 \
    --sweep max_inventory=0.02,0.04,0.08,0.16 --threads 8 --verify 4
```

`--own-feed 0` turns our orders out of the feed and `--md-jitter-us 2000` jitters the market data; `--strategy avellaneda_stoikov` runs that strategy from its defaults under the `--sweep` values (the rows above add `--sweep max_inventory=0.02` and `1000`), and `--threads 8` spreads the passes over eight threads.
