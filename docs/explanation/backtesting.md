# Backtesting

`fastmm-backtest` prints the net PnL, where it came from, and what each fill was worth afterwards. Read the decomposition and the markouts before the net PnL: a passive quoter can show a positive spread capture and a positive net PnL while losing on every fill it gets.

## Model assumptions

The simulated venue ([Simulated exchange](../reference/sim-exchange.md)) and the synthetic market generator model latency and book mechanics. The run assumes four properties of the market:

| Assumption | Consequence |
|---|---|
| Your orders have no market impact: the flow does not react to your quotes | Fill rates and queue positions are optimistic; in a real book the size you add changes who trades with you |
| The latent mid is a symmetric random walk with no drift and no autocorrelation | Adverse selection only appears through the queue and the latency, never because the flow knew something |
| The counterparty side of each synthetic order is a coin flip | Order flow carries no information, so a strategy that predicts flow measures as worthless here and a strategy that ignores it measures as fine |
| There is no informed flow and no toxic counterparty | The markouts you measure here are a floor on the adverse selection you will see live, not an estimate of it |

The synthetic market's touch sits `base_spread_ticks` from its mid, so at a mid of 60,000 USDT and a tick of 0.01 USDT the whole spread is about 0.003 bps: no passive strategy in that market can capture more than a fraction of a basis point, whatever it quotes. And `fill_model = "l2_queue"` replays recorded levels with no counterparties at all, so it checks post-only orders against the same book the strategy saw and never produces the post-only rejects that stale market data causes live ([Configuration](../reference/configuration.md#backtest)). Its queue position starts at the displayed quantity at the order's price, or the touch's of a book ticker newer than the depth, less what the trades printed since took, and is capped by the ticker; `ctx.queue_ahead` runs the same model on the data the strategy sees ([Strategy API](../reference/strategy-api.md#execution-view)). [Calibrating against live sessions](#calibrating-against-live-sessions) measures how close it comes.

## Our orders in the feed

A live venue's depth and book ticker streams show our resting orders; a strategy that reads the touch sees its own quote there. With recorded data (`[backtest] own_orders_in_feed`, on by default) the simulated venue does the same (`SimTransport::with_own`): each forwarded depth level and ticker carries our resting quantity at its price, the next depth update also carries our levels that changed since the last one (quantity 0 for one that was only ours), and a book ticker goes out when one of our orders moves the top of book, flagged synthetic and with no update id, so a strategy comparing it with the depth book goes by time. Our orders at or through the recorded opposite touch are not shown: a live venue would have matched them, and the fill model waits for a trade. The synthetic market's book holds our orders in any case. The engine follows our quantity in the feed as on a live venue.

