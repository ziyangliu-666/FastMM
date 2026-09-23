# Economics of the shipped strategies

`basic_mm`, `avellaneda_stoikov` and `options_mm` are reference implementations of published quoting rules. They show how to use the engine's API. None of them has been shown to make money at a fee schedule you can get, and the shipped example backtest is profitable only because it is configured with a maker rebate.

Read this before you size a live session. [Running this in production](../how-to/operations/running-in-production.md) covers the operational side.

## The example backtest earns its profit from the fee table

`configs/backtest-example.toml` sets `[venues.sim.fees] maker_bps = -0.5`: the simulated venue pays 0.5 bps on every maker fill.

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic --out -
```

```text
backtest basic_mm  seed=1  md_events=15611  steps=16363  wall=0.05s
  net pnl                        0.6087
  realized / unrealized / fees   0.0095 / 0.0000 / -0.5992
  fills (maker / taker)          112 (112 / 0)
  spread captured (bps)          0.008
  volume base / quote            0.19975 / 11984.99
```

Net PnL is `realized + unrealized - fees`, so the rebate of 0.5992 USDT is 98.4 % of the 0.6087 USDT result. Trading contributed 0.0095 USDT on 11,984.99 USDT of volume: 0.0079 bps.

Changing only `maker_bps` in that file gives the same 112 fills — the strategy does not read the fee table — and this PnL:

| `maker_bps` | Fees (USDT) | Net PnL (USDT) |
|---:|---:|---:|
| −0.5 (shipped) | −0.5992 | +0.6087 |
| 0 | 0.0000 | +0.0095 |
| 0.5 | 0.5992 | −0.5898 |
| 1.0 | 1.1985 | −1.1890 |
| 2.0 | 2.3970 | −2.3875 |
| 10.0 | 11.9850 | −11.9755 |

The break-even maker fee is 0.0079 bps. Reproduce it by copying `configs/backtest-example.toml`, editing `[venues.sim.fees] maker_bps` and running the command above.

The same run with `fill_model = "matching"` instead of `l2_queue` captures 0.0072 bps gross (0.0059 USDT on 8,442.57 USDT over 77 fills) and adds 3 `PostOnlyWouldCross` rejects. The break-even fee moves, the conclusion does not.

## Why the gross number is so small

The synthetic market has no informed flow. `MarketGenerator::do_market` picks the aggressor's side with a fair coin (`src/sim/market_generator.cpp`), and the latent mid is a separate Poisson process of ±1-tick steps in `MarketGenerator::step`. A market order therefore carries no information about the next mid move, so a resting quote is never picked off. Under that assumption `spread_captured_bps` and realised PnL are the same number, which is what the run shows: 0.008 bps × 11,984.99 USDT = 0.0095 USDT.

On a real venue the taker chooses when to trade. The quotes that fill are the ones that were wrong, so realised PnL is below the quoted half spread by the adverse-selection cost, which the synthetic market prices at zero. The backtest therefore reports an upper bound on spread capture, before fees.

`[backtest] fill_model = "l2_queue"`, which `configs/backtest-example.toml` uses, is optimistic in a second way: it estimates queue position on L2 data that has no counterparties, and it checks post-only orders against the same book the strategy saw, so it never produces the post-only rejects stale market data causes live ([Configuration](../reference/configuration.md#backtest)).

## What a real session did

Two one-hour `basic_mm` sessions on BTCUSDT in Binance Demo Mode, at the account's 10 bps maker commission ([Journals, replay and PnL](../how-to/operations/journals-replay-pnl.md#example-binance-demo)):

| Quoting | Fills | Trading PnL |
|---|---:|---:|
| 15 bps from the mid | 0 | 0.00 USDT |
| At the touch | 1640 (all maker) | −32.94 USDT |

31,622.62 USDT of notional paid 31.62 USDT in commission, and realised PnL before fees was −1.32 USDT: the fills were adverse as well as expensive. Widening the quote to 15 bps produced no fills at all. The tutorial states the same arithmetic for its own strategy: at 10 bps per fill and a 5 bps quote, every fill loses money ([Binance Demo](../tutorials/first-strategy/09-binance-demo.md)).

## The arithmetic you have to beat

A round trip pays the maker fee twice. To break even, the half spread you actually capture, after adverse selection, must exceed the maker fee:

```text
captured_half_spread_bps  >  maker_fee_bps
```

Widening the quote does not solve it, because the fill rate falls faster than the spread rises. `basic_mm`'s default `half_spread_bps = 5` fills nothing at all in the shipped synthetic market:

```bash
./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic --param half_spread_bps=5 --out -
```

```text
  fills (maker / taker)          0 (0 / 0)
  orders / cancels / replaces    260 / 258 / 0
```

`configs/backtest-example.toml` sets `half_spread_bps = 0.01` (6 ticks at a mid of 60,000) to get fills at all, and the Demo session above saw the same thing at 15 bps. The routes out are all outside what FastMM ships: a fee tier at or below zero (maker rebate, market-maker programme, volume tier), a venue whose spread is wide relative to its fee, or a signal that makes the fills less adverse.

## What the backtest does not model

| Not modelled | Consequence |
|---|---|
| Informed order flow | Spread capture is an upper bound; adverse selection is 0 |
| Market impact of your own quotes | Other participants do not react to you |
| Queue position on `l2_queue` beyond `queue_conservatism` | Fill counts are a guess; the reference calls the model optimistic |
| Venue rate limits, throttles and maintenance windows | Fill and cancel counts are higher than a real session's |
| Funding, borrow and settlement | Perpetual and margin costs are absent |
| Fee tier changes with volume | `maker_bps` and `taker_bps` are constant |
| Commission in a third asset (for example BNB) | The engine leaves it out of fees and positions live too ([Troubleshooting](../how-to/operations/troubleshooting.md#orders-and-reconciliation)) |
| Inverse contracts | PnL is computed linearly ([Running this in production](../how-to/operations/running-in-production.md#6-pnl-and-accounting)) |

## Before you size a live session

- Run the backtest with your venue's real `maker_bps` and `taker_bps`, not the shipped rebate.
- Compare realised PnL against fees: if `realized` is smaller than `fees`, the strategy is paying the venue to trade.
- Run the practice session in [Run on a testnet or Binance Demo](../how-to/operations/run-on-testnet.md), then reconcile it against the account ([Check PnL](../how-to/operations/journals-replay-pnl.md#check-pnl)). A testnet's thin book will not reproduce the fill rate; Demo Mode follows the real market and charges the real commission.
- Set `[risk] max_loss` to what you accept losing in one process lifetime. It is not a daily budget and does not survive a restart ([Running this in production](../how-to/operations/running-in-production.md#the-loss-budget-is-per-process)).
