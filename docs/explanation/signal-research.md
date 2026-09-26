# Judging a signal before writing a strategy

`fastmm.features()` drives a market-data source into the engine's own `L2Book` and writes one row per book update: the touch, the mid, the microprice, the top-of-book imbalance, the spread, and the mid at a set of forward horizons. `fastmm.evaluate_signal()` turns that table into the numbers a passive quote is decided on. Neither runs a strategy or a fill model, so a candidate predictor is measured in seconds instead of a backtest.

```python
t = fastmm.features(data="binance:BTCUSDT,2024-03-27", horizons=[0.1, 1, 10, 60])
print(fastmm.evaluate_signal(t, "imbalance")["table"])
```

The C++ entry points are `fastmm::research::extract_features()` and `fastmm::research::evaluate_signal()` in `include/fastmm/research/`; the Python functions are wrappers that hand back the columns as read-only numpy views onto the same memory.

## What a row is

A row is emitted after a `BookDelta` or `BookSnapshot` has been applied, when the book has two sides. Prices, quantities and spreads are raw 1e-8 fixed-point integers, the convention of `backtest/result.hpp`; imbalance is in the same scale, so 100000000 is 1.0. Columns are structure of arrays: one contiguous vector each, which is what lets Python borrow them without a copy.

`subsample` keeps at most one row per interval of event time. `sample_trades` adds a row at every trade, with the book as it stood when the trade printed; with `sample_book_updates=False` the table then holds exactly one row per trade, which is the timestamp every fill of a backtest over the same data lands on.

The book is the one the simulated venue keeps under the `l2_queue` fill model (`queue_on_delta` in `src/sim/sim_transport.cpp`): book messages are applied, trades are not. So a row's mid is the number `SimTransport::venue_mid()` reports at that time, and the two paths can be compared fill by fill.

## Forward mids, and what unset means

The forward mid of a row at horizon h is the venue mid at `ts + h`, read at that time. One pass resolves them without look-ahead: on an event at time t, every row whose `ts + h` is strictly before t is resolved from the book as it stands before that event is applied, and at the end of the data every row whose `ts + h` is at or before the last event time is resolved from the final book. A row whose `ts + h` is past the last event has no mid to read, so its column holds 0 and `coverage` counts it under `excluded_past_end`; a row whose horizon lands where the book has one side is counted under `excluded_no_mid`. Neither is ever filled in with the last known mid. This is the discipline `backtest/markout.hpp` applies to fill markouts, applied per row.

## What the evaluation reports

Per horizon, over the rows that have a forward mid:

- the information coefficient, the Spearman rank correlation between the signal and the forward mid move, and the same coefficient recomputed over contiguous equal-sized blocks, which is what says whether one hour carried the day;
- a table of the forward mid move by signal bucket, in basis points of the mid, with equal row counts per bucket (ten of them is a decile table);
- the conditional touch markout.

The touch markout decides whether a passive quote is viable. For a row with mid `m`, best bid `b`, best ask `a` and forward mid `f`:

```text
buy_bps  = (f - b) / m * 1e4      a resting bid that filled at b, marked at f
sell_bps = (a - f) / m * 1e4      a resting ask that filled at a, marked at f
```

Both split into the half spread `(m - b) / m` plus or minus the move `(f - m) / m`, so the two sides of one quote average to the half spread whatever the signal says. Conditioning makes the split informative: inside a bucket, a trade mostly comes to one side. The numbers are gross of fees; subtract the venue's maker rate per side to compare.

`evaluate_signal(table, values)` takes either the name of a built-in feature (`imbalance`, `microprice_edge_bps`, `spread_bps`) or a float64 array with one value per row, so an out-of-tree predictor is measured the same way.

## What the extractor is checked against

