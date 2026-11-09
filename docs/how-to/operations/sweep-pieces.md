# Sweep pieces home

On an [account pool](../../reference/configuration.md#account-pools) with homes ([Keep each position on one account](home-placement.md)), a buy that fills away from the home leaves a piece there, and every piece needs its own sell at every reprice. With `sweep = true` the engine moves each spot piece to its home by internal transfer a few seconds after it lands, and sends the pool's sells to the home. A transfer costs no order slot.

## Turn it on

The primary's connector needs the master account's transfer key ([Binance Spot](../../reference/venues/binance-spot.md#internal-transfers)); each account names itself with `sub_account_email`:

```toml
[venues.binance]
kind = "binance_spot"
transfer_key_type = "ed25519"
transfer_api_key_env = "MASTER_API_KEY"
transfer_private_key_file = "${MASTER_PRIVATE_KEY_PATH}"
sub_account_email = "${ACCOUNT_A_EMAIL}"

[venues.binance.pool]
placement = "home"
spill = "busy"
sweep = true

[venues.binance.transfers]
per_hour = 60
per_day = 600
```

`spill = "busy"` lets adding orders leave a home whose order window is busy or whose cash is short; the sweep brings back what they buy. The keys and their defaults: [`[venues.<primary>.pool]`](../../reference/configuration.md#venuespool) and [`[venues.<primary>.transfers]`](../../reference/configuration.md#venuestransfers).

To see what it would do first, set `dry_run = true` in `[venues.binance.transfers]`: each transfer is journaled as `planned`, nothing is held or sent, routing stays home placement's, and no key is needed. `sweep = "auto"` sweeps when the key is there and runs without the sweep otherwise.

## What it moves, and when

| Holding on an account other than the home | Moved |
|---|---|
| a tradable amount, unchanged for `sweep_delay_ms` | at once, the largest first within the budget |
| below the tradable minimum (dust) | after `sweep_dust_s`, or with the delay when the home holds a tradable amount |
| locked by a resting order | not moved; the order's next reprice is a cancel and a new order at the home |

Each transfer moves the account's free balance of the asset, as the engine estimates it. It is held out of that estimate from the moment the engine decides on it, so no order is sized against it. The engine confirms it from both accounts' balance updates on their user streams, or from the venue's transfer history when those do not come. A pool has one transfer of an asset in flight at a time.

While the pool sweeps, `ctx.sweeping(venue)` is true and every spot sell the routing places goes to the home. A sell that names its account goes there, as before.

## When it stops

| Event | Response |
|---|---|
| a refusal | the hold goes back; that account and asset wait `sweep_retry_s` |
| three refusals of one asset in a row for want of balance | the asset is not swept for the rest of the session |
| 5 failures or 3 lost answers in 10 minutes, or a rate-limit answer | the sweep pauses for `breaker_s`; a second trip stops it until `fastmm-ctl pool sweep on` |
| `per_day` used | no new transfers until the day's window has room |
| a transfer unresolved after `timeout_s` | logged and counted as late; it stays held while the venue's history is asked for it |

Whenever the pool does not sweep, routing is home placement's: pieces are sold first, on their own accounts.

A restart reads the transfers still in flight from `state_file`, holds them again and asks the venue for each before the sweep moves anything of that asset.

## Operate it

```bash
fastmm-ctl --name mm pool sweep pause binance   # for breaker_s
fastmm-ctl --name mm pool sweep off binance
fastmm-ctl --name mm pool sweep on binance
fastmm-ctl --name mm pool sweep BTCUSDT         # this instrument's pieces now
```

Every transfer is in the journal as a `Transfer` control with `lane` (`tools/journal_dump.py`): the engine's `request`, `planned` and `done` as records, the venue's answers as inputs, so a replay makes the same transfers at the same points. A backtest simulates the sweep on the pool's simulated accounts; `[backtest] transfer_latency_ms`, `transfer_p_fail`, `transfer_p_unknown` and `transfer_hang_ms` set how the simulated venue carries the transfers out, and `summary.json` counts them under `transfers`.
