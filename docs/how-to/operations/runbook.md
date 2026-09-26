# Operations runbook

What to run on a first deploy, what to check each day, and what to do when something fires. The limits behind these procedures are in [Running this in production](running-in-production.md); the codes are in [Errors and exit codes](../../reference/errors.md).

## First deploy

1. Build a portable tarball on a build host and copy it: `scripts/package-release.sh --preset release --out dist` writes `dist/fastmm-<version>-x86_64.tar.gz`. It holds `fastmm-live`, `fastmm-gateway`, `fastmm-ctl`, `fastmm-top`, `fastmm-pnl`, `fastmm-replay` and `fastmm-sim-itch`, the systemd units, the Prometheus alert rules, the benchmark and host scripts, and the two ITCH simulator configs. It refuses a `-march=native` build, and needs glibc at least as new as the build host's plus libssl3. Your config and `fastmm-backtest` are not in it; copy them separately.
2. Unpack it into a versioned directory and symlink it: `tar xzf fastmm-*.tar.gz -C /opt && ln -sfn /opt/fastmm-<version> /opt/fastmm`. The symlink is your rollback.
3. Tune the host as root: `scripts/host-setup.sh tune` sets hugepages, disables irqbalance, moves device interrupts to CPU 0 and sets the `performance` governor. Add `isolcpus`, `nohz_full` and `rcu_nocbs` for the engine and network cores to the kernel command line yourself and reboot.
4. Install `deploy/fastmm-live.service` ([Deploy](deploy.md#run-under-systemd)). It restarts the process after a crash or exit 4 and not after exit 2, 3, 5, 6 or 7; add an `ExecStopPost=` that alerts on `$EXIT_STATUS` in {5, 6, 7}.
5. Copy the `[engine]` table from `configs/profiles/production-latency.toml` into your config and set `cpu` and `net_cpus` to your isolated cores. Take nothing else from that file: the rest of it is simulator configuration, including literal passwords.
6. Create the journal directory on a filesystem with room for the session, and the session epoch directory: `[engine] journal_dir` and `epoch_file`.
7. Export the keys. They come from the environment only; an unset `${VAR}` is a startup error, exit code 2.
8. Run the [Go-live checklist](go-live-checklist.md) end to end, including a 24 h practice session and a kill-switch drill.

## Daily checks

Before each session:

```bash
df -h runs                                                   # room for the journal
timedatectl                                                  # clock synchronised
/opt/fastmm/bin/fastmm-live --config <your.toml> --dry-run --duration 60s
```

The dry run connects to public market data, reads no keys and sends no orders; the per-second status line must reach `md=live` and `books=n/n`.

While the session runs:

```bash
/opt/fastmm/bin/fastmm-top --name <engine name>
```

| Watch | Healthy | Act when |
|---|---|---|
| state | `running`, uptime rising | it reads `STALE`: the engine has not published for 3 s, usually a dead process |
| `KILLED (<reason>)` | absent | present: go to [When a kill fires](#when-a-kill-fires) |
| per-venue `md` / `user` / `order` | `live` | `stale` or `down` for longer than a reconnect takes |
| `books_synced` | equals `books_total` | below it: the book sync is not completing |
| `risk_rejects` by reason | flat | rising: a limit is refusing every order ([reject reasons](../../reference/errors.md#pre-trade-checks-engine)) |
| `venue_rejects` by reason | `PostOnlyWouldCross` only | anything else |
| `clock_offset_ms` | well below `recv_window_ms` and `[risk] stale_md_ms` | it approaches either |
| `reconnects`, `rest_errors`, `rate_limit_cooldowns` | flat | rising |
| realised PnL and fees | fees below realised | fees exceed realised: the strategy is paying the venue ([Economics](../../explanation/economics.md)) |

After each session:

```bash
grep ' ERROR ' runs/<session>/engine.log
python3 tools/pnl_report.py runs/<session>/session.fmj --engine-log runs/<session>/engine.log \
  --start runs/<session>/equity_start.json --end runs/<session>/equity_end.json
/opt/fastmm/bin/fastmm-replay --journal runs/<session>/session.fmj --verify
```

Every ERROR line needs a known cause ([Troubleshooting](troubleshooting.md)). The PnL report must reconcile against the account snapshots ([Check PnL](journals-replay-pnl.md#check-pnl)). The replay must print `replay MATCH`.

## When a kill fires

The reason is in the log (`kill switch engaged (<reason>, flags=<hex>)`), in `fastmm-top` and in the status file's `kill_reason`. A kill pulls the quotes and cancels the orders; the position stays. While the switch is engaged it also refuses the orders of a flatten: to work the position off in a session that is still running, clear the switch with `fastmm-ctl unkill` (or `SIGHUP`), which also resumes quoting, and run `fastmm-ctl flatten` ([Operating a running session](operate-a-running-session.md)).

| Reason | Diagnosis | Action |
|---|---|---|
| `Requested` | a signal, `--duration`, `fastmm-ctl kill` or `stop`, or a slow-tier failure | read the `shutting down (<reason>)` line above it |
| `MaxLoss` | net PnL, carried across sessions, reached `-max_loss` | stop. The trip is latched: every start exits 6 until `--clear-kill`, which arms the whole budget again. Reconcile the account and find out what the position did first |
| `TransportFull` | the outbound ring to a venue filled; messages were dropped, so the journal no longer matches what was sent | check the venue for orders the engine does not know. Raise `[engine] order_ring_bytes`; check the network thread's core |
| `JournalOverflow` | the journal ring filled; events were lost and the session is no longer replayable | raise `[engine] journal_ring_bytes`; check disk write throughput and whether `fm-journal` is starved |
| `AllVenuesKilled` | every venue with instruments was killed individually | read each venue's own reason first |
| `VenueFatal` | bad key, signature, permission or a failed authentication on one venue | the key usually cannot cancel either: cancel that venue's orders on its website, fix the key, restart |
| `VenueHardStop` | a Binance HTTP 418 IP ban; REST is stopped until the process restarts and the cancel-all fails | cancel on the website, stop the process, wait out the ban, lower the request rate before restarting |
| `DeadMansSwitchLost` | the venue refused the dead man's switch refresh for a whole window, so it may have cancelled the orders itself | check the venue's order channel and REST access ([A dead man's switch that lapses](running-in-production.md#a-dead-mans-switch-that-lapses-is-a-kill-not-a-retry)) |
| `OrderRingOverflow` | the engine did not drain a venue's order events fast enough; exit code 5 | raise `[engine] order_ring_bytes`; check that the engine thread is pinned and not starved |
| `OrderIdsExhausted` | the session's 32-bit client order id sequence ran out | restart |
| `StrategyError` | a Python hot hook raised, called `ctx.fail` or produced a non-finite value | reproduce it in a backtest from the session's journal |
| `FeedLost` | a `nasdaq_itch` feed could not rebuild its books | check `glimpse_url`, the recovery buffer size and the line's packet loss ([Venue connectors](../../reference/venues.md#startup-and-recovery)) |
| `GatewayMaxLoss`, `GatewayOperator` | `fastmm-gateway` tripped the account: `[gateway] max_loss`, or `fastmm-ctl --gateway <name> kill` | see [Run behind a gateway](run-behind-a-gateway.md) |

After any of them, confirm the venue shows no open orders before starting anything again.

## Reconcile after a crash

A crash is any stop that did not log `shutdown took <n> ms (cancel_all ok)`: `kill -9`, an OOM kill, a panic, a power loss, or an exit code 5.

Under the shipped unit a crash or an exit 4 restarts the process. The new session restores the position from the store (`[engine] restore_position`), replays the venue's executions since the last stored fill, and cancels the orders the dead process left on its first connect ([What survives a restart](running-in-production.md#1-what-survives-a-restart)). Until then those orders rest, unless the venue's dead man's switch cancels them: Binance USDⓈ-M (`dead_mans_switch_ms`, default 60 s), Bybit (`dead_mans_switch_s`, off by default), Deribit (`cancel_on_disconnect`, default on). Binance Spot has none.

When the session does not restart by itself:

1. Cancel every order on the venue's own interface if you do not restart soon.
2. Read the venue's position and balances.
3. Read what the last session recorded: `fastmm-pnl recover --engine <name>` prints its PnL, its last position per instrument and every order it still had open ([Query what you traded](query-trading-records.md)). `stopped   never recorded` confirms the crash.
4. Establish what the journal holds: `python3 tools/journal_dump.py <journal> --type OrderFill` and the tail of the file. `trailer MISSING` means the process did not close the file; with `journal_sync = "async"` up to the last 100 ms of events were never synced ([durability](running-in-production.md#the-journal-is-durable-against-a-crash-not-against-power-loss)).
5. Recompute PnL from the journal: `python3 tools/pnl_report.py <journal> --start <before.json> --end <after.json>`. Where the account and the journal disagree, the account is right.
6. Keep `[engine] epoch_file` (`runs/session_epoch`). Deleting it makes client order ids repeat across sessions.
7. Start the session. It logs `<venue>: restored position <symbol> <qty> @ <px> from the previous session`; compare that and its first status with the venue's position. A venue that cannot replay executions starts flat (`not restoring the previous position ...`).

## Roll back

1. Stop the running session with SIGTERM or `fastmm-ctl stop` and confirm `cancel_all ok` and no open orders on the venue.
2. Repoint the symlink: `ln -sfn /opt/fastmm-<previous version> /opt/fastmm`.
3. Check the config loads with the older binary: `fastmm-live --config <your.toml> --dry-run --duration 10s`. A key the older build does not know is ignored with a warning naming the key and line, so a config written for a newer build usually starts, without that feature.
4. Use the matching `fastmm-top`. The status file layout is versioned, and a mismatched reader refuses the file.
5. Use the matching `fastmm-replay` for journals from the rolled-back build. Journal format v3 tools read v1 and v2; a replay with a different binary is not expected to match ([Determinism](../../explanation/determinism.md)).
6. Keep the journals of the rolled-back session. They are the only execution record.

## Compatibility

| Artefact | Versioned by | Rule |
|---|---|---|
| Journal (`.fmj`) | a format version in the header, currently 3 | the tools read 1, 2 and 3; pre-v2 journals carry no config or engine clock and only replay as what-if runs |
| Status file | a magic number and a version field, currently 10 | `fastmm-top` refuses a file from another version; use the binary from the same build |
| Configuration | none | an unknown key is a warning, a wrong type is an error |
| Public API | version 0.2; breaking changes are listed in the CHANGELOG until 1.0 | [Public API and header tiers](../../reference/public-api.md) |
