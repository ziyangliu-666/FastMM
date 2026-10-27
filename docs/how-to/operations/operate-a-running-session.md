# Operate a running session

You need a running `fastmm-live` session and `fastmm-ctl` from the same build. Each `fastmm-ctl` call sends one command and prints the reply:

```bash
fastmm-ctl --name sim-local status
fastmm-ctl --name sim-local pull --instrument BTCUSDT
fastmm-ctl --name sim-local flatten --max-slippage-bps 30
```

The journal records every command that reaches the engine, so a replay reproduces the session ([Determinism](../../explanation/determinism.md)). `status`, `stop`, `handoff` and `ab` do not go through the control ring. `param` is checked against the strategy's schema first; the update it sends is journaled with its `--source`.

## The socket

| Property | Value |
|---|---|
| Path | `<journal_dir>/<engine name>.ctl`; `fastmm-live --control <path>` moves it, `--no-control` leaves it out |
| Type | `AF_UNIX`, `SOCK_SEQPACKET`: one datagram per request, one per reply |
| Mode | 0600, created at start, removed at shutdown |
| Reach | local only; tunnel over SSH from another host |

The socket's file mode is the only authorisation. Keep `journal_dir` out of shared directories, and run the session and `fastmm-ctl` as the same user, or give the directory a group and `chmod g+x`.

Any `SOCK_SEQPACKET` client works. `nc -U` does not, because it speaks stream and datagram sockets only:

```bash
echo status | socat - UNIX-CONNECT:runs/mm.ctl,socktype=5
```

A reply means the command is queued. The engine runs it at its next step; `fastmm-ctl status` or `fastmm-top` shows the effect.

## The commands

Replies start with `ok` or `error`, and `fastmm-ctl` exits 0 or 1, except for `status` and `help`.

