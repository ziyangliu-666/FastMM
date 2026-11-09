# Keep each position on one account

On an [account pool](../../reference/configuration.md#account-pools) the engine sends each new order to whichever account has the balance and the most order window left. A bid that fills on a second account starts a second piece of the position there, and every piece then needs its own sell, and its own resend at every reprice. With `placement = "home"` each instrument's position lives on one account of the pool, its home, and pieces drain away by trading.

## Turn it on

Add a `pool` table to the pool's primary:

```toml
[venues.binance]
kind = "binance_spot"

[venues.binance_b]
kind = "binance_spot"
pool_of = "binance"

[venues.binance_c]
kind = "binance_spot"
pool_of = "binance"

[venues.binance.pool]
placement = "home"
homes = { BTCUSDT = "binance_b" }   # optional pins
```

The other keys ([reference](../../reference/configuration.md#venuespool)) have working defaults: `reduce_accounts = 1`, `spill = "never"` and a planner that moves at most 3 homes an hour. Accounts are venue ids in the strategy API, in the order of the `[venues.*]` tables; `ctx.pool(venue)` lists a pool's, `ctx.venue_name(id)` names one.

Each home buys with its own quote balance; a treasury with `targets = "demand"` keeps each account funded for its homes ([Fund each account of a pool](fund-a-pool.md)).

## Where orders go

| Order | Account |
|---|---|
| takes a piece's own part towards zero (a spot sell; on a derivative, the side opposite a piece's position) and fits in what that piece has left | the piece, in pool order |
| any other | the home |
| names `NewOrderRequest::account` (or `DesiredQuotes::bid_account` / `ask_account`) | that account, as before |

A piece never takes an order larger than its part, so it never goes through zero; a flatten slices the position one holder at a time.

What happens to an adding order the home cannot take depends on `spill`:

| `spill` | Home down (order link or kill switch) | Home's order window full | Home short of the balance |
|---|---|---|---|
| `"never"` | refused `HomeUnavailable` | refused `RateLimit` | refused `BalanceShort` |
| `"flat"` | a flat, unpinned instrument moves to the usable account with the most window; else refused | refused `RateLimit` | refused `BalanceShort` |
| `"busy"` | sent to the account the balance routing picks; the home stays | sent elsewhere from `spill_at` of the window (default 0.8) | sent to an account that covers it |

With `"flat"`, when the old home's link is back, the adding orders left on it are cancelled. With `"busy"`, an order `[capacity]` prices above its value on the home also goes elsewhere, and `balance_room` sizes a buy against any usable account. A spilled order that fills starts a piece, which drains like any other. On a derivative, `"busy"` behaves as `"flat"`.

`reduce_accounts` is how many holders carry reducing orders of one instrument and side at once; a piece may always join while only the home carries one. With `1`, a piece's part is offered alone until it is gone, so the resting sell is the piece's size, not the position's. `ctx.balance_room` and `fit_to_balance` size each side to the one account its next order goes to: a single-level quote stays on that account. Above `1`, only a strategy that places one level per account (`NewOrderRequest`, or several levels) uses the extra holders.

## Where homes come from

| Source | When |
|---|---|
| pin | at the start, from `homes`; the planner never moves a pin |
| reconciliation | once every account of the pool has reported its balances: an instrument held on one account gets that account, one held on several keeps its home or else goes to the largest holder |
| first order | an instrument with no home yet goes to the account holding the most of it, else round the pool by instrument id |
| planner | every `plan_interval_s`, from the engine's own orders |
| operator | `fastmm-ctl pool home SYMBOL ACCOUNT`; this pins the home until the end of the session, `--free` leaves it to the planner |
| strategy | `ctx.set_home(id, account)` |

With `placement = "home"` a home change cancels the instrument's adding orders on every other account; the strategy's next quotes go to the new home. What the old home holds becomes a piece and drains through the reducing orders.

The planner keeps the orders each instrument sent per 10 s window over the last `plan_lookback_h`. Each plan it first gives a home to each instrument that has orders and none, then moves one instrument at a time off the account with the heaviest load, where that lowers the busiest account's share of the pool's busiest 1% of windows plus its share of all orders, up to `plan_max_moves` instruments. Pins stay.

Every change is in the journal as a `PoolHome` control: a replay makes the same changes at the same points, and a backtest runs the planner on simulated time. A restarted session starts again from the pins and the reconciliation, and its planner from an empty lookback.

## Try it before switching

Keep `placement = "balance"` with the table present: the engine keeps and journals the homes and runs the planner, and routes as before. `tools/journal_dump.py` prints each `PoolHome` with its source.

A backtest over a pool with [`[backtest.venues.<member>.balances]`](../../reference/configuration.md#backtestvenues) runs either placement on simulated accounts; `orders_credit_fills = true` gives back a filled order's window slot, as Binance Spot does. To compare against a recorded session, run the same journal twice with `--data "journal:<file>"`, once with each placement.

## Watch it

The status file's quote rows carry `home` and `pieces` per instrument ([Status file](../../reference/status-file.md)). In a strategy, `ctx.home(id)`, `ctx.holding(id, account)` and `ctx.pieces(id)` read the same ([Strategy API](../../reference/strategy-api.md#homes)).
