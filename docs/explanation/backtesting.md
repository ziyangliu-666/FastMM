# Backtesting

`fastmm-backtest` prints the net PnL, where it came from, and what each fill was worth afterwards. Read the decomposition and the markouts before the net PnL: a passive quoter can show a positive spread capture and a positive net PnL while losing on every fill it gets.

## What the simulator cannot tell you

The simulated venue ([Simulated exchange](../reference/sim-exchange.md)) and the synthetic market generator are a latency and book-mechanics model, not a market model. Four properties of the run are assumptions:

| Assumption | Consequence |
|---|---|
| Your orders have no market impact: the flow does not react to your quotes | Fill rates and queue positions are optimistic; in a real book the size you add changes who trades with you |
| The latent mid is a symmetric random walk with no drift and no autocorrelation | Adverse selection only appears through the queue and the latency, never because the flow knew something |
| The counterparty side of each synthetic order is a coin flip | Order flow carries no information, so a strategy that predicts flow measures as worthless here and a strategy that ignores it measures as fine |
| There is no informed flow and no toxic counterparty | The markouts you measure here are a floor on the adverse selection you will see live, not an estimate of it |

The synthetic market's touch sits `base_spread_ticks` from its mid, so at a mid of 60,000 USDT and a tick of 0.01 USDT the whole spread is about 0.003 bps: no passive strategy in that market can capture more than a fraction of a basis point, whatever it quotes. And `fill_model = "l2_queue"` replays recorded levels with no counterparties at all, so it checks post-only orders against the same book the strategy saw and never produces the post-only rejects that stale market data causes live ([Configuration](../reference/configuration.md#backtest)).

A backtest here is a determinism, latency and plumbing test that also gives an upper bound on PnL.

## Markouts

A market maker's fill is worth the spread it captured minus what the mid took back afterwards. For a fill of signed quantity `s` (positive bought, negative sold) at price `p` and time `t`, with the venue mid `m`:

```text
spread capture       = s * (m(t) - p)
markout(h)           = s * (m(t + h) - p)
adverse selection(h) = spread capture - markout(h)
```

`spread_captured_bps` is positive by construction for any passive quoter: it quotes away from the mid, so it is filled away from the mid. Only the markout says whether the fills were worth taking: a capture of +1 bps with a markout of −3 bps at 10 s means the strategy paid 4 bps to be filled.

The horizons come from `[backtest] markout_horizons_s` (default `"1,10,60"` seconds). The mid used is the venue mid at `t + h`, read with the simulated clock stopped there; the run loop stops at every fill time plus horizon, so nothing is interpolated. A fill the run could not mark is dropped from that horizon and counted, never marked at a substitute price:

* `past end` — `t + h` is after the last event of the run. A 60 s markout on a 60 s run measures nothing and says so.
* `no mid` — the venue book had only one side at `t + h`, or at the fill. Under `l2_queue` the mirrored book holds only the levels the feed published, so it empties on one side when the price moves past the recorded depth. Under `matching` the synthetic generator's market orders are larger than its limit orders (`market_qty_median_lots` 1500 against `limit_qty_median_lots` 300 in `configs/backtest-example.toml`), so the sweep that fills a passive quote often empties that side of the book: a fifth to a half of the fills of a `matching` run have no mid at the moment of the fill and drop out of every horizon. Such a fill contributes its whole `signed qty * (final mid − price)` to the mid drift of the decomposition, because none of it can be called spread capture.

Because the excluded fills differ per horizon, `capture` is recomputed over the same fills as the markout of that horizon, so the two columns of one row are comparable.

Markouts are reported per instrument, per side, per liquidity flag and in total, in quote currency and in bps of traded notional, in the CLI summary, in `summary.json` (`markouts`), from Python (`result.markouts()`, `fastmm.markout_frame(result)`) and per fill in `fills.csv` (`mid_1s`, `mid_10s`, `mid_1m`). One backtest covers one strategy, so the total is that strategy's markout; a sweep gives one result per point.

`tools/pnl_report.py` computes the same markouts from a recorded session journal, marking against the `BookTicker` stream. A journal recorded without top-of-book updates (`[sim] book_ticker = false`) has no mid to mark against and the report says so.

## Fill quality

| Metric | Meaning |
|---|---|
| realised spread | `signed qty * (mid at fill − price)` over the traded notional, in bps: the same quantity the markout starts from |
| fills at touch / behind / through | Where the fill price sat relative to the venue's best quote on that side at the fill |
| quotes filled / placed | Share of new orders that got at least one fill, which is not the same as `fill ratio` (fills per order) |
| time to fill | Venue arrival of a new order to its first fill, p50 / p90 / p99 |
| queue ahead at fill | Displayed quantity still ahead of the order when it filled. `fill_model = "l2_queue"` only; the matching engine fills from the front of the queue, so it has no such number |

## Fees and rebates

Fees are centi-basis-points of the fill notional. A positive rate is a fee the account pays; a negative rate is a rebate it receives. The simulated venue books the signed amount as the fill's fee, so a rebate raises net PnL.

