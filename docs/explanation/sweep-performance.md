# Sweep performance

A sweep under `l2_queue` or `l3_queue` runs its points in shared passes over the data ([Many runs in one pass](backtesting.md#many-runs-in-one-pass)). The data is read, ordered and applied to the venue's books once per pass; each point still runs its own engine, strategy and orders. What a point costs is therefore what its strategy does: the market data its hooks take and the orders it sends.

Every number here was measured on one machine: an AMD Ryzen 7 7800X3D (8 cores) under WSL2 with 19 GB of memory, Linux 6.6, gcc 13.3, preset `release-nolto`, on 2026-10-27.

## Data

| Data | Fill model | Market data a run | A backtest alone |
|---|---|---:|---:|
| Tardis `incremental_book_L2` and `trades`, Binance USDⓈ-M BTCUSDT, 2026-09-01 00:00 to 04:00 UTC | `l2_queue` | 939,569 events | 3.2 s |
| Nasdaq TotalView-ITCH, 2019-07-30 09:30 to 16:00, AAPL MSFT AMZN FB INTC CSCO NVDA TSLA, every order | `l3_queue` | 5,353,515 events | 3.3 s |

The ITCH file was the day's file reduced to the messages of the eight stocks (186 MB, 6.1 million of its 282 million messages); read from the whole file a backtest takes about 11 s ([Market-data sources](../reference/data-sources.md#itch)) and a point in a pass costs the same as below. The `tardis` source downloads the day on first use (the first day of a month needs no key). Configurations: [`configs/backtest-binance.toml`](../../configs/backtest-binance.toml) (5 ms latency with 2 ms jitter) and [`configs/backtest-itch.toml`](../../configs/backtest-itch.toml) (50 µs).

## One thread

Each row is one pass of 64 or 128 variants on one thread, against a backtest of its own per variant. The variants are a grid over the strategy's parameters and the order and ack latency (`--latency-us`). Each pass also checked 4 of its variants against `run_backtest()`: every one had the same fills, orders, equity bars, counts and outbound hash.

| Data | Strategy | Variants | Grid | A variant in the pass | Against alone |
|---|---|---:|---|---:|---:|
| BTCUSDT | `basic_mm`, our orders in the feed | 64 | `half_spread_bps` 0.007 to 2, `quote_qty` 0.002, 0.004, `requote_threshold_ticks` 1, 3, latency 2 to 20 ms | 777 ms | 4x |
| BTCUSDT | `basic_mm` | 64 | the same | 95 ms | 33x |
| BTCUSDT | `basic_mm` | 128 | the same, `skew_bps_per_unit` 1 to 8 | 75 ms | 42x |
| BTCUSDT | `avellaneda_stoikov` | 64 | `gamma` 0.01 to 10, `kappa` 0.5 to 15, `quote_qty` 0.002, 0.004, latency 5, 20 ms | 153 ms | 20x |
| ITCH | `basic_mm` | 64 | `half_spread_bps` 0.5 to 4, `requote_threshold_ticks` 1, 3, `quote_qty` 100, 200, latency 20 µs to 1 ms | 469 ms | 7x |
| ITCH | `basic_mm` | 64 | `half_spread_bps` 2 to 16, `requote_threshold_ticks` 3, 5, the same | 219 ms | 15x |
| ITCH | `avellaneda_stoikov` | 64 | `gamma` 0.01 to 10, `kappa` 10 to 1000, `quote_qty` 100, 200, latency 50 µs, 1 ms | 924 ms | 4x |

Rows without "our orders in the feed" set `[backtest] own_orders_in_feed = false`. A run whose feed shows its own orders sees market data no other run sees, so it forwards and applies its own, as a backtest alone does; it shares the reading and ordering of the data and the venue's books, and 41 % of its time goes to applying the depth deltas to its own engine books. A run that shares the feed is handed only what its engine asks for: `basic_mm` asks for a book update once the mid has moved its requote threshold, and a variant on ITCH took about 5 % of the messages. `avellaneda_stoikov` updates its volatility estimate on every book update, so each of its variants takes every message. Between those, the cost of a variant is its order flow: a `requote_threshold_ticks` of 1 tick on eight Nasdaq stocks sends 125,000 to 260,000 replaces a day.

## Eight threads

The passes of a sweep run on its threads; a pass holds up to 128 runs. The separate backtests ran as `fastmm-backtest --sweep ... --solo --threads 8`, 0.43 s a point on BTCUSDT and 0.50 s on ITCH.

| Data | Strategy | Variants | Wall | Separate backtests, wall | Peak memory |
|---|---|---:|---:|---:|---:|
| BTCUSDT | `basic_mm` | 1,025 | 18.6 s | about 445 s | 9.0 GB |
| ITCH | `basic_mm` | 257 | 19.6 s | about 128 s | 8.6 GB |

Each check ran 8 variants again alone; all were the same. A run holds about 9 MB besides its result, and the result keeps every order sent: on ITCH with a 1-tick threshold, about 23 MB a run at the end of the day.

## Against the passes before

Before this release a pass held 8 runs, and each run took every market-data message into its own engine books; `l3_queue` ran every point alone. The same grid of 64 `basic_mm` points through `fastmm-backtest --sweep ... --threads 1`, built from each commit with `release-nolto`:

| Data | Fill model | Before (`7111b229`) | Now | A backtest alone |
|---|---|---:|---:|---:|
| BTCUSDT, `own_orders_in_feed = false`, no jitter | `l2_queue` | 60.0 s | 6.4 s | 3.2 s a point |
| ITCH | `l2_queue` | 92.0 s | 29.3 s | 2.8 s a point |
| ITCH | `l3_queue` | each point alone | 30.3 s | 3.3 s a point |

Both builds printed the same results, outbound hash included. With 256 points on 8 threads the BTCUSDT grid took 32.0 s before and 5.3 s now.

## Method

`fastmm-bench-multiverse` builds the variants (the configuration's own first, then the grid in a fixed shuffled order), runs them through `bt::run_multiverse`, and with `--check K` runs K of them, spread over the list, through `run_backtest()`, times them and compares each result with `bt::result_difference`; any difference fails the run. "A variant in the pass" is the pass's wall time times its threads over its variants; "Against alone" divides the checked backtests' mean time by it. Memory is the process's peak resident set (`/usr/bin/time -f %M`). Each row is one run on an otherwise idle machine.

```bash
cmake --preset release-nolto && cmake --build --preset release-nolto --target fastmm-bench-multiverse fastmm-backtest
./build/release-nolto/bin/bench/fastmm-bench-multiverse --config configs/backtest-binance.toml \
    --data "tardis:binance-futures,BTCUSDT,2026-09-01,end=04:00" \
    --sweep half_spread_bps=0.007,0.5,1,2 --sweep quote_qty=0.002,0.004 \
    --sweep requote_threshold_ticks=1,3 --latency-us 2000,5000,10000,20000 --variants 64 --check 4
./build/release-nolto/bin/bench/fastmm-bench-multiverse --config configs/backtest-itch.toml \
    --data "itch:07302019.NASDAQ_ITCH50,start=09:30,end=16:00" \
    --sweep half_spread_bps=0.5,1,2,4 --sweep requote_threshold_ticks=1,3 --sweep quote_qty=100,200 \
    --latency-us 20,50,200,1000 --variants 64 --check 4
```

`--strategy avellaneda_stoikov` runs that strategy from its defaults under the `--sweep` values (the AS rows add `--sweep max_inventory=0.02` and `1000`), and `--threads 8` spreads the passes over eight threads. A user's own sweep checks itself the same way: `fastmm-backtest --sweep ... --verify 4` and `fastmm.sweep(..., verify=4)` run 4 of its points again alone and fail on the first difference.
