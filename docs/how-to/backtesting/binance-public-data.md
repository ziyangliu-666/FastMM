# Backtest on real BTCUSDT data

Backtest a shipped strategy on a real day of Binance BTCUSDT perpetual data, at the venue's fees.

## 1. Build

```bash
./scripts/bootstrap.sh          # dependencies, once
cmake --preset release && cmake --build build/release -j
```

## 2. Fetch a day

```bash
python3 python/fastmm/data/__main__.py fetch --symbol BTCUSDT --date 2024-03-27
```

It needs only the Python standard library, so it runs before the bindings are built. With the wheel installed it is `python3 -m fastmm.data fetch --symbol BTCUSDT --date 2024-03-27`.

It downloads `bookTicker` and `aggTrades` from [data.binance.vision](https://data.binance.vision), checks each file against the SHA-256 the archive publishes next to it, and unpacks them into `$FASTMM_DATA_HOME` (default `~/.cache/fastmm/data`). A partial download resumes. One day of BTCUSDT is 207 MB of ZIP and 1.5 GB unpacked; `python3 -m fastmm.data ls` shows what the cache holds.

Pick a date between 2023-05-16 and 2024-03-30. The archive published futures `bookTicker` only in that window and nothing since, so those are the days with a book. Trades run to yesterday.

## 3. Run

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data binance:BTCUSDT,2024-03-27
```

The day is 18 million market-data events. To try an hour first, add `,start=12:00,end=13:00`.

`configs/backtest-binance.toml` runs `basic_mm` quoting one level at the touch, with BTCUSDT perpetual's real tick (0.10 USDT), step (0.001 BTC) and minimum notional (100 USDT), and Binance USDⓈ-M VIP 0 fees: 0.0200 % maker and 0.0500 % taker, which is 2 bps and 5 bps.

## 4. Pack it for repeated runs

```bash
./build/release/bin/fastmm-data convert --config configs/backtest-binance.toml \
    --data binance:BTCUSDT,2024-03-27 --out ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
```

The journal holds the same events already decoded. It gives byte-identical results and runs several times faster: 3 s against 14 s for this day on a WSL2 desktop ([Journal format](../../reference/journal-format.md)).

## From Python

```python
import fastmm
from fastmm.data import binance

binance.fetch("BTCUSDT", ["2024-03-27"])
cfg = fastmm.BacktestConfig.from_toml("configs/backtest-binance.toml")
r = fastmm.run_backtest(cfg, data="binance:BTCUSDT,2024-03-27")
print(r.summary_table())
print(fastmm.markout_frame(r))
```

## What came out

The run above, `basic_mm` on BTCUSDT perpetual, 2024-03-27, `l2_queue` fills with `queue_conservatism = 1.0`, 5 ms round trip:

```text
  net pnl                        -499.0521
  realized / unrealized / fees   -148.8768 / 0.9549 / 351.1302
  fills (maker / taker)          12529 (12529 / 0)
  spread captured (bps)          0.032
  fills at touch / behind / through 70.9% / 12.0% / 17.1%
  volume base / quote            24.99800 / 1755650.99

where the PnL came from (quote currency, 1755650.99 traded notional)
  gross spread capture                 5.6225  +0.032 bps, mid at the fill
  mid drift after the fills         -153.5444  adverse selection + the open inventory
  fees paid                         -351.1302
  = net                             -499.0521

markout per fill (bps of notional)
  horizon     markout     mo bps    capture    cap bps    adv.sel    fills
  1s        -116.4704    -0.6634     5.6225     0.0320     0.6954    12529
  10s       -149.8871    -0.8537     5.6225     0.0320     0.8858    12529
  1m        -149.0135    -0.8488     5.6225     0.0320     0.8808    12529
```

The strategy loses 499 USDT on 1.76 M USDT traded, and the decomposition says where: it captured 0.032 bps of spread and paid 2 bps of maker fee. No queue position closes that gap: BTCUSDT perpetual's spread is one tick, 0.1 USDT on a 70,000 USDT mid, which is 0.014 bps, a fourteenth of the fee.

The markouts say the fills were not worth having either: the mid moved 0.66 bps against each one within a second and 0.85 bps within ten. Buys and sells lose about equally, so this is adverse selection on both sides, not a directional bet gone wrong. A strategy that quoted for free would still lose 0.85 bps per fill.

Quote uptime is 36.9 % and 5,796 of 70,301 orders are rejected, nearly all of them `PostOnlyWouldCross`. That is the spread again: with a one-tick market and a 5 ms round trip, the touch has usually moved by the time an order arrives, and a post-only order that would take liquidity is rejected rather than filled. Raise `latency_fixed_us` and it gets worse; that number is a property of the venue and the link, not of the simulator.

The synthetic market cannot produce this number: its flow is a coin flip, so its markouts measure the queue and the latency and nothing else ([Backtesting](../../explanation/backtesting.md#what-the-simulator-cannot-tell-you)). Here the flow is real, and it costs 0.85 bps a fill.

## What this does not tell you

[Backtesting](../../explanation/backtesting.md) lists the simulator's assumptions: no market impact, and a queue model instead of counterparties. The limits below belong to this data.

**The feed is the top of book, and nothing deeper.** `bookTicker` publishes the best bid and ask; the archive has no depth file that can be replayed. The queue model sets an order's position from the displayed quantity at its price, and at any price except the touch that quantity is zero as far as this feed is concerned. So an order resting behind the touch is modelled as alone at its price and fills the moment a trade reaches it. In the run above 12 % of the fills are behind the touch, and those are the optimistic ones. `configs/backtest-binance.toml` quotes one level at the touch for exactly this reason; a multi-level version of the same strategy measured on this data would be fiction.

For depth, use [`tardis`](../../reference/data-sources.md#tardis), whose `incremental_book_L2` is the whole book:

```bash
python3 python/fastmm/data/__main__.py fetch --source tardis \
    --exchange binance-futures --symbol BTCUSDT --date 2026-09-01
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data tardis:binance-futures,BTCUSDT,2026-09-01
```

That is a different day, so it is not a controlled comparison, but it says the same thing: 1,771 maker fills, markouts of −0.65 bps at 1 s and −0.87 bps at 10 s, a *negative* spread capture of −0.036 bps, and 50.6 USDT of fees against 71.0 USDT of loss. One day of BTCUSDT L2 is 748 MB compressed and 10.9 GB unpacked, and the run takes 28 seconds.

**Both clocks are milliseconds.** `bookTicker` and `aggTrades` both stamp the venue's transaction time to the millisecond, so a trade and the book update it caused usually carry the same timestamp and their true order is not recoverable. The merge puts the trade first, which consumes the queue before the level is recorded as smaller. With `queue_conservatism = 1.0` the other order would give the same answer; at lower conservatism it would not, and the run would be slightly optimistic.

**Your orders are not in it.** No backtest on recorded data can be: the flow was generated by a market that never saw your quotes. Everything here is an upper bound on what the strategy would have earned, and the markouts are a floor on the adverse selection it would have paid.

**One day is one day.** 2024-03-27 was an ordinary day. A result from a single session says nothing about a different volatility regime; fetch a range with `--from` and `--to` and pass `binance:BTCUSDT,from=2024-03-25,to=2024-03-29`.

## Licence

The datasets are [CC BY-NC-SA 4.0](https://data.binance.vision/Binance_Vision-Terms_of_Use.pdf). Their terms allow "algorithmic historical backtesting for purely personal non-production research" and forbid "live proprietary trading execution"; a redistributed derivative must keep the same licence and credit Binance Vision. No sample is committed to this repository for that reason; fetch your own ([Market-data sources](../../reference/data-sources.md#binance)).
