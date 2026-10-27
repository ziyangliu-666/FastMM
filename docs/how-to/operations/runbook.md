# Operations runbook

What to run on a first deploy, what to check each day, and what to do when something fires. Background is in [Run in production](running-in-production.md); exit codes are in [Errors and exit codes](../../reference/errors.md).

## First deploy

1. On a build host, build the portable tarball. Copy it, your config and `fastmm-backtest` (not in the tarball) to the trading host.

    ```bash
    scripts/package-release.sh --preset release --out dist
    ```

    The tarball `dist/fastmm-<version>-x86_64.tar.gz` holds the live programs, the simulators, the systemd units, the Prometheus alert rules and the host scripts. It refuses a `-march=native` build and needs glibc at least as new as the build host's (2.28 with `--container`); OpenSSL and libstdc++ are linked in.

2. Unpack it into a versioned directory and point a symlink at it. The symlink is your rollback.

    ```bash
    tar xzf fastmm-*.tar.gz -C /opt && ln -sfn /opt/fastmm-<version>-x86_64 /opt/fastmm
    ```

3. Tune the host as root. `tune` sets hugepages, disables irqbalance, moves device interrupts to CPU 0 and sets the `performance` governor.

    ```bash
    scripts/host-setup.sh tune
    ```

    Add `isolcpus`, `nohz_full` and `rcu_nocbs` for the engine and network cores to the kernel command line yourself, and reboot.

