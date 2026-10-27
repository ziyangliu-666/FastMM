# Benchmarks

The results and the machine are on [Benchmarks](../docs/explanation/benchmarks.md); every benchmark is in [full-results.md](full-results.md). `scripts/bench.sh` regenerates both and this file.

```bash
scripts/bench-e2e.sh --duration 30 --runs 3 --sim-cpu 0 --engine-cpu 1 --net-cpu 3 --out bench/results/e2e
./scripts/bench.sh --preset release-native --cpu 1 --rounds 3 --e2e bench/results/e2e
python3 tools/check_budgets.py bench/results/latest
python3 tools/bench_compare.py bench/results/baseline bench/results/latest
```

`check_budgets.py` fails when a benchmark exceeds its budget in `bench/ci_budget.toml` by more than 10 %. `scripts/build-pgo.sh [--bolt]` builds `release-native` with PGO, and BOLT, trained on these benchmarks; shipped builds do not use it.

For an ITCH change, also replay a real session: NASDAQ publishes full days of TotalView-ITCH 5.0 (`emi.nasdaq.com/ITCH`). `fastmm-bench-l3-itch` times every book message, `--from 09:30` times the open on books warmed by the morning, and `--check` compares every touch with a map book. `BM_ItchL2Bridge_Message_MultiSymbol` interleaves 1, 10 and 100 books, so a datagram publishes several of them.

```bash
build/release/bin/bench/fastmm-bench-l3-itch day.itch --top 100 --from 09:30 --check
```

For a change to the queue models, `fastmm-bench-queue-truth` scores `l2_queue` and `l3_queue` against the same file, where every order's outcome is known ([below](#queue-models-against-itch)).

## Method of the micro-benchmarks

`scripts/bench.sh` pins each benchmark to one core, except the few that need a second thread (`FASTMM_BENCH_NEEDS_CORES`, `bench/bench_pin.hpp`). The machine ran nothing else, without `isolcpus` or a fixed CPU frequency. Each benchmark ran five repetitions in each of three rounds, and `tools/bench_table.py` pools them.

Where a benchmark keeps a per-operation `rdtsc` histogram (buckets about 2 % wide), the tables give its percentiles; otherwise p50 is the median over repetitions of Google Benchmark's mean time. The codec benchmarks time 64 operations per `rdtsc` pair. Two processes of one binary differ by 2 to 5 %, so compare a change over several interleaved processes per side.

Tick to order (`BM_TickToOrder_Sim`) starts when a book delta is pushed into the engine's feed and ends when the last order it caused is serialised into the transport. In the simulator the transport copies each message into a 192-byte scheduler slot, the stand-in for the live engine's outbound ring. The venue, acks and the untimed settle loop between ticks are outside it. The simulator's outbound SHA-256, a determinism check the live engine does not compute, is off; `BM_TickToOrder_SimHash` turns it on.

## Method of the end-to-end run

