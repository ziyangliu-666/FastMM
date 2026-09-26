# Operating a running session

A running `fastmm-live` session listens on a control socket. `fastmm-ctl` sends it one command and prints the reply:

```console
$ fastmm-ctl --name sim-local status
$ fastmm-ctl --name sim-local pull --instrument BTCUSDT
ok pull queued (instrument BTCUSDT)
$ fastmm-ctl --name sim-local flatten --max-slippage-bps 30
ok flatten queued (every instrument)
```

Every command except `param`, `status` and `stop` becomes a message on the engine's control ring, so the journal records it and a replay reproduces the session exactly ([Determinism](../../explanation/determinism.md)). `param` is validated against the strategy's schema on the control thread and reaches the engine as a `ParamUpdate`, which the journal records too.

## The socket

`<journal_dir>/<engine name>.ctl`, `AF_UNIX` `SOCK_SEQPACKET`, mode 0600, created when the session starts and removed when it shuts down. `fastmm-live --control <path>` moves it; `--no-control` leaves it out.

Authorisation is the file system. Whoever can write to the socket can already send the process a signal; nothing else is checked, so keep `journal_dir` out of shared directories. Run the session and `fastmm-ctl` as the same user, or give the directory a group and `chmod g+x`.

One datagram is the request, one datagram back is the reply, so any client that speaks `SOCK_SEQPACKET` works:

```console
$ echo status | socat - UNIX-CONNECT:runs/mm.ctl,socktype=5
```

(`socktype=5` is `SOCK_SEQPACKET`; `nc -U` speaks stream and datagram sockets only.)

The control thread reads the socket between its other work, every 50 ms. A command that reaches the engine does so at its next step; `fastmm-ctl` answers as soon as the command is queued, not when the engine has run it. Watch `fastmm-ctl status` or `fastmm-top` for the effect.

## The commands