1. The known effect. Forward mid move bucketed by top-of-book imbalance must be monotone in imbalance and straddle zero on a real day. A flat table would mean the extractor is wrong.
2. The existing path. `basic_mm` under the `l2_queue` fill model on the same day, and for every fill the extractor's forward mid at that timestamp against the markout mid the runner recorded. Any mismatch is a clock or a look-ahead bug in one of the two. The same comparison runs on every build as `research.cross_check` (`tests/research/cross_check_test.cpp`) and in `python/tests/test_features.py`.
3. No look-ahead, mechanically, in the shape of `tests/backtest/markout_test.cpp`: the same row and the same horizon on two tapes, one ending at `ts + h - 1 ns` and one ending at `ts + h`. The first leaves the forward mid unset and counts it under `excluded_past_end`; the second resolves it.
4. The question the extractor was built for: was a quote resting at the touch adversely selected on 2024-03-27, and does conditioning on imbalance or the microprice change that.

## BTCUSDT perpetual, 2024-03-27

Binance USD-M public data, one day: 18 067 861 events, 15 814 430 book updates, one row each, extracted in 9.7 s. No row landed on a one-sided book. Rows whose horizon ran past the end of the day, and which are therefore excluded from every statistic of that horizon: 4 at 100 ms, 38 at 1 s, 1348 at 10 s, 9934 at 1 minute.

The median spread is 0.0143 bps, one tick of 0.10 USDT on a mid near 70 000; the mean half spread is 0.0162 bps.

### The known effect

Imbalance deciles, 1 581 442 rows each, forward mid move in basis points of the mid:

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

Monotone at 100 ms, 1 s and 10 s, monotone apart from decile 6 to 7 at 1 minute, and straddling zero at all four. The information coefficient is +0.580, +0.415, +0.159 and +0.060; over 24 hourly blocks every block carries the sign of the whole day at 100 ms, 1 s and 10 s, and 23 of 24 at 1 minute. The microprice edge, `(microprice - mid) / mid`, says the same thing slightly more weakly: +0.568, +0.400, +0.153 and +0.057. On a feed one level deep the microprice is the imbalance rescaled by the spread, so this is one signal measured twice.

### The cross-check against the backtest

`basic_mm` with [`configs/backtest-binance.toml`](../reference/configuration.md) over the same day: 12 529 fills, all maker. Against a table of trade rows over the same source:

| compared | fills | mismatches |
|---|---|---|
| mid at the fill | 12 529 | 0 |
| best bid, best ask at the fill | 12 529 | 0 |
| mid at fill + 1 s | 12 529 | 0 |
| mid at fill + 10 s | 12 529 | 0 |
| mid at fill + 1 min | 12 529 | 0 |

Every number matches to the raw integer.

### Was a quote at the touch adversely selected

Not on average. Over every book update of the day, a quote resting at the touch is worth, gross of fees:

| horizon | half spread | resting bid | resting ask | mean absolute move |
|---|---|---|---|---|
| 100 ms | 0.0162 bps | +0.0173 bps | +0.0151 bps | 0.21 bps |
| 1 s | 0.0162 bps | +0.0133 bps | +0.0191 bps | 0.93 bps |
| 10 s | 0.0162 bps | -0.0065 bps | +0.0389 bps | 3.46 bps |
| 1 min | 0.0162 bps | -0.1477 bps | +0.1801 bps | 8.11 bps |

Restricted to the 2 253 431 timestamps where a trade printed, which is where a fill can actually happen, it is the same picture: at 1 s the resting bid is worth +0.0248 bps and the resting ask +0.0674 bps, against a mean half spread of 0.0461 bps.

The fills `basic_mm` got are worth -0.663 bps at 1 s, -0.854 bps at 10 s and -0.849 bps at 1 minute. The adverse selection is therefore not a property of the touch; it is a property of which fills a quote gets.

Conditioning on imbalance shows it. Imbalance deciles over trade rows, 225 343 rows each, 1 s horizon:

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

In the most ask-heavy decile the trade comes to the bid, and that bid is worth -0.915 bps. In the most bid-heavy decile the trade lifts the ask, and that ask is worth -0.755 bps. A symmetric quoter takes the negative number in every bucket, which is why the realised -0.663 bps sits inside this range and not near the +0.03 bps average.

