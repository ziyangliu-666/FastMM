# Fund each account of a pool

With [home placement](home-placement.md) each account of a pool buys with its own quote balance, so the accounts that are home to the most instruments need the most cash. A treasury with `targets = "demand"` moves the quote asset to each account in proportion to what its home instruments want resting.

## Turn it on

The treasury goes on the pool's primary, next to the `pool` table. The primary's connector carries the transfers with the master account's key, and each account names itself by `sub_account_email`; an account without one is the master account. The trading keys are set as for any [account pool](../../reference/configuration.md#account-pools).

```toml
[venues.binance]
kind = "binance_spot"
transfer_api_key_env = "MASTER_TRANSFER_KEY"
transfer_api_secret_env = "MASTER_TRANSFER_SECRET"
sub_account_email = "${ACCOUNT_A_EMAIL}"

[venues.binance_b]
kind = "binance_spot"
pool_of = "binance"
sub_account_email = "${ACCOUNT_B_EMAIL}"

[venues.binance.pool]
placement = "home"

[venues.binance.treasury]
enabled = true
asset = "USDT"
targets = "demand"
reserve = true
headroom = 0.15
min_amount = "100"
```

Start with `dry_run = true` to see the plans in the log and the journal without sending anything. Without `reserve = true` a bid placed between the plan and the transfer can take the cash; the venue then refuses the transfer and the pool waits `cooldown_s`.

## Declare what each instrument needs

The strategy says how much quote each instrument's bids want resting, in the instrument's quote asset, and again whenever its sizes change:

```cpp
ctx.set_funding_need(id, Notional::from_double(clip_usd * bid_levels));
```

In Python the call is `ctx.set_funding_need(inst, clip_usd * bid_levels)`. The engine adds up the needs of the instruments homed on each account; a need moves with its instrument's home. A need counts only once its instrument has a home, and only when its quote asset (a derivative's settlement asset) is the treasury's `asset`.

| Term | Value at each look |
|---|---|
| target | the needs of the account's home instruments × (1 + `headroom`), plus its `min_free` |
| what it has | its free balance plus what its own resting orders hold |
| short | below target × (1 − `threshold`); it asks for target less what it has |
| gives | what it has above its target, out of its free balance |

When the pool holds less than the targets add up to, `min_free` is kept and the needs' part of each target shrinks in the same proportion, below the need itself if it must. An account that is home to nothing and has no `min_free` has a target of zero and gives all its free balance. Until the strategy declares a need, after a start or a restart, nothing is planned.

Each home the planner moves takes its need along, so a move can start a transfer; `max_per_hour` and `min_interval_s` bound how many.

## Hold the payout before it is sent

With `reserve = true` the engine holds a planned transfer out of the payer's free balance before the treasury sends it. No order is sized against the hold, so a bid placed in the meantime cannot take the cash and make the transfer fail. The engine holds at most what the payer has free; the treasury sends what was held, and nothing if that is below `min_amount`.

| The treasury's record | What the engine does |
|---|---|
| reserve | holds the amount, at most the payer's free balance |
| sent | the hold becomes the amount sent |
| done | gives the hold back, takes the amount off the payer and adds it to the receiver until their next balance reports |
| failed, released | gives the hold back |
| timed out | keeps the hold |

Each record is a journaled `Transfer` control: a replay holds and moves the same amounts at the same points, and so does a backtest of the journal with `balances_from_journal`.

## When a transfer goes wrong

| Case | What happens |
|---|---|
| the venue refuses it | the hold is given back and the pool waits `cooldown_s` before planning again |
| no answer | the hold stays; the treasury asks for the transfer by its client id every `interval_s` |
| unresolved after `timeout_s` | the pool waits `cooldown_s` and keeps asking; the hold stays until the venue answers; a transfer the venue has no record of has failed |
| the process restarts | the ledger in `state_file` names the transfers in flight; they are held again and asked for before anything new is planned; one held and not yet sent is dropped |

## Try it in a backtest

A backtest runs the treasury on simulated time against the pool's simulated accounts: each account needs `[backtest.venues.<account>.balances]` ([Configuration](../../reference/configuration.md#backtestvenues)), or `balances_from_journal = true` with a journal. Four `[backtest]` keys set how the simulated venue carries transfers out:

```toml
[backtest]
transfer_latency_ms = 500   # a transfer is carried out this long after it is sent
transfer_p_fail = 0.05      # share of transfers the venue refuses
transfer_p_unknown = 0.05   # share whose answer is lost; carried out transfer_hang_ms later
transfer_hang_ms = 90000
```

To see what demand targets would have done in a recorded session, run its journal with `--data "journal:<file>"`, `balances_from_journal = true` and the treasury enabled: the session's own transfers between the pool's accounts are left out, its other flows of money stay, and the engine's transfers take their place. The summary counts the transfers and `transfers.csv` lists them.

## Watch it

`tools/journal_dump.py` prints each `Transfer` record with its amount, accounts and, for a held payout, `held seq=<n>`. The log has one line per hold, with what the receiver's homes need, and one per send and result (`[treasury binance] transfer ...`); and the status file and fastmm-top's Prometheus endpoint carry the counters (`fastmm_treasury_*`).