Each instrument pays its own venue's `[venues.<name>.fees]`, and `[[instruments]] maker_bps` / `taker_bps` override one instrument ([Configuration](../reference/configuration.md#venuesfees)). The shipped `configs/backtest-example.toml` charges Binance spot VIP 0, 0.100 % maker and 0.100 % taker, which is 10 bps each side.

Check a maker rebate against the venue's published schedule. Binance spot pays no maker rebate at VIP 0; a configured `maker_bps = -0.5` adds 0.5 bps to every passive fill, more than any strategy in this repository captures gross.

## Reading the summary

The decomposition is exact:

```text
net = gross spread capture + mid drift after the fills − fees paid + rebates received + unexplained
```

`mid drift` is `signed qty * (final mid − mid at fill)` summed over the fills: the adverse selection over the whole run plus the mark-to-market of whatever inventory is still open at the end. `unexplained` is the difference from the ledger's own net PnL: fixed-point rounding on a healthy run; anything larger means the split, not the ledger, is wrong.

## Reference example

`basic_mm` from `configs/backtest-example.toml` on the synthetic market, 60 s, seed 1, at Binance spot VIP 0 fees:

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic
```

```text
  net pnl                        -11.9755
  fills (maker / taker)          112 (112 / 0)
  spread captured (bps)          0.008
  realized spread (bps of notional) 0.008
  fills at touch / behind / through 8.0% / 91.1% / 0.9%
  quotes filled / placed         101 / 366 (27.6%)
  time to fill p50/p90/p99       44.1 / 271.9 / 1100.2 ms
  queue ahead at fill p50/p90    0.00000 / 0.00621

where the PnL came from (quote currency, 11984.99 traded notional)
  gross spread capture                 0.0097  +0.008 bps of 11984.99, mid at the fill
  mid drift after the fills           -0.0002  adverse selection + the open inventory, marked at the final mid
  fees paid                          -11.9850
  rebates received                     0.0000
  = net                              -11.9755
  reported net pnl                   -11.9755
  unexplained                         -0.0000

markout per fill (mid at fill + horizon vs the fill price; bps of notional)
  horizon     markout     mo bps    capture    cap bps    adv.sel    fills past end   no mid
  1s           0.0082     0.0078     0.0082     0.0079     0.0000       97       15        0
  10s          0.0067     0.0075     0.0070     0.0079     0.0003       81       31        0
  1m           0.0000     0.0000     0.0000     0.0000     0.0000        0      112        0
```

`first_mm` from the tutorial (`examples/cpp/tutorial/first_mm_backtest.cpp`, 60 s, seed 7, same fees) on the matching fill model:

```text
  net pnl                        -27.0083
  fills (maker / taker)          467 (467 / 0)
  realized spread (bps of notional) 0.002
  fills at touch / behind / through 78.4% / 0.0% / 0.0%
  quotes filled / placed         456 / 762 (59.8%)
  queue ahead at fill p50/p90    n/a (the matching fill model has no queue position)

where the PnL came from (quote currency, 27014.99 traded notional)
  gross spread capture                 0.0046  +0.002 bps of 20974.79, over the 366 of 467 fills that had a venue mid
  mid drift after the fills            0.0021  adverse selection + the open inventory, marked at the final mid
  fees paid                          -27.0150
  = net                              -27.0083

markout per fill (mid at fill + horizon vs the fill price; bps of notional)
  horizon     markout     mo bps    capture    cap bps    adv.sel    fills past end   no mid
  1s           0.0053     0.0026     0.0044     0.0022    -0.0004      351        9      107
  10s          0.0040     0.0024     0.0037     0.0022    -0.0002      295       80       92
```

Neither strategy has a measurable gross edge. `basic_mm` captures 0.008 bps and `first_mm` 0.002 bps of traded notional gross, against a 10 bps maker fee: both pay three to four orders of magnitude more in fees than they earn from the spread, and both are net negative at any fee a real venue charges. Their markouts are within rounding of their spread capture, as the symmetric random-walk mid guarantees: the simulator produces no adverse selection beyond queue and latency artefacts, so near-zero adverse selection here says nothing about live.

Over 600 s the same `basic_mm` run trips the `max_loss = 100` kill switch on fees alone after about 85 s of trading. `avellaneda_stoikov` at its default `gamma` and `kappa` never fills on this market, so it has no markout to report.

With `maker_bps = -0.5`, a 0.5 bps maker rebate, the same 60 s run nets +0.6087, of which 0.5992 (98.4 %) is the rebate and 0.0097 the spread.

## See also

* [Configuration](../reference/configuration.md#backtest): every `[backtest]` and `[sim]` key
* [Command lines](../reference/cli.md#fastmm-backtest): flags of `fastmm-backtest`
* [Determinism](determinism.md): why two runs of the same configuration send the same orders
* [Journals, replay and PnL](../how-to/operations/journals-replay-pnl.md): `tools/pnl_report.py` on a live session
* [Simulated exchange](../reference/sim-exchange.md): the venue model the backtest and `fastmm-sim-exchange` share
