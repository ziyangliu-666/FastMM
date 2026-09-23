# Operations runbook

What to run on a first deploy, what to check each day, and what to do when something fires. The limits behind these procedures are in [Running this in production](running-in-production.md); the codes are in [Errors and exit codes](../../reference/errors.md).

## First deploy

1. Build a portable tarball on a build host and copy it: `scripts/package-release.sh --preset release --out dist` writes `dist/fastmm-<version>-x86_64.tar.gz`. It holds four binaries (`fastmm-live`, `fastmm-replay`, `fastmm-top`, `fastmm-sim-itch`), the benchmark and host scripts, and the two ITCH simulator configs. It refuses a `-march=native` build, and needs glibc at least as new as the build host's plus libssl3. Your own config and `fastmm-backtest` are not in it; copy them separately.
2. Unpack it into a versioned directory and symlink it: `tar xzf fastmm-*.tar.gz -C /opt && ln -sfn /opt/fastmm-<version> /opt/fastmm`. The symlink is your rollback.
3. Tune the host as root: `scripts/host-setup.sh tune` sets hugepages, disables irqbalance, moves device interrupts to CPU 0 and sets the `performance` governor. Add `isolcpus`, `nohz_full` and `rcu_nocbs` for the engine and network cores to the kernel command line yourself and reboot.
4. Write the service unit. The repository ships none. `Restart=no`, `LimitMEMLOCK=infinity` if you set `[engine] lock_memory = true`, `Environment=` or `EnvironmentFile=` for the API keys, and an `ExecStopPost=` that alerts on `$EXIT_STATUS` in {5, 6, 7}.
5. Copy the `[engine]` table from `configs/profiles/production-latency.toml` into your config and set `cpu` and `net_cpus` to your isolated cores. Take nothing else from that file: the rest of it is simulator configuration, including literal passwords.
6. Create the journal directory on a filesystem with room for the session, and the session epoch directory: `[engine] journal_dir` and `epoch_file`.
7. Export the keys. They come from the environment only, and an unset `${VAR}` is a startup error, exit code 2.
8. Run the [Go-live checklist](go-live-checklist.md) end to end, including a 24 h practice session and a kill-switch drill.

## Daily checks

Before each session:

```bash
df -h runs                                                   # room for the journal
timedatectl                                                  # clock synchronised
/opt/fastmm/bin/fastmm-live --config <your.toml> --dry-run --duration 60s
```

The dry run connects to public market data, reads no keys and sends no orders; the per-second status line should reach `md=live` and `books=n/n`.

Then check the venue's own page shows no open orders, because no connector except Binance USDⓈ-M reconciles at startup.

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
| `clock_offset_ms` | well below `recv_window_ms` and below `[risk] stale_md_ms` | it approaches either |
| `reconnects`, `rest_errors`, `rate_limit_cooldowns` | flat | rising |
| realised PnL and fees | fees below realised | fees exceed realised: the strategy is paying the venue ([Economics](../../explanation/economics.md)) |

After each session:

```bash
grep ' ERROR ' runs/<session>/engine.log
python3 tools/pnl_report.py runs/<session>/session.fmj --engine-log runs/<session>/engine.log \
  --start runs/<session>/equity_start.json --end runs/<session>/equity_end.json
/opt/fastmm/bin/fastmm-replay --journal runs/<session>/session.fmj --verify
```