| Command | What it does |
|---|---|
| `pull [--instrument SYM \| --venue NAME]` | Stops quoting and pulls the quotes. Without a scope the whole session stops quoting; with one only that instrument or venue does, and the rest keeps trading. Working orders that are not quotes stay. |
| `resume [--instrument SYM \| --venue NAME]` | The opposite. Without a scope it also clears every scoped pull and stops a running flatten. |
| `param <name>=<value> ... [--instrument SYM]` | New strategy parameters. Validated here first: an unknown name, a value out of range or a failed `validate()` is refused and nothing is published ([Strategy parameters](../../reference/strategy-api.md#parameters)). |
| `limits <key>=<value> ...` | New risk limits. The keys are the `[risk]` keys; the ones you do not name keep the values the session started with. |
| `flatten [--instrument SYM] [--max-slippage-bps N]` | The engine works the position off itself ([below](#flatten)). |
| `kill` | Trips the global kill switch: quoting off, every quote pulled, every working order cancelled. The position stays. |
| `unkill` | Clears it and resumes quoting, exactly as `SIGHUP` does, including a latched `max_loss` trip ([Kill switch and shutdown](kill-switch-and-shutdown.md#the-latched-loss-budget)). |
| `stop` | Shuts the session down: kill switch, cancel-all on every venue, exit. Exactly what `SIGTERM` does. |
| `status` | The `fastmm-top` frame plus the limits the session runs with. |
| `help` | The command list. |

`--instrument` takes a symbol, or `<venue>:<symbol>` when the same symbol trades on more than one venue. A scoped pull stays until a matching `resume`: `unkill` does not clear it.

Replies start with `ok` or `error` (`fastmm-ctl` exit code 0 or 1), except `status` and `help`.

```console
$ fastmm-ctl --name mm param half_spread_bps=99999
error parameter 'half_spread_bps': value 99999 outside [0, 10000]
$ fastmm-ctl --name mm limits max_position=0.5 orders_per_sec=10
ok limits queued (2 key(s))
```

New limits take effect on the next pre-trade check, except `price_collar_bps` and `fat_finger_bps`, whose bands are recomputed on the next book or trade update of each instrument.

## Flatten

`flatten` is the engine's own: it does not ask the strategy, because the strategy may be what broke. It

1. stops quoting in its scope, pulls those quotes and cancels the working orders there (with `--instrument` only that instrument; without it the whole session);
2. every `[engine] flatten_interval_ms` (default 500) looks at the position of each instrument in scope and, when one is not flat and has no slice in flight, sends a **reduce-only IOC limit order** for the whole remaining position, priced `--max-slippage-bps` (default `[engine] flatten_slippage_bps`, 25) through the touch: selling at `best_bid - bps`, buying at `best_ask + bps`, rounded back to the tick;
3. ends when every instrument in scope is flat.

Only one slice per instrument is out at a time, so a flatten cannot sell the same position twice. A slice is capped at `[risk] max_order_qty`, so a large position leaves over several sweeps.

A flatten order is exempt from `[risk] max_position` (a reduce-only order exists to get under it) and from self-trade prevention (the flatten cancelled the orders of its scope first; a cancel the venue never acknowledges must not leave the position on). Everything else applies: the **price collar**, the fat-finger band, `max_order_qty`, `max_order_notional`, `min_notional` and the rate limit. A slice a limit refuses is simply retried at the next sweep — which is why `limits price_collar_bps=...` exists: if the collar is narrower than the slippage you asked for, the flatten cannot price an order at all and will never finish.

Progress is in the status file and in `fastmm-ctl status`:

```text
flatten    state=Working instruments_left=1 orders=3
```

`fastmm-top` shows `FLATTENING (1 left)` next to the state, and Prometheus gets `fastmm_flatten_state`, `fastmm_flatten_instruments_left` and `fastmm_flatten_orders_total`.

### When it cannot fill

| Case | What happens |
|---|---|
| A slice is rejected (risk, rate limit, the venue) or does not fill | The next sweep reprices against the current touch and tries again. |
| No valid book for an instrument | Nothing is sent for it; the sweeps keep running and it resumes when the book is back. |
| What is left is below the lot or `min_qty` | No order can move it. The flatten logs `flatten leaves <qty> on <symbol>: below the tradable minimum` and treats that instrument as done. |
| `[engine] flatten_timeout_ms` (default 60000) elapses with a position left | The flatten **gives up**: `state=TimedOut`, an ERROR line naming how many instruments still hold a position, no further orders. Quoting stays off in its scope, so the strategy does not start trading into whatever broke; it is now an operator's position. Widen the slippage or the collar and run `flatten` again, or `resume` to give the session back to the strategy. |
| `flatten_timeout_ms = 0` | No deadline: it keeps trying until it is flat or an operator stops it. |

`resume` (without a scope) stops a running flatten (`state=Stopped`) and hands the instruments back to the strategy. `kill` does not stop it: the kill switch refuses new orders, so the flatten's slices are rejected until you `unkill`. Flatten first, then kill.

### A restart during a flatten

The flatten is session state and is not persisted. A restart does not resume it: the shutdown cancels the slice that was in flight, the new session starts with quoting as `[engine] quoting_enabled` and the configuration say, and the position is still there. After a restart, check the position and issue `flatten` again if you want it worked off. The previous session's journal holds the whole story: `tools/journal_dump.py` prints the `Flatten` command, every sweep and every order it sent.

## Behind a gateway

A strategy attached to `fastmm-gateway` has its own socket and takes every command above. The gateway has one too, `fastmm-ctl --gateway <name>`, whose `pull`, `resume`, `kill` and `clear-kill` act on every attached strategy or the account ([Run behind a gateway](run-behind-a-gateway.md#control)).

## What an operator cannot do

- Change anything that is not a strategy parameter or a risk limit. Instruments, venues, threads, ring sizes and the strategy itself need a restart; `ControlCommand::Reload` is still not implemented.
- Cancel one order, or place one. The control plane works in scopes (session, venue, instrument), not in single orders.
- Reach the engine from another host. The socket is local and has no authentication of its own; tunnel over SSH if you need it from elsewhere.
- Undo a fill. `flatten` trades the position away at the market; it does not restore the PnL.

## See also

- [Kill switch and shutdown](kill-switch-and-shutdown.md) — what trips the kill switch, the latched loss budget, the shutdown sequence.
- [Monitoring a live session](monitor-with-fastmm-top.md) — the status file and Prometheus.
- [Running this in production](running-in-production.md) — what still needs a human.