Conditioning identifies the bad side. Whether it produces a usable one is beyond this measurement: the touch markout is what a fill is worth given the state, not the chance of getting that fill. The best bucket of either signal, on the side the flow is moving away from, is:

| horizon | imbalance | microprice edge |
|---|---|---|
| 1 s | +0.97 bps | +1.11 bps |
| 10 s | +1.34 bps | +1.96 bps |
| 1 min | +1.67 bps | +3.59 bps |

The Binance USD-M VIP-0 maker rate is 2 bps per fill. At 1 s and 10 s, the horizons a touch quote lives on here, nothing clears it. One bucket does: the top decile of the microprice edge, marked a minute later, is worth +3.59 bps to a resting bid. That decile is the wide-spread tail rather than a sharp signal: its mean edge is 0.14 bps against 0.007 bps for the eight middle deciles, and its mean half spread is 0.206 bps against 0.008 bps, because a one-level-deep feed quantises the microprice to a fraction of a tick and leaves only the tails free to vary. It is also a bid resting under a book that is about to trade up, which is the fill a quoter is least likely to get, and a minute is longer than the inventory limits in this configuration tolerate.

### Subsampling

`subsample=0.01` keeps one row per 10 ms and leaves 2 981 482 of the 15 814 430 rows. The sign and the monotonicity survive: the 1 s decile table runs from -0.503 to +0.492 bps against -0.564 to +0.565, and the information coefficient is +0.382 against +0.415. The magnitudes shift by up to 0.06 bps, because sampling on a clock reweights the day towards its quiet moments; the table above uses every update.

## Why the shipped strategy loses money

`basic_mm` on this day nets -499 USDT: 5.62 USDT of gross spread capture, -153.54 of mid drift after the fills, and 351.13 of fees on 1 755 651 USDT of traded notional.

The capture is 0.032 bps because that is what the touch is worth. The mean half spread at a trade is 0.046 bps, and the strategy's `half_spread_bps = 0.007` puts its quote at the touch (70.9% of fills are at the venue's best price on its side). There is no configuration of a touch quote that captures more than a few hundredths of a basis point on this instrument.

The markout is -0.66 bps at 1 s because the fills are drawn from the buckets where the quote is on the wrong side of the imbalance, not from the day's average state. Both are visible in the extractor without running the strategy.

The gap is not a tuning problem, and the third route out in [Economics of the shipped strategies](economics.md#the-arithmetic-you-have-to-beat) does not close it at this holding period: three hundredths of a basis point of gross capture cannot pay two basis points of fee, and at 1 s and 10 s the best conditional fill in the day is worth less than the fee even before the question of whether it is obtainable.

Whether a fair-value offset helps at a longer horizon is not something this measurement settles. The conditional markout prices a fill given the state; a strategy that skews its quote changes which fills it gets, and that is a fill-model question. On a top-of-book feed the `l2_queue` model's fill counts are a guess ([Backtesting](backtesting.md)), so a backtest of a skewed variant on this day would produce a number that could not be defended. The measurement that would settle it needs a feed with depth.

## Reproducing the numbers

```bash
python3 -m fastmm.data fetch --symbol BTCUSDT --date 2024-03-27
```

```python
t = fastmm.features(data="binance:BTCUSDT,2024-03-27", horizons=[0.1, 1, 10, 60])
print(fastmm.evaluate_signal(t, "imbalance", blocks=24)["table"])
```

The cross-check and the trade-row tables need one row per trade, because that is where a fill can land:

```python
f = fastmm.features(data="binance:BTCUSDT,2024-03-27", horizons=[1, 10, 60],
                    sample_book_updates=False, sample_trades=True)
```

The day is 1.5 GB of `bookTicker` CSV and the full table holds about 1.8 GB of columns; the evaluation needs about as much again while it ranks.
