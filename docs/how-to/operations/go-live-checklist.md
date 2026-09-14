# Go-live checklist

> **Warning.** FastMM is research software. The shipped configs point at testnets and Binance Demo
> Mode. Trading real money with it is at your own risk, and nothing in this repository is investment
> advice. Work through every item below for the exact binary, config and strategy parameters you
> intend to run, and repeat the list when any of them changes.

Tick each item only when you have seen the evidence yourself.

## The build and the strategy

- [ ] The release build passes its tests: `ctest --preset release`.
- [ ] The strategy behaves in a backtest with the parameters you will use, for example
  `./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic`
  with your `[strategy.params]`.
- [ ] It survives the simulated exchange with faults. Enable `[sim.faults]` in a copy of
  `configs/sim.toml` (market-data drop, order-channel drop, skipped depth update, delayed acks,
  rejects, rate limits; see [fastmm-sim-exchange](../../sim-exchange.md#fault-injection)) and run
  `./scripts/run-sim.sh --duration 5m --sim-config <your sim config>`. The engine log has no
  unexpected ERROR lines and the shutdown line says `cancel_all ok`.
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

- [ ] A Binance Demo or testnet session of several hours (at least 24 h is a sensible minimum,
  covering quiet and busy hours) with the same strategy and risk settings
  ([Run on a testnet](run-on-testnet.md)).
- [ ] No ERROR lines you cannot explain: `grep ' ERROR ' runs/demo-1/engine.log`. Look each one up
  in [Troubleshooting](troubleshooting.md).
- [ ] The session's PnL reconciles: `python3 tools/pnl_report.py` with account snapshots shows the
  journal and the engine agreeing with the account's trading PnL to within rounding
  ([Journals, replay and PnL](journals-replay-pnl.md#check-pnl)).
- [ ] You know what the fees do to the strategy. In our Demo session at the touch, 1640 maker fills
  cost 31.62 USDT in commission against -1.32 USDT of realised trading PnL.

## Risk limits

Every `[risk]` limit is **off** when it is `0` or missing ([Configuration](../../configuration.md#risk)).
Set each one deliberately.

- [ ] `max_order_qty` and `max_order_notional` (quote currency; BTC for Deribit options).
- [ ] `max_position` per instrument, which counts same-side open orders.
- [ ] `max_open_orders` per instrument.
- [ ] `price_collar_bps` and `fat_finger_bps`.
- [ ] `stale_md_ms`.
- [ ] `max_loss`, sized to what you accept losing in one session. When it trips, quoting stops but
  the process keeps running until you stop it ([Kill switch and shutdown](kill-switch-and-shutdown.md#what-trips-it)).
- [ ] `orders_per_sec` and `burst` below the venue's order rate limit for your account. The
  connectors also limit themselves (Bybit `orders_per_second`, Deribit `matching_engine_rate` and
  `matching_engine_burst`, Binance from `exchangeInfo`), but the risk limit is the one you control.
- [ ] `stp = true` unless you have a reason to trade against yourself.

## Keys and access

- [ ] Keys come only from the environment through `${VAR}` references; the config file contains no
  secret, and you never run with `--allow-inline-secrets`.
- [ ] The key has trading permission only: no withdrawals, no transfers.
- [ ] The key is restricted to your server's IP addresses where the venue supports it.
- [ ] One key per running engine, so that revoking one stops one engine.

## The host

- [ ] On bare metal, `[engine] cpu` and `net_cpus` pin the engine and network threads to isolated
  cores, and `spin_mode` is chosen deliberately (`busy` burns a core; `adaptive` for shared
  machines). WSL2 and laptops use `cpu = -1`.
- [ ] The system clock is synchronised (chrony or systemd-timesyncd), and the status line's
  `clock_offset_ms` stays well below `recv_window_ms`.
- [ ] The journal directory has room for the session. A one-symbol Binance Demo session wrote 23 MB
  to 32 MB per hour; check with `df -h runs`.
- [ ] The status file works: `./build/release/bin/fastmm-top --name <engine name> --once` prints a
  frame while the engine runs.
- [ ] The log goes to a file (`--log`), and warnings are mirrored to stderr (`[logging] mirror_level`).

## Stopping

- [ ] You have done a kill-switch drill with this config: a keyed session with open orders, Ctrl-C,
  `shutdown took <n> ms (cancel_all ok)`, and zero open orders confirmed on the venue.
- [ ] You know where the venue's own "cancel all" is on its website, and you have read
  [When cancel_all failed](kill-switch-and-shutdown.md#when-cancel_all-failed).
- [ ] Someone watches the first live session from start to finish.