Every ERROR line should have a known cause ([Troubleshooting](troubleshooting.md)). The PnL report must reconcile against the account snapshots ([Check PnL](journals-replay-pnl.md#check-pnl)). The replay must print `replay MATCH`.

## When a kill fires

The reason is in the log (`kill switch engaged (<reason>, flags=<hex>)`), in `fastmm-top` and in the status file's `kill_reason`. A kill pulls the quotes and cancels the orders; the position stays. `fastmm-ctl unkill` (or `SIGHUP`) clears the switch of a session that is still running, and `fastmm-ctl flatten` works the position off — flatten first, because while the switch is engaged the pre-trade check refuses the flatten's orders too ([Operating a running session](operate-a-running-session.md)).

| Reason | Diagnosis | Action |
|---|---|---|
| `Requested` | a signal, `--duration` or a slow-tier failure; the session was shutting down anyway | read the `shutting down (<reason>)` line above it |
| `MaxLoss` | net PnL reached `-max_loss` for this process | stop. Reconcile the account, find out what the position did, decide whether to restart. A restart gets a fresh full budget ([per process](running-in-production.md#the-loss-budget-is-per-process)) |
| `TransportFull` | the outbound ring to a venue filled; messages were dropped, so the journal no longer matches what was sent | check the venue for orders the engine does not know. Raise `[engine] order_ring_bytes`; check the network thread's core |
| `JournalOverflow` | the journal ring filled; events were lost and the session is no longer replayable | raise `[engine] journal_ring_bytes`; check disk write throughput and whether `fm-journal` is starved |
| `AllVenuesKilled` | every venue with instruments was killed individually | read each venue's own reason first |
| `VenueFatal` | bad key, signature, permission or a failed authentication on one venue | the key usually cannot cancel either: cancel that venue's orders on its website, fix the key, restart |
| `VenueHardStop` | a Binance HTTP 418 IP ban; REST is stopped until the process restarts and the cancel-all will fail | cancel on the website, stop the process, wait out the ban, then lower the request rate before restarting |
| `OrderRingOverflow` | the engine did not drain a venue's order events fast enough; the session exits with code 5 | raise `[engine] order_ring_bytes`; check that the engine thread is pinned and not starved |
| `StrategyError` | a Python hot hook raised, called `ctx.fail` or produced a non-finite value | reproduce it in a backtest from the session's journal |
| `FeedLost` | a `nasdaq_itch` feed could not rebuild its books | check `glimpse_url`, the recovery buffer size and the line's packet loss ([Venue connectors](../../reference/venues.md#startup-and-recovery)) |

After any of them, confirm the venue shows no open orders before starting anything again.

## Reconcile after a crash

A crash is any stop that did not log `shutdown took <n> ms (cancel_all ok)`: `kill -9`, an OOM kill, a panic, a power loss, or an exit code 5.

1. Cancel every order on the venue's own interface. Only Deribit arms venue-side cancel-on-disconnect; on Binance and Bybit your orders are still resting.
2. Read the venue's position and balances, and write them down. The engine's position is gone.
3. Read what the last session recorded: `/opt/fastmm/bin/fastmm-pnl recover --engine <name>` prints its PnL, its last position per instrument and every order it still had open ([Query what you traded](query-trading-records.md)). `stopped never recorded` confirms the crash.
4. Establish what the journal holds: `python3 tools/journal_dump.py <journal> --type OrderFill` and the tail of the file. `trailer MISSING` means the process did not close the file, and up to the last 100 ms of events were never synced ([durability](running-in-production.md#the-journal-is-durable-against-a-crash-not-against-power-loss)).
5. Recompute PnL from the journal: `python3 tools/pnl_report.py <journal> --start <before.json> --end <after.json>`. Where the account and the journal disagree, the account is right: fills that arrived while the private stream was down are not in the journal either.
6. Decide what to do with the inherited position before restarting. The restarted engine will quote as if flat.
7. Keep `[engine] epoch_file` (`runs/session_epoch`). Deleting it makes client order ids repeat across sessions.
8. Set `[risk] max_position` and `max_loss` for the restarted session with the inherited position in mind.

## Roll back

1. Stop the running session with SIGTERM or `fastmm-ctl stop` and confirm `cancel_all ok` and no open orders on the venue.
2. Repoint the symlink: `ln -sfn /opt/fastmm-<previous version> /opt/fastmm`.
3. Check the config still loads against the older binary: `fastmm-live --config <your.toml> --dry-run --duration 10s`. A configuration key the older build does not know is ignored with a warning naming the key and line, so a config written for a newer build usually starts, silently without that feature.
4. Use the matching `fastmm-top`. The status file layout is versioned, and a mismatched reader refuses the file rather than showing wrong numbers.
5. Use the matching `fastmm-replay` for journals from the rolled-back build. Journal format v3 tools read v1 and v2; a replay with a different binary is not expected to match ([Determinism](../../explanation/determinism.md)).
6. Keep the journals of the rolled-back session. They are the only execution record.

## Compatibility

| Artefact | Versioned by | Rule |
|---|---|---|
| Journal (`.fmj`) | a format version in the header, currently 3 | the tools read 1, 2 and 3; pre-v2 journals carry no config or engine clock and only replay as what-if runs |
| Status file | a magic number and a version field, currently 4 | `fastmm-top` refuses a file from another version; use the binary from the same build |
| Configuration | none | an unknown key is a warning, a wrong type is an error |
| Public API | version 0.1; the API may change | [Public API and header tiers](../../reference/public-api.md) |
