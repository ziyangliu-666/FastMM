# Judging a signal before writing a strategy

A candidate predictor can be measured in seconds, before any strategy or fill model exists. `fastmm.features()` replays a market-data source through the engine's book and writes one row per book update. `fastmm.evaluate_signal()` turns that table into the numbers a passive quote is decided on.

```python
t = fastmm.features(data="binance:BTCUSDT,2024-03-27", horizons=[0.1, 1, 10, 60])
print(fastmm.evaluate_signal(t, "imbalance")["table"])
```

The C++ entry points are `fastmm::research::extract_features()` and `fastmm::research::evaluate_signal()`. In Python the columns come back as read-only numpy views onto the same memory.

## What a row is

A row is written after each book update that leaves the book with two sides. It holds the touch, the mid, the microprice, the top-of-book imbalance, the spread and the mid at each forward horizon.

| Column kind | Unit |
|---|---|
| prices, quantities, spreads | raw 1e-8 fixed-point integers |
| imbalance | same scale: 100000000 is 1.0 |
| forward mid | venue mid at `ts + h`, or 0 when unset |

The book is the one the simulated venue keeps under the `l2_queue` fill model: book messages move it, trades do not. A row's mid is therefore the mid a backtest marks its fills against at that time.

| Option | Effect |
|---|---|
| `subsample=s` | at most one row per `s` seconds of event time |
| `sample_trades=True` | adds a row at every trade, with the book as it stood when the trade printed |
| `sample_book_updates=False` | drops the book-update rows; with `sample_trades` the table is one row per trade, where a fill from a trade lands |