| Command | What it does |
|---|---|
| `pull [--instrument SYM \| --venue NAME]` | Stops quoting and pulls the quotes, in the whole session or in one scope. Working orders that are not quotes stay. |
| `resume [--instrument SYM \| --venue NAME]` | Quotes again. Without a scope it also clears every scoped pull and stops a running flatten. |
| `kill` | Trips the global kill switch: quoting off, quotes pulled, working orders cancelled. The position stays. |
| `unkill` | Clears the kill switch and quotes again, as `SIGHUP` does, including a [latched `max_loss`](kill-switch-and-shutdown.md#the-latched-loss-budget). |
| `flatten [--instrument SYM] [--max-slippage-bps N]` | The engine works the position off itself, also while the kill switch is set ([below](#flatten)). |
| `param <name>=<value> ... [--instrument SYM] [--source WHO]` | New strategy parameters ([below](#confirm-a-parameter-change)). |
| `params [--instrument SYM]` | The parameters' values and the update counters. |
| `limits <key>=<value> ... [--force]` | New risk limits ([below](#change-a-limit)). |
| `ab`, `ab stop`, `ab promote <arm>` | Show, end or promote the A/B test ([Run an A/B test](../strategies/ab-test.md#run-it-live)). |
| `stop` | Shuts down as `SIGTERM` does: kill switch, cancel-all on every venue, exit. |
| `handoff` | `stop` for a successor waiting on the instance lock; the lock is released last ([Hand over a running session](hand-over-a-session.md)). |
| `log-reopen` | Opens the log file again, as `SIGUSR1` does ([below](#rotate-the-log)). |
| `status` | The `fastmm-top` frame plus the limits in force. |
| `help` | The command list. |

`--instrument` takes a symbol, or `<venue>:<symbol>` when a symbol trades on more than one venue. A scoped pull stays until a matching `resume`; `unkill` does not clear it.

## Confirm a parameter change

`param` validates the values before it sends anything. An unknown name, a value out of range or a failed `validate()` is refused ([Strategy parameters](../../reference/strategy-api.md#parameters)):

```bash
fastmm-ctl --name mm param half_spread_bps=99999
```

```text
error parameter 'half_spread_bps': value 99999 outside [0, 10000]
```

1. Send the change with `--wait`, so `fastmm-ctl` waits until the engine has applied it. It exits 4 if that takes longer than the wait in ms. `--source` names who asked, at most 39 bytes.

    ```bash
    fastmm-ctl --name sim-local --wait 5000 param half_spread_bps=7 --source scheduled:drain
    ```

    ```text
    ok param queued seq=1 source=scheduled:drain (every instrument)
    ok param applied seq=1
    ```

2. Read the values back:

    ```bash
    fastmm-ctl --name sim-local params
    ```

    ```text
    ok published=1 applied=1 pending=0 engine_updates=1 last_seq=1 last_origin=control last_source=scheduled:drain
    values=applied
    half_spread_bps=7
    skew_bps_per_unit=1
    quote_qty=0.001
    max_inventory=0.01
    requote_threshold_ticks=1
    pull_on_stale_ms=2000
    levels=1
    level_step_ticks=1
    ```

`published` counts the updates sent; `applied` is the sequence number of the last one the engine applied. While they differ, the values shown are the ones about to be applied (`values=published`). The same counters are under `params` in the [status file](../../reference/status-file.md), and the journal records each update ([Journal format](../../reference/journal-format.md#parameter-updates)).

A strategy that publishes its own parameters (`LiveStrategy::set_params`) numbers the updates itself. Its replies carry no `seq`, `--wait` cannot confirm them and `params` shows no values.

## Change a limit

The keys are the `[risk]` keys, `check_balance` included, and `underlying.<BASE>.max_net` for a base with a `[risk.underlying.<BASE>]` section. Keys you do not name keep their values.

```bash
fastmm-ctl --name mm limits max_position=0.5 orders_per_sec=10
```

```text
ok limits queued (2 key(s))
```

A new limit applies at the next pre-trade check. `price_collar_bps` and `fat_finger_bps` apply at each instrument's next book or trade update, when their bands are recomputed.

Turning a limit off (`0`, or `false` for `stp` and `check_balance`) needs `--force`. `burst=0` is not off: it means `orders_per_sec`.

```bash
fastmm-ctl --name mm limits max_loss=0
```

```text
error this turns off max_loss; add --force to mean it
```

A session whose instruments settle in more than one currency and that has no `[accounting]` refuses to turn on `max_loss`, `max_gross_notional` or `max_net_notional`, as it does at start-up.

Each applied change is logged (`risk limit replaced by the operator: max_loss 100 -> 0 (--force)`), journaled and stored with the values before and after. List them with `fastmm-pnl limit-changes` ([Storage](../../reference/storage.md#limit_changes)).

## Flatten

The engine runs `flatten` itself, so it works when the strategy is what broke. It goes in three steps:

1. Stops quoting in its scope, pulls those quotes and cancels the working orders there.
2. Every `[engine] flatten_interval_ms` (500 ms by default), sends a reduce-only IOC limit order for each instrument that is not flat and has no slice in flight. The price is `--max-slippage-bps` through the touch (default `[engine] flatten_slippage_bps`, 25 bps), rounded to the tick.
3. Ends when every instrument in scope is flat.

One slice per instrument is out at a time, so a position is never sold twice. A slice is capped at `[risk] max_order_qty`, so a large position takes several sweeps.

| Check | Applies to a flatten order |
|---|---|
| Global kill switch, `max_position`, self-trade prevention | no |
| Venue kill switch, price collar, fat-finger band, `max_order_qty`, `max_order_notional`, `min_notional`, rate limit | yes |

A slice a limit refuses is retried at the next sweep. If the price collar is narrower than the slippage, no slice can pass; widen it with `limits price_collar_bps=...`.

Progress is in `fastmm-ctl status` and the status file. `fastmm-top` shows `FLATTENING (1 left)`, and Prometheus gets `fastmm_flatten_state`, `fastmm_flatten_instruments_left` and `fastmm_flatten_orders_total`.

```text
flatten    state=Working instruments_left=1 orders=3
```

### When it cannot fill

| Case | What happens |
|---|---|
| A slice is rejected or does not fill | The next sweep reprices against the touch and tries again. |
| No valid book | Nothing is sent for that instrument until the book is back. |
| The rest is below the lot or `min_qty` | Logs `flatten leaves <qty> on <symbol>: below the tradable minimum` and counts the instrument as done. |
| `[engine] flatten_timeout_ms` (60000 by default) elapses | `state=TimedOut`, an ERROR line with the instruments still holding a position, no more orders; quoting stays off. Widen the slippage or collar and `flatten` again, or `resume`. |
| `flatten_timeout_ms = 0` | No deadline. |

`resume` without a scope stops a running flatten (`state=Stopped`). `kill` does not, and a `flatten` after a kill runs with the kill switch still set. The session's own shutdown (`stop`, `SIGTERM`, `--duration`) ends it.

When the engine trips its own kill switch, `fastmm-live` exits. Set `[engine] on_kill = "stay"` to keep the session running and able to take `flatten`.

### A restart during a flatten

A restart does not resume a flatten. The shutdown cancels the slice in flight, the new session quotes as `[engine] quoting_enabled` says, and the position is still there; run `flatten` again. `tools/journal_dump.py` on the old journal prints the `Flatten` command, each sweep and each order.

## Rotate the log

`fastmm-live` and `fastmm-gateway` keep `[logging] file` (or `--log`) open. After logrotate renames the file, `SIGUSR1` or `fastmm-ctl log-reopen` flushes to the renamed file and opens the path again. This avoids `copytruncate`, which loses the lines written between its copy and its truncate.

```text
/var/log/fastmm/*.log {
    daily
    rotate 14
    compress
    delaycompress
    postrotate
        systemctl kill -s USR1 'fastmm-live@*' fastmm-gateway
    endscript
}
```

If the path cannot be opened, the log keeps going to the old file, stderr says so, and `log-reopen` replies with an error.

## Behind a gateway

A strategy attached to `fastmm-gateway` has its own socket and takes every command above. The gateway's socket, `fastmm-ctl --gateway <name>`, takes `pull`, `resume`, `kill` and `clear-kill` for every attached strategy or the account ([Run behind a gateway](run-behind-a-gateway.md#control)).

## What needs a restart

The control socket changes strategy parameters, risk limits and quoting state, in scopes of session, venue or instrument. Instruments, venues, threads, ring sizes and the strategy itself need a restart. Single orders cannot be placed or cancelled from it, and `flatten` trades a position away without undoing its PnL.

Next: [Kill switch and shutdown](kill-switch-and-shutdown.md).
