# Go-live checklist

Repeat this list whenever the binary, config or strategy parameters change.

## The build and the strategy

- [ ] The release build passes its tests: `ctest --preset release`.
- [ ] You have run a backtest with the parameters you will use, for example
  `./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic`
  with your `[strategy.params]`.
- [ ] It survives the simulated exchange with faults. Enable `[sim.faults]` in a copy of
  `configs/sim.toml` (market-data drop, order-channel drop, skipped depth update, delayed acks,
  rejects, rate limits; see [fastmm-sim-exchange](../../reference/sim-exchange.md#fault-injection)) and run
  `./scripts/run-sim.sh --duration 5m --sim-config <your sim config>`. Each ERROR line in the engine
  log follows from an injected fault, and the shutdown line says `cancel_all ok`.
- [ ] The strategy is deterministic: record a backtest with
  `./build/release/bin/fastmm-backtest --config <your config> --data synthetic --out - --journal-out runs/bt/session.fmj`
  and replay it with `./build/release/bin/fastmm-replay --journal runs/bt/session.fmj --verify`,
  which must exit with code 0.
- [ ] A live session replays: replay the journal of the simulator run above (the log names it,
  `journal: <path>`) with `./build/release/bin/fastmm-replay --journal <path> --verify`, which must
  print `replay MATCH` and exit with code 0
  ([Journals, replay and PnL](journals-replay-pnl.md#replay)). Do the same for a journal of the
  practice session below.

## A practice session

- [ ] A Binance Demo or testnet session of at least 24 h with the same strategy and risk settings
  ([Run on a testnet](run-on-testnet.md)).
- [ ] Each ERROR line (`grep ' ERROR ' runs/demo-1/engine.log`) has a known cause
  ([Troubleshooting](troubleshooting.md)).
- [ ] The session's PnL reconciles with the account
  ([Journals, replay and PnL](journals-replay-pnl.md#check-pnl)).
- [ ] Your PnL estimate includes fees ([example](journals-replay-pnl.md#example-binance-demo)).

## Risk limits

A `[risk]` limit is off when it is `0` or missing ([Configuration](../../reference/configuration.md#risk)).

- [ ] `max_order_qty` and `max_order_notional` (quote currency; BTC for Deribit options).
- [ ] `max_position` per instrument, which counts same-side open orders.
- [ ] `max_open_orders` per instrument.
- [ ] `price_collar_bps` and `fat_finger_bps`.
- [ ] `stale_md_ms`.
- [ ] `max_loss`, sized to what you accept losing in one session. `[engine] on_kill` decides what
  the process does when it trips ([Kill switch and shutdown](kill-switch-and-shutdown.md#after-a-kill-the-engine-trips-itself)).
- [ ] `orders_per_sec` and `burst` below the venue's order rate limit for your account. The
  connectors also limit themselves (Bybit `orders_per_second`, Deribit `matching_engine_rate` and
  `matching_engine_burst`, Binance from `exchangeInfo`).
- [ ] `stp = true` unless you intend to trade against yourself.

## Keys and access

- [ ] Keys come only from the environment ([Configuration](../../reference/configuration.md#general-rules)),
  and you never run with `--allow-inline-secrets`.
- [ ] The key has trading permission only: no withdrawals, no transfers.
- [ ] The key is restricted to your server's IP addresses where the venue supports it.
- [ ] One key per running engine, so that revoking one stops one engine.

## The host

- [ ] On bare metal, `[engine] cpu` and `net_cpus` pin the engine and network threads to isolated
  cores with `spin_mode = "busy"`. Shared machines use `"adaptive"`; WSL2 and laptops also use
  `cpu = -1`.
- [ ] The system clock is synchronised (chrony or systemd-timesyncd), and the status line's
  `clock_offset_ms` stays well below `recv_window_ms`.
- [ ] The journal directory has room for the session
  ([Journal files](journals-replay-pnl.md#journal-files)); check with `df -h runs`.
- [ ] The status file works: `./build/release/bin/fastmm-top --name <engine name> --once` prints a
  frame while the engine runs.
- [ ] The log goes to a file (`--log`), and warnings are mirrored to stderr (`[logging] mirror_level`).

## Stopping

- [ ] You have done a kill-switch drill with this config: a keyed session with open orders, Ctrl-C,
  `shutdown took <n> ms (cancel_all ok)` ([Reading the last lines](kill-switch-and-shutdown.md#reading-the-last-lines)),
  and no open orders on the venue.
- [ ] After every stop, including one that logged `cancel_all ok`, the venue's open-orders page
  shows no orders. The shutdown cancel-all sends one request per subscribed instrument and `ok`
  means those requests succeeded; orders on other instruments, orders the venue accepted after the
  request, and every order in a `--dry-run` (where the cancel-all is skipped) are not covered.
- [ ] You know where the venue's own "cancel all" is on its website, and you have read
  [When cancel_all failed](kill-switch-and-shutdown.md#when-cancel_all-failed).
- [ ] `[engine] on_kill` is set, and whatever starts `fastmm-live` alerts on exit codes 5 and 6
  ([Kill switch and shutdown](kill-switch-and-shutdown.md#after-a-kill-the-engine-trips-itself)).
- [ ] Someone watches the first live session from start to finish.
