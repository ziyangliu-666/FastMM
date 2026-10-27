# Economics of the shipped strategies

At a retail fee, the example backtest's market maker pays the venue about 1,400 times what it earns from the spread. `basic_mm`, `avellaneda_stoikov`, `options_mm` and `xmm` are reference implementations of published quoting rules, written to show the engine's API. `lead_mm` joins the touch of a thin pair when a liquid leader's mid, converted by an FX pair, leaves enough edge.

## The example backtest and the fee table

`configs/backtest-example.toml` charges Binance spot VIP 0, `[venues.sim.fees] maker_bps = 10.0`:

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml \
    --data synthetic --out -
```

<!-- output: fastmm-backtest --config configs/backtest-example.toml --data synthetic --out - -->
```text
backtest basic_mm  seed=1  md_events=15955  steps=16745  wall=0.01s
  net pnl                        -12.1548
  realized / unrealized / fees   0.0086 / 0.0001 / 12.1635
  fills (maker / taker)          147 (147 / 0)
  spread captured (bps)          0.008
  volume base / quote            0.20273 / 12163.49
```

Trading earned 0.0087 USDT on 12,163.49 USDT of volume, 0.0072 bps; the fee took 12.16 USDT. Net PnL is `realized + unrealized - fees`. The strategy does not read the fee table, so changing only `maker_bps` keeps the same 147 fills:

| `maker_bps` | Fees (USDT) | Net PnL (USDT) |
|---:|---:|---:|
| −0.5 | −0.6082 | +0.6169 |
| 0 | 0.0000 | +0.0087 |
| 0.5 | 0.6082 | −0.5995 |
| 1.0 | 1.2163 | −1.2076 |
| 2.0 | 2.4327 | −2.4240 |
| 10.0 (shipped) | 12.1635 | −12.1548 |

The break-even maker fee is 0.0072 bps. At a 0.5 bps rebate the run nets +0.6169 USDT, 98.6 % of it the rebate.

With `fill_model = "matching"` instead of `l2_queue`, the run realizes 0.0059 USDT on 8,442.57 USDT over 77 fills (0.007 bps). It also gets 3 `PostOnlyWouldCross` rejects. The break-even fee moves; the conclusion does not.

## Why the gross number is so small

The synthetic market has no informed flow, so a resting quote is never picked off. The aggressor's side is a fair coin, and the mid moves by ±1-tick steps independent of the orders. In this run the mid drift after the fills is −0.0006 USDT against 0.0093 USDT of gross spread capture.

On a real venue the taker chooses when to trade, so the quotes that fill are the ones that were wrong. Realised PnL falls below the quoted half spread by the adverse-selection cost, which the synthetic market prices at zero. The backtest reports an upper bound on spread capture.

`fill_model = "l2_queue"` is optimistic in a second way. It guesses queue position from L2 data with no counterparties. It also checks post-only orders against the book the strategy saw, so it never produces the post-only rejects that stale data causes live ([Configuration](../reference/configuration.md#backtest)).

## What a real session did

Quoting at the touch on a real book lost more than the fees. Two one-hour `basic_mm` sessions on BTCUSDT in Binance Demo Mode, at a 10 bps maker commission ([Journals, replay and PnL](../how-to/operations/journals-replay-pnl.md#example-binance-demo)):

| Quoting | Fills | Notional | Commission | PnL before fees | Trading PnL |
|---|---:|---:|---:|---:|---:|
| 15 bps from the mid | 0 | 0 | 0 | 0 | 0.00 USDT |
| At the touch | 1640, all maker | 31,622.62 USDT | 31.62 USDT | −1.32 USDT | −32.94 USDT |

## The arithmetic you have to beat

A round trip pays the maker fee twice. To break even, the half spread you capture after adverse selection must exceed the maker fee:

```text
captured_half_spread_bps  >  maker_fee_bps
```

Widening the quote does not solve it, because the fill rate falls faster than the spread rises. `basic_mm`'s default `half_spread_bps = 5` fills nothing in the synthetic market:

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml \
    --data synthetic --param half_spread_bps=5 --out -
```

```text
  fills (maker / taker)          0 (0 / 0)
  orders / cancels / replaces    260 / 258 / 0
```

The example sets `half_spread_bps = 0.01`, 6 ticks at a mid of 60,000 USDT, to get fills. The Demo session got none at 15 bps.

The routes out are outside what FastMM ships:

| Route | Example |
|---|---|
| a fee at or below zero | maker rebate, market-maker programme, volume tier |
| a spread wide relative to the fee | another venue or instrument |
| a signal that makes fills less adverse | measured and closed for BTCUSDT at VIP 0 in [Judging a signal](signal-research.md#was-a-quote-at-the-touch-adversely-selected) |

## What the backtest does not model

| Not modelled | Consequence |
|---|---|
| Informed order flow | Adverse selection is 0 |
| Market impact of your quotes | Nobody reacts to you |
| Queue position under `l2_queue` | Fill counts are optimistic |
| Venue rate limits, throttles, maintenance | More fills and cancels than live |
| Funding, borrow, settlement | Absent from backtest data; live sessions book funding on Binance USDⓈ-M and Bybit linear |
| Fee tiers that change with volume | `maker_bps` and `taker_bps` are constant |
| Commission in a third asset such as BNB | Left out of fees and positions, live too ([Troubleshooting](../how-to/operations/troubleshooting.md#orders-and-reconciliation)) |

## Before you size a live session

1. Run the backtest with your venue's real `maker_bps` and `taker_bps`.
2. Compare `realized` with `fees`: if it is smaller, the strategy pays the venue to trade.
3. Run the practice session in [Run on a testnet or Binance Demo](../how-to/operations/run-on-testnet.md), then reconcile it with the account ([Check PnL](../how-to/operations/journals-replay-pnl.md#check-pnl)). A testnet's thin book will not reproduce the fill rate; Demo Mode follows the real market and charges the real commission.
4. Set `[risk] max_loss` to what you accept losing across restarts. The budget is carried in `<journal_dir>/<name>.kill` until you clear it, so it is not a daily budget ([Run in production](../how-to/operations/running-in-production.md#restarts)).