`scripts/bench-e2e.sh` runs `fastmm-sim-itch` and `fastmm-live` in two network namespaces joined by a veth pair: preset `release`, `configs/nasdaq-itch-sim.toml`, kernel receive, `spin_mode = "busy"`, simulator on CPU 0, engine on CPU 1, network thread on CPU 3 (the engine core's second hyperthread), three runs of 30 s.

Wire to wire is measured by the simulator with its own TSC: from right before the `sendmmsg` that sent an ITCH datagram to right after the read that returned the order it caused. Each order carries the MoldUDP64 sequence number that triggered it in its ClOrdID, so the simulator matches them. `fastmm-live` measures the other rows from its T0 to T5 stamps. On veth the order's `write` runs the simulator's TCP receive path inside the system call, which is most of the gap between T5 and wire to wire.

## Sweeps

The numbers are on [Sweep performance](../docs/explanation/sweep-performance.md). Builds use gcc 13.3 and the preset `release-nolto`; each row is one run on an otherwise idle machine. The ITCH file is the day's file reduced to the messages of AAPL, MSFT, AMZN, FB, INTC, CSCO, NVDA and TSLA (186 MB, 6.1 million of its 282 million messages). The BTCUSDT configuration runs 5 ms latency with 2 ms jitter and our orders in the feed; `xmm` adds 15 ms to Bybit; ITCH runs 50 µs with our orders not in the feed.

`fastmm-bench-multiverse` builds the variants (the configuration's own first, then the grid in a fixed shuffled order) and runs them through `bt::run_multiverse` as a sweep's points. With `--check K` it runs K of them, spread over the list, through `run_backtest()`, times them and compares each result field by field with `bt::result_difference`; any difference fails the run. "A variant in the pass" is the pass's wall time times its threads over its variants; "Against alone" divides the checked backtests' mean time by it. Memory is the process's peak resident set (`/usr/bin/time -f %M`).

| Rows | Grid |
|---|---|
| BTCUSDT `basic_mm` | `half_spread_bps` 0.007 to 2, `quote_qty` 0.002, 0.004, `requote_threshold_ticks` 1, 3, latency 2 to 20 ms |
| BTCUSDT `avellaneda_stoikov` | `gamma` 0.01 to 10, `kappa` 0.5 to 15, `quote_qty` 0.002, 0.004, latency 5, 20 ms, `max_inventory` 0.02 and 1000 |
| BTCUSDT and Bybit `xmm` | `edge_bps` 0 to 2, `slippage_bps` 0, 0.5, `quote_qty` 0.002, 0.004, `requote_threshold_ticks` 1, 3, `basis_halflife_s` 30, 120, latency 2 to 20 ms |
| BTCUSDT `fastmm init` strategy | `half_spread_bps` 0.007 to 2, `quote_qty` 0.002, 0.004, `requote_ticks` 1, 3, latency 2 to 20 ms, `max_inventory` 0.02, `skew_bps` 2 |
| ITCH `basic_mm` | `half_spread_bps` 0.5 to 4, `requote_threshold_ticks` 1, 3, `quote_qty` 100, 200, latency 20 µs to 1 ms |
| ITCH `avellaneda_stoikov` | `kappa` 10 to 1000, latency 50 µs, 1 ms |
| Threads | the BTCUSDT `basic_mm` grid with `skew_bps_per_unit` 1 to 8 and `levels` 1 to 4 |

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

`--own-feed 0` takes our orders out of the feed; `--strategy avellaneda_stoikov` runs that strategy from its defaults under the `--sweep` values. Thousands of points runs the last command with the first 1, 8, 64, 256 and 1,024 values of `skew_bps_per_unit`. Threads runs the third command with `--threads` 1 to 64, and the engine rows of GPUs the second with `--sweep skew_bps_per_unit=0.5,1,2,4 --sweep max_inventory=500,1000,2000,5000 --variants 1024` on 8 and 64 threads. The `fastmm init` row times `fastmm.sweep(cfg, grid, data=..., strategy=Mm, threads=1, summary_only=True)` against `fastmm.run_backtest` of 2 of its points, which gave the same outbound hashes. In the same run `basic_mm` took 97 ms a variant, and the `fastmm init` strategy quoted with 7 % fewer fills.

Against 0.9.0, where `fastmm.sweep` ran each point as a backtest of its own on at most 32 threads, the Threads grid with `max_inventory` 0.02, 0.04 in place of latency took 476 s against 30 s on the desktop (8 threads), and 145 s against 5.8 s on the server (64 threads). 0.9.0 ran through `pip install "fastmm-engine==0.9.0"` and `fastmm.sweep(fastmm.BacktestConfig.from_toml("configs/backtest-binance.toml"), grid, data="tardis:binance-futures,BTCUSDT,2026-09-01,end=04:00", threads=64)` with that release's configuration.

The GPU kernel is on the branch `research/gpu-lanes`. It runs `basic_mm` under `l3_queue` with one level a side, fixed latencies, our orders not in the feed and no risk limit but self-trade prevention. Its grid is `half_spread_bps` 0.25 to 8, `skew_bps_per_unit` 0 to 2, `quote_qty` 100 to 500, `max_inventory` 300 to 2,000 and none, `requote_threshold_ticks` 1 to 3 and latency 20 to 500 µs over the ITCH day. `fastmm-bench-population` runs the lanes on the CPU and writes a tape and the CPU's outputs; the GPU driver compares fills, cash, fees, position and a hash of the fills with them. The GPU times leave out the 4.2 to 4.6 s the tape takes to build on one core. It needs the CUDA toolkit:

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

`--lanes 8192` gives the larger population.

## Queue models against ITCH

The results are on [Backtesting](../docs/explanation/backtesting.md#the-queue-from-every-order). `fastmm-bench-queue-truth FILE` follows each order that joins the touch, or the level behind it, in a TotalView-ITCH day. It runs the `l2_queue` rule on the L2 view of the same messages over the order's life, and the `l3_queue` rule on the full view, and compares each with what ITCH executed against the order. A day of the 100 most active stocks takes about 35 s.

```bash
build/release/bin/bench/fastmm-bench-queue-truth day.itch --top 100
```

`l3_queue` takes an execution at a price better than ours as one that never reached us: an order that would lock the market is displayed a tick away and executes at the locking price. Without that rule, 215,000 executions of later orders on 2019-07-30 looked like queue jumps.
