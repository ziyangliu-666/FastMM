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

The script needs only the Python standard library, so it runs before the bindings are built. With the wheel installed, run `python3 -m fastmm.data fetch --symbol BTCUSDT --date 2024-03-27`.

It downloads `bookTicker` and `aggTrades` from [data.binance.vision](https://data.binance.vision), checks each file against the SHA-256 the archive publishes, and unpacks it into `$FASTMM_DATA_HOME` (default `~/.cache/fastmm/data`). A partial download resumes, and a ZIP is deleted once unpacked.

| | |
|---|---|
| dates with a book | 2023-05-16 to 2024-03-30, the only window with futures `bookTicker`; trades run to yesterday |
| one day of BTCUSDT | 207 MB download, 1.5 GB unpacked |
| what the cache holds | `python3 -m fastmm.data ls` |

## 3. Run

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data binance:BTCUSDT,2024-03-27
```

The day is 18 million market-data events. To try an hour first, add `,start=12:00,end=13:00`.

`configs/backtest-binance.toml` runs `basic_mm` quoting one level at the touch, with the instrument's real terms:

| Setting | Value |
|---|---|
| tick | 0.10 USDT |
| step | 0.001 BTC |
| minimum notional | 100 USDT |
| fees, Binance USDⓈ-M VIP 0 | 2 bps maker, 5 bps taker |

## 4. Pack it for repeated runs

```bash
./build/release/bin/fastmm-data convert --config configs/backtest-binance.toml \
    --data binance:BTCUSDT,2024-03-27 --out ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data ~/.cache/fastmm/data/btcusdt-2024-03-27.fmj
```

The journal holds the same events already decoded ([Journal format](../../reference/journal-format.md)). It gives byte-identical results and runs this day in 3 s instead of 5 s on a desktop.

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
  maker fills at / inside / behind / through 70.9% / 3.7% / 8.2% / 17.1%
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

The kill switch stops quoting at 09:37 UTC, when the loss reaches `[risk] max_loss` (500 USDT); the log shows `kill switch engaged (MaxLoss ...)`. The numbers cover those hours.

| What the numbers say | Value |
|---|---|
| loss | 499 USDT on 1.76 M USDT traded |
| spread captured | 0.032 bps, against a maker fee of 2 bps |
| BTCUSDT perpetual's spread | one tick, 0.1 USDT on a 70,000 USDT mid: 0.014 bps |
| mid move against each fill | 0.66 bps within 1 s, 0.85 bps within 10 s |
| orders rejected | 5,796 of 70,301, nearly all `PostOnlyWouldCross` |

The fee is the first problem: no queue position earns 2 bps from a spread of 0.014 bps.

The fills are not worth having either. Buys and sells lose about equally, so this is adverse selection on both sides, not a directional bet. A strategy that quoted for free would still lose 0.85 bps per fill.

The rejects are the one-tick spread again. With a 5 ms round trip, the touch has usually moved by the time an order arrives, and a post-only order that would take liquidity is rejected. A higher `latency_fixed_us` makes it worse; that number belongs to the venue and the link.

The synthetic market cannot produce this result. Its flow is a coin flip, so its markouts measure the queue and the latency only ([Backtesting](../../explanation/backtesting.md#model-assumptions)). Here the flow is real, and it costs 0.85 bps a fill.

## Properties of this data

[Backtesting](../../explanation/backtesting.md) lists the simulator's assumptions: no market impact, and a queue model instead of counterparties. This data adds four of its own.

| Property | Effect | What to do |
|---|---|---|
| top of book only | an order behind the touch is modelled as alone at its price and fills as soon as a trade reaches it | quote at the touch; measure multi-level strategies on depth data |
| millisecond clocks | a trade and the book update it caused share a timestamp; the merge puts the trade first | keep `queue_conservatism = 1.0`, where the order does not change the answer |
| your orders are absent | the market never saw your quotes | read the PnL as an upper bound, the markouts as a lower bound on adverse selection |
| one day | 2024-03-27 was an ordinary day | fetch a range with `--from` and `--to`, then pass `binance:BTCUSDT,from=2024-03-25,to=2024-03-29` |

### Top of book only

`bookTicker` publishes the best bid and ask, and the archive has no depth file that can be replayed. The queue model takes an order's position from the displayed quantity at its price, which this feed shows only at the touch.

In the run above 8 % of the fills are behind the touch, and those are the optimistic ones. The 4 % inside it improved the best price, where the order really was alone. `configs/backtest-binance.toml` quotes one level at the touch for that reason.

For depth, use [`tardis`](../../reference/data-sources.md#tardis), whose `incremental_book_L2` is the whole book:

```bash
python3 python/fastmm/data/__main__.py fetch --source tardis \
    --exchange binance-futures --symbol BTCUSDT --date 2026-09-01
./build/release/bin/fastmm-backtest --config configs/backtest-binance.toml \
    --data tardis:binance-futures,BTCUSDT,2026-09-01
```

| Tardis, 2026-09-01 | Value |
|---|---|
| maker fills | 1,771 |
| markout | -0.65 bps at 1 s, -0.87 bps at 10 s |
| spread capture | -0.036 bps |
| fees / loss | 50.6 USDT / 71.0 USDT |
| one day of BTCUSDT L2 | 748 MB compressed, 10.9 GB unpacked, 28 s to run |

It is a different day, so not a controlled comparison, but it says the same thing.

### Millisecond clocks

`bookTicker` and `aggTrades` both stamp the venue's transaction time to the millisecond, so a trade and the book update it caused usually carry the same timestamp. The merge puts the trade first, which consumes the queue before the level is recorded as smaller. With `queue_conservatism = 1.0` the other order gives the same answer; at lower conservatism the run is slightly optimistic.

## Licence

The datasets are [CC BY-NC-SA 4.0](https://data.binance.vision/Binance_Vision-Terms_of_Use.pdf). Their terms allow "algorithmic historical backtesting for purely personal non-production research" and forbid "live proprietary trading execution"; a redistributed derivative must keep the same licence and credit Binance Vision. No sample is committed to this repository for that reason; fetch your own ([Market-data sources](../../reference/data-sources.md#binance)).
