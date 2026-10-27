# Go-live checklist

Work through this list before the first live session, and again whenever the binary, the config or the strategy parameters change. Background: [Run in production](running-in-production.md); the economics of the shipped strategies: [Economics](../../explanation/economics.md).

## The build and the strategy

- [ ] The release build passes its tests: `ctest --preset release`.
- [ ] A backtest with your `[strategy.params]` gives results you accept, for example `./build/release/bin/fastmm-backtest --config configs/backtest-example.toml --data synthetic`.
- [ ] The strategy survives the simulated exchange with faults ([fault injection](../../reference/sim-exchange.md#fault-injection)):
    1. Enable `[sim.faults]` in a copy of `configs/sim.toml`.
    2. Run `./scripts/run-sim.sh --duration 5m --sim-config <your sim config>`.
    3. Each ERROR line in the engine log follows from an injected fault, and the shutdown line says `cancel_all ok`.
- [ ] A restart restores the position: `kill -9` a keyed session that holds a position and start it again. It logs `restored position`, ends at the venue's position, and leaves none of the dead process's orders ([Restarts](running-in-production.md#restarts)).
- [ ] The strategy is deterministic. Record a backtest, then replay it; `--verify` exits 0:

    ```bash
    ./build/release/bin/fastmm-backtest --config <your config> --data synthetic --out - --journal-out runs/bt/session.fmj
    ./build/release/bin/fastmm-replay --journal runs/bt/session.fmj --verify
    ```

- [ ] A live session replays. Take the journal the simulator run logs as `journal: <path>`, run `./build/release/bin/fastmm-replay --journal <path> --verify`, and see `replay MATCH` with exit code 0 ([Replay](journals-replay-pnl.md#replay)). Do the same for the practice session below.

## A practice session

- [ ] A Binance Demo or testnet session of at least 24 h with the same strategy and risk settings ([Run on a testnet](run-on-testnet.md)).
- [ ] Each ERROR line has a known cause: `grep ' ERROR ' runs/demo-1/engine.log` ([Troubleshooting](troubleshooting.md)).
- [ ] The session's PnL reconciles with the account ([Check PnL](journals-replay-pnl.md#check-pnl)).
- [ ] Your PnL estimate includes fees ([example](journals-replay-pnl.md#example-binance-demo)).

## Risk limits

A `[risk]` limit is off when it is `0` or missing ([Configuration](../../reference/configuration.md#risk)). A session with keys does not start without `max_loss` and one of `max_position` or `max_order_notional`.

| Set | Note |
|---|---|
| `max_order_qty`, `max_order_notional` | in the settlement currency: the base coin for an inverse contract |
| `max_position` | per instrument; counts same-side open orders |
| `max_open_orders` | per instrument |
| `price_collar_bps`, `fat_finger_bps` | refuse prices too far from the mid and from the last trade |
| `stale_md_ms` | refuse orders while the book is older than this |
| `max_gross_notional`, `max_net_notional` | across instruments |
| `max_loss` | what you accept losing. The budget carries across restarts until `--clear-kill` ([The latched loss budget](kill-switch-and-shutdown.md#the-latched-loss-budget)) |
| `[engine] on_kill` | what the process does when a limit trips ([After a kill](kill-switch-and-shutdown.md#after-a-kill-the-engine-trips-itself)) |
| `[accounting]` | with more than one settlement currency: the reporting currency and a rate source per other currency. Without it, a start with `max_loss` exits 3 ([Configuration](../../reference/configuration.md#accounting)) |
| `orders_per_sec`, `burst` | below your account's order rate limit at the venue. Connectors also limit themselves: Bybit `orders_per_second`, Deribit `matching_engine_rate` and `matching_engine_burst`, Binance from `exchangeInfo` |
| `stp = true` | unless you trade against yourself on purpose |

## Keys and access

- [ ] Keys come only from the environment ([Configuration](../../reference/configuration.md#general-rules)); never run with `--allow-inline-secrets`.
- [ ] The key can trade and nothing else: no withdrawals, no transfers.
- [ ] The account or subaccount is the session's alone. The session cancels unknown open orders on its symbols, and its cancel-alls take every order there ([`cancel_unknown`](../../reference/configuration.md#engine)).
- [ ] The key is restricted to your server's IP addresses where the venue supports it.
- [ ] Each running engine has its own key, so revoking one stops one engine.

## The host

- [ ] Threads and spin mode match the machine ([Cores](running-in-production.md#cores)), and the `topology:` lines at start show no warning:

    | Host | `[engine]` |
    |---|---|
    | bare metal | `cpu` and `net_cpus` on isolated cores, `spin_mode = "busy"` |
    | shared machine | `spin_mode = "adaptive"` |
    | WSL2, laptop | `spin_mode = "adaptive"`, `cpu = -1` |

    `fastmm init --profile production` writes a Binance Spot config for a dedicated host. `configs/profiles/production-latency.toml` has the same `[engine]` table: busy spinning on isolated cores, `timer_slack_ns = 1`, `lock_memory = true` and a larger journal ring. `lock_memory` needs a raised `ulimit -l`, or `LimitMEMLOCK=infinity` under systemd.

- [ ] The engine and network cores are isolated with `isolcpus`, `nohz_full` and `rcu_nocbs`, or a cpuset.
- [ ] The CPU governor is `performance`, and NIC interrupts go to other cores.
- [ ] Transparent huge pages are `madvise` or `always` in `/sys/kernel/mm/transparent_hugepage/enabled`: the order and book tables ask for 2 MiB pages.
- [ ] Latency figures in `fastmm-top` and the log are not zero. Hosts without `nonstop_tsc`, such as many cloud VMs, take wall time from `clock_gettime` and still time intervals with the TSC when `constant_tsc` is present.
- [ ] The system clock is synchronised by chrony or systemd-timesyncd, and the status line's `clock_offset_ms` stays well below `recv_window_ms`.
- [ ] The journal directory has room for the session: `df -h runs` ([Journal files](journals-replay-pnl.md#journal-files)).
- [ ] `./build/release/bin/fastmm-top --name <engine name> --once` prints a frame while the engine runs.
- [ ] The log goes to a file (`--log`), and warnings are mirrored to stderr by `[logging] mirror_level`.

## Stopping

- [ ] You have done a kill-switch drill with this config: a keyed session with open orders, Ctrl-C, the line `shutdown took <n> ms (cancel_all ok)` ([Reading the last lines](kill-switch-and-shutdown.md#reading-the-last-lines)), and no open orders left at the venue.
- [ ] After every stop, the venue's open-orders page shows no orders, even after `cancel_all ok`. The shutdown sends one cancel-all per subscribed instrument; `ok` does not cover other instruments, orders accepted after the request, or a `--dry-run`, which skips it.
- [ ] You know where the venue's own "cancel all" is on its website, and you have read [When cancel_all failed](kill-switch-and-shutdown.md#when-cancel_all-failed).
- [ ] `[engine] on_kill` is set, and whatever starts `fastmm-live` or `python -m fastmm run` alerts on exit codes 5, 6 and 7. The shipped systemd unit does not restart them ([Deploy](deploy.md#run-under-systemd)).
- [ ] Someone watches the first live session from start to finish.