4. Install `deploy/fastmm-live.service` ([Deploy](deploy.md#run-under-systemd)). It restarts after a crash or exit 4, not after exit 2, 3, 5, 6 or 7. Add an `ExecStopPost=` that alerts on `$EXIT_STATUS` in {5, 6, 7}.
5. Copy the `[engine]` table from `configs/profiles/production-latency.toml` into your config and set `cpu` and `net_cpus` to your isolated cores. Take nothing else from that file: the rest is simulator configuration with literal passwords.
6. Size `[risk]` to the account. Every limit is off by default. A session with keys exits 3 without `max_loss` and one of `max_position` or `max_order_notional`.
7. Give the session an account of its own. It cancels every open order it does not know on its symbols at start-up and after a reconnect ([`cancel_unknown`](../../reference/configuration.md#engine)).
8. Create `[engine] journal_dir` on a filesystem with room for the session, and the directory of `[engine] epoch_file`.
9. Export the keys. They come from the environment only; an unset `${VAR}` exits 2.
10. Run the [Go-live checklist](go-live-checklist.md) end to end, including a 24 h practice session and a kill-switch drill.

## Daily checks

### Before each session

```bash
df -h runs                                                   # room for the journal
timedatectl                                                  # clock synchronised
/opt/fastmm/bin/fastmm-live --config <your.toml> --dry-run --duration 60s
```

The dry run reads public market data with no keys and sends no orders. Its status line must reach `md=live` and `books=n/n`, and the `dry run <symbol>` lines must show the quotes you expect.

### While it runs

```bash
/opt/fastmm/bin/fastmm-top --name <engine name>
```

| Watch | Healthy | Act when |
|---|---|---|
| state | `running`, uptime rising | `STALE`: no status for 3 s, usually a dead process |
| `KILLED (<reason>)` | absent | present: [When a kill fires](#when-a-kill-fires) |
| per-venue `md`, `user`, `order` | `live` | `stale` or `down` for longer than a reconnect takes |
| `books_synced` | equals `books_total` | below it: book sync is not completing |
| `risk_rejects` by reason | flat | rising: a limit refuses every order ([reject reasons](../../reference/errors.md#pre-trade-checks-engine)) |
| `venue_rejects` by reason | `PostOnlyWouldCross` only | anything else |
| `clock_offset_ms` | well below `recv_window_ms` and `[risk] stale_md_ms` | it approaches either |
| `reconnects`, `rest_errors`, `rate_limit_cooldowns` | flat | rising |
| `fill_audit_missing`, `_phantom`, `_mismatched` | 0 | any: the `fill audit` log lines name the executions |
| `store` line | absent | present: the store failed writes or dropped records; the first `store:` ERROR says why |
| realised PnL and fees | fees below realised | fees above realised: the strategy is paying the venue ([Economics](../../explanation/economics.md)) |

`[storage] kill_on_failure = true` turns a store failure into a kill.

### After each session

```bash
grep ' ERROR ' runs/<session>/engine.log
python3 tools/pnl_report.py runs/<session>/session.fmj --engine-log runs/<session>/engine.log \
  --start runs/<session>/equity_start.json --end runs/<session>/equity_end.json
/opt/fastmm/bin/fastmm-replay --journal runs/<session>/session.fmj --verify
```

| Check | Pass |
|---|---|
| ERROR lines | each has a known cause ([Troubleshooting](troubleshooting.md)) |
| PnL report | reconciles with the account snapshots ([Check PnL](journals-replay-pnl.md#check-pnl)) |
| Replay | prints `replay MATCH` |

## When a kill fires

The reason is in the log (`kill switch engaged (<reason>, flags=<hex>)`), in `fastmm-top` and in the status file's `kill_reason`. A kill pulls the quotes and cancels the orders; the position stays.

If the session is still running (`fastmm-ctl kill`, or `[engine] on_kill = "stay"`), `fastmm-ctl flatten` works the position off with the kill switch still set ([Flatten](operate-a-running-session.md#flatten)). A venue's own kill (`VenueFatal`, `VenueHardStop`, `DeadMansSwitchLost`) refuses the flatten on that venue.

| Reason | Cause | Action |
|---|---|---|
| `Requested` | a signal, `--duration`, `fastmm-ctl kill` or `stop`, or a slow-tier failure | read the `shutting down (<reason>)` line above it |
| `MaxLoss` | net PnL, carried across sessions, reached `-max_loss` | stop. The trip is latched: every start exits 6 until `--clear-kill`, which re-arms the whole budget. Reconcile the account first |
| `TransportFull` | the outbound ring to a venue filled and dropped messages; the journal no longer matches what was sent | look for orders at the venue the engine does not know; raise `[engine] order_ring_bytes`; check the network thread's core |
| `JournalOverflow` | the journal ring filled; the session is no longer replayable | raise `[engine] journal_ring_bytes`; check disk throughput and whether `fm-journal` is starved |
| `AllVenuesKilled` | every venue with instruments was killed | read each venue's reason |
| `VenueFatal` | bad key, signature or permission on one venue | the key usually cannot cancel either: cancel on the venue's website, fix the key, restart |
| `VenueHardStop` | a Binance HTTP 418 IP ban; REST stops until restart and the cancel-all fails | cancel on the website, stop, wait out the ban, lower the request rate |
| `DeadMansSwitchLost` | the venue refused the dead man's switch refresh for a whole window | check the order channel and REST ([A lapsed switch is a kill](running-in-production.md#a-lapsed-switch-is-a-kill)) |
| `OrderRingOverflow` | the engine did not drain a venue's order events; exit 5 | raise `[engine] order_ring_bytes`; check the engine thread is pinned |
| `OrderIdsExhausted` | the 32-bit client order id sequence ran out | restart |
| `StrategyError` | a Python hot hook raised, called `ctx.fail` or produced a non-finite value | reproduce it in a backtest from the journal |
| `FeedLost` | a `nasdaq_itch` feed could not rebuild its books | check `glimpse_url`, the recovery buffer and packet loss ([Venue connectors](../../reference/venues/nasdaq-itch.md#startup-and-recovery)) |
| `GatewayMaxLoss`, `GatewayOperator` | `fastmm-gateway` tripped the account | [Run behind a gateway](run-behind-a-gateway.md) |
| `StoreFailure` | the store failed and `[storage] kill_on_failure = true` | the first `store:` ERROR names the SQLite error. For dropped records raise `[storage] ring_bytes`. The next start restores from an incomplete store: compare its position with the venue's |

After any kill, confirm the venue shows no open orders before starting again.

## Reconcile after a crash

A crash is any stop that did not log `shutdown took <n> ms (cancel_all ok)`: `kill -9`, an OOM kill, a panic, a power loss or exit 5.

Under the shipped unit a crash restarts the process. The new session restores the position from the store, replays the venue's executions since the last stored fill, and cancels the dead process's orders on first connect ([What survives a restart](running-in-production.md#restarts)).

Until then the old orders rest, unless the venue's dead man's switch cancels them:

| Venue | Setting | Default |
|---|---|---|
| Binance USDⓈ-M | `dead_mans_switch_ms` | 60 s |
| Bybit | `dead_mans_switch_s` | off |
| Deribit | `cancel_on_disconnect` | on |
| Binance Spot | none | |

When the session does not restart by itself:

1. If you will not restart soon, cancel every order on the venue's own interface.
2. Read the venue's position and balances.
3. Read what the last session recorded: its PnL, last position per instrument and open orders ([Query what you traded](query-trading-records.md)). `stopped   never recorded` confirms the crash.

    ```bash
    fastmm-pnl recover --engine <name>
    ```

4. Check what the journal holds. `trailer MISSING` means the process did not close the file; with `journal_sync = "async"` up to the last 100 ms of events were never synced ([durability](running-in-production.md#durability)).

    ```bash
    python3 tools/journal_dump.py <journal> --type OrderFill
    ```

5. Recompute PnL from the journal. Where the account and the journal disagree, the account is right.

    ```bash
    python3 tools/pnl_report.py <journal> --start <before.json> --end <after.json>
    ```

6. Keep `[engine] epoch_file` and `[engine] kill_file`. A start without them while the store holds sessions is refused ([below](#a-start-refuses-the-state-it-carries-over)).
7. Start the session. Compare its `<venue>: restored position <symbol> <qty> @ <px> from the previous session` line and first status with the venue. A venue that cannot replay executions starts flat (`not restoring the previous position ...`).

## A start refuses the state it carries over

Before it trades, `fastmm-live` checks the epoch file, the kill file and the store against each other. A refusal prints one line to stderr and sends nothing.

| Exit | Message starts | Cause | Action |
|---:|---|---|---|
| 9 | `[engine] epoch_file <path> is missing, and the store holds sessions of this engine up to epoch <n>` | a new counter would reuse the stored sessions' client order ids | restore the file, or write the newest epoch of every engine sharing it: `echo <n> > <path>` |
| 6 | `[engine] kill_file <path> is missing, and the store's newest session ... ended on a max_loss trip` | the kill file was lost after a latched trip | reconcile the account, then start with `--clear-kill` |
| 9 | `the store's newest session (<id>) booked executions more than once (<n> <venue> <symbol>; ...)` | its stored position counts some executions twice, and the venue does not report positions before trading | see below |

For the duplicate-execution refusal:

1. List the duplicates: `fastmm-pnl duplicates --engine <name>`.
2. Compare the account's balance with `fastmm-pnl recover --engine <name>`.
3. Start with `--accept-position` to trade from the stored position, or with `[engine] restore_position = false` to start flat.

It applies to spot instruments and to everything behind `fastmm-gateway`. A derivative on a venue that reports positions (Binance USDⓈ-M, Bybit linear, OKX, Gate, Deribit, Hyperliquid, Lighter, Gemini perpetuals) is not refused: the venue's position replaces the stored one. Only the newest session is checked, so the session after an accepted or flat start runs without the flag.

By default the epoch file, kill file and store all live under `runs/`. Losing that directory makes the next start a first start, with nothing to check; set `[storage] path` elsewhere to keep the store.

## Roll back

1. Stop the session with `SIGTERM` or `fastmm-ctl stop`. Confirm `cancel_all ok` and no open orders on the venue.
2. Repoint the symlink:

    ```bash
    ln -sfn /opt/fastmm-<previous version>-x86_64 /opt/fastmm
    ```

3. Check the config loads with the older binary. An unknown key is an error, so remove keys the older build does not know.

    ```bash
    /opt/fastmm/bin/fastmm-live --config <your.toml> --dry-run --duration 10s
    ```

4. Use the `fastmm-top` and `fastmm-replay` of the same build. A replay with a different binary is not expected to match ([Determinism](../../explanation/determinism.md)).
5. Keep the rolled-back session's journals: they are its only execution record.

## Compatibility

| Artefact | Versioned by | Rule |
|---|---|---|
| Journal (`.fmj`) | format version in the header, now 5 | the tools read 1 to 5; pre-v2 journals carry no config or engine clock and replay only as what-if runs |
| Status file | magic number and version | `fastmm-top` refuses a file from another version |
| Configuration | none | an unknown key or a wrong type is an error |
| Public API | breaking changes listed in the CHANGELOG until 1.0 | [Public API and header tiers](../../reference/public-api.md) |