Without it a strategy acts on a book a live venue never showed. `lead_mm` never improves on its own order (a live book shows it at the touch); when the level it had improved on goes, a feed without our order shows the next level as the touch, and the strategy moves its quote down to it and back, a cancel and a new order each time. On two live BTCU sessions the `strip_own` backtests sent 18 and 20 % more orders than the sessions with the same fills; with our orders in the feed and [venue-time order](#venue-time), 1 % fewer and 0.2 % more.

## Venue time

A recording is in receive order, and its streams reach the recorder with different delays: in a Binance Spot session recorded from AWS Tokyo, 46 % of trades arrive after a book ticker the venue stamped later (by 0.5 ms at the median, 40 ms at the 99th percentile). Taken in that order, an order that reached the simulated venue in between would be filled by a trade printed before it arrived, and cancelled before a trade printed while it rested. The simulated venue therefore takes recorded events in venue-time order (`sim/venue_order.hpp`): events wait until none still unread can be earlier, assuming none was received more than `[backtest] reorder_window_ms` (default 1000) after its venue time. The strategy still gets them in recorded order, at their recorded times under `md_arrival = "recorded"`. The run's report counts the events reordered and those later than the window; a trade later than the window still fills only orders that had arrived by its time (`core/queue_model.hpp`), and under `matching` a recorded level that crosses a resting order fills it at the later of the two arrivals.

On three live BTCU sessions this removed the first fills stamped before their order's arrival (33 of 232, 18 of 159 and 58 of 595), and on two of them the backtests' orders and fills came within 2 % of the live ones.

## Markouts

A market maker's fill is worth the spread it captured minus what the mid took back afterwards. For a fill of signed quantity `s` (positive bought, negative sold) at price `p` and time `t`, with the venue mid `m`:

```text
spread capture       = s * (m(t) - p)
markout(h)           = s * (m(t + h) - p)
adverse selection(h) = spread capture - markout(h)
```

`spread_captured_bps` is positive by construction for any passive quoter: it quotes away from the mid, so it is filled away from the mid. Only the markout says whether the fills were worth taking: a capture of +1 bps with a markout of −3 bps at 10 s means the strategy paid 4 bps to be filled.

The horizons come from `[backtest] markout_horizons_s` (default `"1,10,60"` seconds). The mid used is the venue mid at `t + h`, read with the simulated clock stopped there; the run loop stops at every fill time plus horizon, so nothing is interpolated. A fill the run could not mark is dropped from that horizon and counted, never marked at a substitute price:

* `past end`: `t + h` is after the last event of the run. A 60 s markout on a 60 s run measures nothing and says so.
* `no mid`: the venue book had only one side at `t + h`, or at the fill. Under `l2_queue` the mirrored book holds only the levels the feed published, so it empties on one side when the price moves past the recorded depth. Under `matching` the synthetic generator's market orders are larger than its limit orders (`market_qty_median_lots` 1500 against `limit_qty_median_lots` 300 in `configs/backtest-example.toml`), so the sweep that fills a passive quote often empties that side of the book: a fifth to a half of the fills of a `matching` run have no mid at the moment of the fill and drop out of every horizon. Such a fill contributes its whole `signed qty * (final mid − price)` to the mid drift of the decomposition, because none of it can be called spread capture.

Because the excluded fills differ per horizon, `capture` is recomputed over the same fills as the markout of that horizon, so the two columns of one row are comparable.

Markouts are reported per instrument, per side, per liquidity flag and in total, in quote currency and in bps of traded notional, in the CLI summary, in `summary.json` (`markouts`), from Python (`result.markouts()`, `fastmm.markout_frame(result)`) and per fill in `fills.csv` (`mid_1s`, `mid_10s`, `mid_1m`). One backtest covers one strategy, so the total is that strategy's markout; a sweep gives one result per point.

`tools/pnl_report.py` computes the same markouts from a recorded session journal, marking against the `BookTicker` stream. A journal recorded without top-of-book updates (`[sim] book_ticker = false`) has no mid to mark against and the report says so.

## Fill quality

| Metric | Meaning |
|---|---|
| realised spread | `signed qty * (mid at fill − price)` over the traded notional, in bps: the same quantity the markout starts from |
| fills at / inside / behind / through | Where the fill price sat relative to the venue's best quote on that side at the fill: equal, better (the order improved it), worse, or at or beyond the opposite quote |
| quotes filled / placed | Share of new orders that got at least one fill, which is not the same as `fill ratio` (fills per order) |
| time to fill | Venue arrival of a new order to its first fill, p50 / p90 / p99 |
| queue ahead at fill | Displayed quantity still ahead of the order when it filled. `fill_model = "l2_queue"` only; the matching engine fills from the front of the queue, so it has no such number |

## Fees and rebates

Fees are centi-basis-points of the fill notional. A positive rate is a fee the account pays; a negative rate is a rebate it receives. The simulated venue books the signed amount as the fill's fee, so a rebate raises net PnL.

Each instrument pays its own venue's `[venues.<name>.fees]`, and `[[instruments]] maker_bps` / `taker_bps` override one instrument ([Configuration](../reference/configuration.md#venuesfees)). The shipped `configs/backtest-example.toml` charges Binance spot VIP 0, 0.100 % maker and 0.100 % taker, which is 10 bps each side.

Check a maker rebate against the venue's published schedule. Binance spot pays no maker rebate at VIP 0; a configured `maker_bps = -0.5` adds 0.5 bps to every passive fill, more than any strategy in this repository captures gross.

## Several venues

A run can hold instruments on several venues: quote one on venue A and hedge another on venue B. Each venue gets its own latency model (the first keeps `[backtest] seed`, the others derive theirs from it and the venue id), its own order and market-data connection, and its own replace and STP settings ([`[backtest.venues.<name>]`](../reference/configuration.md#backtestvenues)): quotes on a venue with replace are amended in place, on one without it cancelled and sent again. The matching engine, the queue model and the books are shared, since an instrument lives on one venue.

Recorded feeds of the venues are merged by event time: `--data "binance:BTCUSDT,2024-03-27,venue=0; csv:other.csv,venue=1"`, or a list in Python (`data=[...]`). Each source's `venue=` must be the venue of its instruments in `[venues]` order. The venues' clocks are taken as they are: a feed stamped with local receive time and one stamped with venue time interleave wrongly by the difference.

`equity.csv` gains `pnl_<id>`, `position_<id>`, `mid_<id>` and `quoted_<id>` per instrument when there is more than one (`result.equity_by_instrument` in Python); `pnl` is in the instrument's settlement currency. The `position` and `mid` columns stay the sum over instruments and instrument 0's mid. The synthetic market drives instrument 0 only.

## Balances

With `[backtest.balances]` ([Configuration](../reference/configuration.md#backtestbalances)) each simulated venue keeps the strategy's account (`src/sim/sim_account.cpp`):

| Instrument | Held by an order | A fill |
|---|---|---|
| spot buy | notional at the order price, in the quote asset (a market buy at the opposite touch) | adds the base, takes the notional at the fill price and the fee from the quote |
| spot sell | quantity, in the base asset | takes the base, adds the notional less the fee to the quote |
| derivative | notional x `[[instruments]] initial_margin` of the larger side of the instrument's orders, in the settlement asset; nothing for an order that only reduces the position | moves the position's margin at its entry price into locked; closing realises PnL into the wallet; the fee comes out of it |

An order that holds more than the free balance is refused with `InsufficientBalance` (venue code `-2010`); a replace whose new leg does not fit cancels the order and refuses the new one. Unrealised PnL is not counted in a derivative's free margin. The venue reports a snapshot at the start and, after each acknowledgement, fill, cancel, expiry or reject that moved an asset, that asset's row behind the order event, on the same connection. The engine's estimate, `[risk] check_balance` (`BalanceShort` before an order leaves), `ctx.balance`, `basic_mm`'s sizing and `xmm`'s side pulling work on those reports as on a live venue's ([Risk model](risk-model.md#balance-check)). With `check_balance = false` the order goes out and the venue refuses it, which is what a session without a balance table got live.

`balances_from_journal = true` starts each venue's account from the first balance snapshot of the journal the backtest runs over, so a live session replayed as a `strip_own` backtest starts from the balances it had.

## Reading the summary

The decomposition is exact:

```text
net = gross spread capture + mid drift after the fills − fees paid + rebates received + unexplained
```

`mid drift` is `signed qty * (final mid − mid at fill)` summed over the fills: the adverse selection over the whole run plus the mark-to-market of whatever inventory is still open at the end. `unexplained` is the difference from the ledger's own net PnL: fixed-point rounding on a healthy run; anything larger means the split, not the ledger, is wrong.

## Calibrating against live sessions

`fastmm-data calibrate` fits the `l2_queue` fill model and the simulated latency to journals recorded by `fastmm-live`, and prints the `[backtest]` keys to use:

```bash
./build/release/bin/fastmm-data calibrate sessA.fmj sessB.fmj sessV.fmj --backtest
```

For each session it replays the orders that rested, from ack to end in venue time, through the queue model on the journal's own market data (the [fill check](../how-to/operations/journals-replay-pnl.md#check-the-fill-model-against-live-fills)), once per `queue_conservatism` of `--conservatism` (default `0,0.25,0.5,0.75,1`), and counts orders by whether they filled live and in the model:

```text
  conservatism     both   live  model   none    hit   miss  false  error model/live
  0.00              151      2      0   1573  0.987  0.013  0.000  0.013      0.987
  ...
  1.00              151      2      0   1573  0.987  0.013  0.000  0.013      0.987
  no ticker         144      9      1   1572  0.941  0.059  0.001  0.065      0.940
  no tape           150      3      0   1573  0.980  0.020  0.000  0.020      0.981
  neither           127     26      1   1572  0.830  0.170  0.001  0.175      0.795
```

`hit` is the share of live fills the model also fills, `miss` the rest, `false` the share of orders not filled live that the model fills. `error` is (live only + model only) over the orders filled either way; the fit minimises it, then `model/live` (filled quantity) nearest 1, then prefers the larger conservatism. `no ticker`, `no tape` and `neither` repeat the fitted value without the book ticker, without the trades printed since the depth update, or both: what each input is worth on this data. The session is followed by the time from ack to first fill, live and in the model, and the model's queue ahead at the live fills; zero there is agreement.

The conservatism is cross-validated: fitted on each session alone and scored on the others (`best c` is the best value on those sessions themselves); the snippet's value is fitted on all of them. When every value scores the same on every session, the data does not identify it and 1 is kept: the queues of orders at or inside the touch are set by the book ticker, where the conservatism does not act.

Latency comes from each order's send time, the venue's time on its ack and the ack's arrival. The simulated venue draws each leg as a fixed part plus a lognormal excess (mean `jitter`); the fit solves fixed and jitter for the measured 5th and 50th percentiles of the round trip. Binance stamps orders in whole milliseconds, so the one-way leg is taken at its median (plus the half millisecond the truncation takes off, at most the round trip's fixed part) and the ack leg gets the rest. The tail of the round trip (a p99 of 40 ms on Binance Spot from AWS Tokyo) is beyond this model.

```text
[backtest]
fill_model = "l2_queue"
queue_conservatism = 1.00
md_arrival = "recorded"
latency_fixed_us = 792
latency_jitter_us = 0
latency_ack_us = 0
latency_ack_jitter_us = 817
```

`md_arrival = "recorded"` is for backtests over journals like these: market data reaches the strategy when the session received it.

`--backtest` then re-runs each session with its own recorded strategy configuration (or `--config`, one for all or one per journal) over its journal with our orders stripped (`journal:<file>,strip_own=1`), once with the configuration's `[backtest]` and once with the fitted keys (`l2_queue` and recorded arrival in both), and prints both beside the live session: orders, the venue's rejects, fills, time to fill, the PnL decomposition and markouts, all three marked against the journal's book ticker mids. The fill check isolates the fill model; this comparison adds everything else a backtest re-decides, such as how often the strategy requotes and what the account's balances allowed. `--csv` writes the fill-check grid.

## Reference example

`basic_mm` from `configs/backtest-example.toml` on the synthetic market, 60 s, seed 1, at Binance spot VIP 0 fees:

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml \
    --data synthetic
```

```text
  net pnl                        -11.9755
  fills (maker / taker)          112 (112 / 0)
  spread captured (bps)          0.008
  realized spread (bps of notional) 0.008
  fills at / inside / behind / through 8.0% / 27.7% / 63.4% / 0.9%
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
  fills at / inside / behind / through 78.4% / 0.0% / 0.0% / 0.0%
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