A resting order that the book comes to fills at that book update instead ([Backtesting](backtesting.md#model-assumptions)).

## Forward mids, and what unset means

A forward mid is never filled in with the last known mid. It is read at `ts + h` from the book as it stood then, in one pass with no look-ahead. Two cases leave it unset, and `coverage` counts each:

| Case | Counted under |
|---|---|
| `ts + h` is past the last event of the data | `excluded_past_end` |
| the book has one side at `ts + h` | `excluded_no_mid` |

Fill markouts in a backtest follow the same rule ([Markouts](backtesting.md#markouts)).

## What the evaluation reports

Per horizon, over the rows that have a forward mid, `evaluate_signal()` reports three things:

| Output | What it says |
|---|---|
| information coefficient | Spearman rank correlation of the signal with the forward mid move, for the whole data and per contiguous block (`blocks=`), so one hour cannot carry the day |
| bucket table | mean forward mid move in bps of the mid, by signal bucket of equal row count (ten buckets is a decile table) |
| conditional touch markout | what a quote resting at the touch is worth in each bucket |

The touch markout decides whether a passive quote is viable. For a row with mid `m`, best bid `b`, best ask `a` and forward mid `f`:

```text
buy_bps  = (f - b) / m * 1e4      a resting bid that filled at b, marked at f
sell_bps = (a - f) / m * 1e4      a resting ask that filled at a, marked at f
```

Each side is the half spread plus or minus the move, so the two sides average to the half spread whatever the signal says. Conditioning makes the split informative, because inside one bucket the trade mostly comes to one side. The numbers are gross of fees: subtract the venue's maker rate per side.

The signal is either a built-in feature (`imbalance`, `microprice_edge_bps`, `spread_bps`) or a float64 array with one value per row. An out-of-tree predictor is measured the same way.

## BTCUSDT perpetual, 2024-03-27

One day of Binance USD-M public data gives 18,067,861 events and 15,814,430 book updates, one row each, extracted in 9.7 s. The median spread is 0.0143 bps: one tick of 0.10 USDT on a mid near 70,000 USDT. The mean half spread is 0.0162 bps.

No row landed on a one-sided book. Rows whose horizon ran past the end of the day are left out of that horizon:

| Horizon | 100 ms | 1 s | 10 s | 1 min |
|---|---:|---:|---:|---:|
| Rows past the end | 4 | 38 | 1,348 | 9,934 |

### The known effect

Imbalance predicts the next mid move, and the table shows it. Deciles of 1,581,442 rows each, forward mid move in bps of the mid:

| decile | mean imbalance | 100 ms | 1 s | 10 s | 1 min |
|---|---|---|---|---|---|
| 1 | -0.997 | -0.237 | -0.564 | -0.823 | -0.887 |
| 2 | -0.974 | -0.172 | -0.471 | -0.731 | -0.747 |
| 3 | -0.881 | -0.118 | -0.335 | -0.566 | -0.663 |
| 4 | -0.647 | -0.074 | -0.209 | -0.324 | -0.517 |
| 5 | -0.254 | -0.025 | -0.071 | -0.161 | -0.404 |
| 6 | +0.213 | +0.025 | +0.049 | -0.004 | -0.183 |
| 7 | +0.629 | +0.077 | +0.198 | +0.278 | +0.204 |
| 8 | +0.876 | +0.121 | +0.336 | +0.521 | +0.442 |
| 9 | +0.972 | +0.175 | +0.472 | +0.720 | +0.526 |
| 10 | +0.997 | +0.240 | +0.565 | +0.862 | +0.590 |

The table is monotone at 100 ms, 1 s and 10 s, and at 1 minute apart from deciles 6 to 7. It straddles zero at all four horizons.

| | 100 ms | 1 s | 10 s | 1 min |
|---|---:|---:|---:|---:|
| IC, imbalance | +0.580 | +0.415 | +0.159 | +0.060 |
| IC, microprice edge | +0.568 | +0.400 | +0.153 | +0.057 |
| Hourly blocks with the day's sign, imbalance | 24 of 24 | 24 of 24 | 24 of 24 | 23 of 24 |

The microprice edge is `(microprice - mid) / mid`. On a feed one level deep it is the imbalance rescaled by the spread, so the two rows measure one signal twice.

### The cross-check against the backtest

The extractor's forward mids match the backtest's markout mids to the raw integer. `basic_mm` with [`configs/backtest-binance.toml`](../../configs/backtest-binance.toml) on the same day made 12,925 fills, all maker:

| compared | fills | mismatches |
|---|---|---|
| mid at fill + 1 s | 12 925 | 0 |
| mid at fill + 10 s | 12 925 | 0 |
| mid at fill + 1 min | 12 925 | 0 |

The same comparison, plus a no-look-ahead test at `ts + h - 1 ns` against `ts + h`, runs on every build. So a number from the extractor and a number from a backtest describe the same fills.

### Was a quote at the touch adversely selected

Not on average. Over every book update of the day, a quote resting at the touch is worth, gross of fees:

| horizon | half spread | resting bid | resting ask | mean absolute move |
|---|---|---|---|---|
| 100 ms | 0.0162 bps | +0.0173 bps | +0.0151 bps | 0.21 bps |
| 1 s | 0.0162 bps | +0.0133 bps | +0.0191 bps | 0.93 bps |
| 10 s | 0.0162 bps | -0.0065 bps | +0.0389 bps | 3.46 bps |
| 1 min | 0.0162 bps | -0.1477 bps | +0.1801 bps | 8.11 bps |

The 2,253,431 timestamps where a trade printed, where a fill can happen, give the same picture. At 1 s the resting bid is worth +0.0248 bps and the resting ask +0.0674 bps, against a mean half spread of 0.0461 bps.

The fills `basic_mm` got are worth far less: -0.653 bps at 1 s, -0.820 bps at 10 s and -0.823 bps at 1 minute. The adverse selection is a property of which fills a quote gets, not of the touch.

Conditioning on imbalance shows which fills those are. Deciles over trade rows, 225,343 rows each, 1 s horizon:

| decile | mean imbalance | resting bid | resting ask |
|---|---|---|---|
| 1 | -0.995 | -0.915 bps | +0.972 bps |
| 2 | -0.961 | -0.749 bps | +0.831 bps |
| 3 | -0.837 | -0.490 bps | +0.581 bps |
| 4 | -0.583 | -0.232 bps | +0.321 bps |
| 5 | -0.219 | -0.329 bps | +0.452 bps |
| 6 | +0.179 | +0.203 bps | -0.085 bps |
| 7 | +0.558 | +0.541 bps | -0.438 bps |
| 8 | +0.829 | +0.505 bps | -0.403 bps |
| 9 | +0.956 | +0.900 bps | -0.802 bps |
| 10 | +0.995 | +0.815 bps | -0.755 bps |

In the most ask-heavy decile the trade comes to the bid, which is worth -0.915 bps. In the most bid-heavy decile it lifts the ask, worth -0.755 bps. A symmetric quoter takes the negative side in every bucket, so its realised -0.653 bps sits inside this range, far from the +0.03 bps average.

Conditioning identifies the bad side, but not whether the good side can be had. The touch markout prices a fill given the state, not the chance of getting it. The best bucket of either signal, on the side the flow moves away from:

| horizon | imbalance | microprice edge |
|---|---|---|
| 1 s | +0.97 bps | +1.11 bps |
| 10 s | +1.34 bps | +1.96 bps |
| 1 min | +1.67 bps | +3.59 bps |

The Binance USD-M VIP 0 maker rate is 2 bps per fill. At 1 s and 10 s, the horizons a touch quote lives on here, nothing clears it.

One bucket does clear it: the top microprice-edge decile, a minute out, at +3.59 bps for a resting bid. That decile is the wide-spread tail, not a sharp signal. Its mean edge is 0.14 bps against 0.007 bps in the eight middle deciles, and its mean half spread 0.206 bps against 0.008 bps. A one-level feed quantises the microprice to a fraction of a tick, so only the tails vary.

That bid also rests under a book about to trade up, the fill a quoter is least likely to get. And a minute is longer than this configuration's inventory limits tolerate.

### Subsampling

`subsample=0.01` keeps one row per 10 ms: 2,981,482 of the 15,814,430 rows. The sign and the monotonicity survive, but magnitudes shift by up to 0.06 bps, because a clock reweights the day towards its quiet moments. The tables above use every update.

| | Every update | `subsample=0.01` |
|---|---:|---:|
| 1 s decile table, lowest to highest | -0.564 to +0.565 bps | -0.503 to +0.492 bps |
| 1 s information coefficient | +0.415 | +0.382 |

## Where basic_mm's PnL goes on this day

`basic_mm` on this day nets -501 USDT on 1,769,825 USDT of traded notional:

| Part | USDT |
|---|---:|
| gross spread capture | +4.69 |
| mid drift after the fills | -151.26 |
| fees | -353.97 |

The capture is 0.026 bps because that is what the touch is worth. The mean half spread at a trade is 0.046 bps, and `half_spread_bps = 0.007` puts the quote at the touch. 58.3 % of fills are at the venue's best price on their side; 30.0 % came when the other side of the book reached the quote.

No touch quote on this instrument captures more than a few hundredths of a basis point. The markout is -0.65 bps at 1 s because the fills come from the buckets where the quote is on the wrong side of the imbalance. Both facts are visible in the extractor without running the strategy.

The gap is not a tuning problem. A signal, the third route out in [Economics of the shipped strategies](economics.md#the-arithmetic-you-have-to-beat), does not close it at this holding period. Under three hundredths of a basis point of capture cannot pay a 2 bps fee, and the best conditional fill at 1 s and 10 s is worth less than the fee.

This measurement does not settle whether a fair-value offset helps at longer horizons. A skewed quote changes which fills it gets, which is a fill-model question. On a top-of-book feed the `l2_queue` fill counts are a guess ([Backtesting](backtesting.md#the-queue-from-every-order)), so settling it needs a feed with depth.

## Reproducing the numbers

```bash
python3 -m fastmm.data fetch --symbol BTCUSDT --date 2024-03-27
```

```python
t = fastmm.features(data="binance:BTCUSDT,2024-03-27", horizons=[0.1, 1, 10, 60])
print(fastmm.evaluate_signal(t, "imbalance", blocks=24)["table"])
```

The trade-row tables need one row per trade. The cross-check takes the book rows as well (`sample_book_updates=True`):

```python
f = fastmm.features(data="binance:BTCUSDT,2024-03-27", horizons=[1, 10, 60],
                    sample_book_updates=False, sample_trades=True)
```

The day is 1.5 GB of `bookTicker` CSV. The full table holds about 1.8 GB of columns, and ranking needs about as much again.
