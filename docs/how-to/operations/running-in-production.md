# Running this in production

What an operator will hit, roughly in order of cost, with the code that decides each behaviour and the mitigation where one exists. Read [Economics of the shipped strategies](../../explanation/economics.md) first: the shipped strategies lose money at any positive maker fee.

Every shipped config points at a testnet, Demo Mode or a local simulator. There is no main-net config in the repository.

## 1. What survives a restart

| State | After a restart |
|---|---|
| Position | restored from the store (`[engine] restore_position`, default on), then brought up to date by the venue's executions since the last stored fill, counted in the venue's clock: fills of orders that were resting when the process died, and trades made on the account outside FastMM |
| Orders the dead process left | every connector sweeps the venue's open orders on its first connect; their ids belong to an earlier session epoch, so the engine cancels them (`cancelling unknown live order <id>`) |
| Kill switch, `[risk] max_loss` budget | carried in `<journal_dir>/<name>.kill`: a latched kill exits 6 at every start until cleared, and the loss budget is for the deployment, not the process |
| Client order id sequence | continues: `[engine] epoch_file` (default `runs/session_epoch`) keeps ids unique; keep the file |
| Realised PnL of the session | starts at zero; the previous session's is logged at start and kept in the store |
| A running flatten | abandoned ([Operating a running session](operate-a-running-session.md#a-restart-during-a-flatten)) |

The shipped systemd unit therefore restarts after a crash ([Deploy](deploy.md#run-under-systemd)). Shown against `fastmm-sim-exchange`: `kill -9` while quoting, restart after 2 s, the one order that filled in between booked from the executions, the other cancelled as unknown, and engine and venue at the same position with no open orders.

## 2. Orders the engine cannot see

A process that is gone cancels nothing. Its orders rest until the next session's start-up sweep, or until the venue's own switch clears them.

### Binance Spot has no dead man's switch, and Bybit's is not self-serve

`cancel_on_order_channel_loss = true` (every connector, default on) is a REST cancel-all the connector issues when its order channel drops. It needs the process alive and the network working, so it does nothing after `kill -9`, an OOM kill, a kernel panic or a power loss. Then only the venue can clear the orders:

| venue | venue-side switch | default | after a hard kill |
|---|---|---|---|
| Binance USDⓈ-M | `POST /fapi/v1/countdownCancelAll` per symbol, refreshed every third of `dead_mans_switch_ms` | 60 s | orders gone within the window |
| Deribit | `private/enable_cancel_on_disconnect`, scope `connection`, plus `public/set_heartbeat` | on, 10 s heartbeat | orders gone once the heartbeat misses |
| Bybit | `POST /v5/order/disconnected-cancel-all` (product `SPOT`, or `DERIVATIVES` for `category = "linear"`) plus the `dcp.spot` / `dcp.future` topic | **off** | orders rest until you cancel them |
| Binance Spot | **none exists** | — | orders rest until you cancel them |
| `nasdaq_itch` | none | — | orders rest until you cancel them |

Deribit's cancel-on-disconnect fires promptly only because the connector runs heartbeats; without them the venue waits out a ten-minute inactivity timeout before it notices a socket that died without a FIN.

Bybit grants DCP only on request: its documentation says the feature "is only available for Ins clients" and an account manager has to enable it. Set `dead_mans_switch_s` once the account has it; the connector then arms the window and subscribes the `dcp.*` topic that DCP needs in order to fire. On an account without it the connector logs `disconnect-cancel-all refused` and keeps quoting.

Binance Spot has no equivalent: no countdown, no session auto-cancel and no cancel-on-disconnect in its REST, WebSocket or FIX APIs. The FIX "countdown" message counts down to a maintenance logout and leaves orders alone. The only venue-side primitive is the manual `openOrders.cancelAll`.

Mitigation, for Binance Spot and for Bybit without DCP: size quotes so that being filled on all of them while disconnected is survivable, and know where the venue's own cancel-all is.

### A dead man's switch that lapses is a kill, not a retry

On Binance USDⓈ-M, if the countdown cannot be refreshed for a whole window while the process is still running, the venue has cancelled every order on that symbol. The connector does not put them back: it sets its fatal flag and trips the venue's kill bit with `DeadMansSwitchLost`, so the quoter stops rather than race a kill switch it cannot see. Restarting is an operator decision ([Kill switch and shutdown](kill-switch-and-shutdown.md)).

## 3. Fills you will not book

Every connector with order entry replays the account's executions before each open-orders snapshot and once a minute while connected, and books what the private stream missed with the venue's price and fee ([Executions the private stream never delivered](../../reference/venues.md#executions-the-private-stream-never-delivered)).

What is left is a replay that could not be completed (the query failed, was rate limited, or returned a full page). Until the connector's retry succeeds, that reconciliation is an estimate: a partial fill it reports is booked at the order's own price with no fee (`EngineStats::estimated_reconciles`), and an order it drops with quantity still working is counted in `EngineStats::unresolved_orders` and logged, because the position may be short by that much.

Mitigation: reconcile against the account after every disconnect, not only after the session ([Check PnL](journals-replay-pnl.md#check-pnl)). Treat a lost private channel in the log (`<venue>: private channel lost` on Bybit and Deribit, `<venue>: user channel -> Disconnected` on Binance) as an accounting event.

## 4. You can only watch it from outside

The observability surface is the memory-mapped status file (`/dev/shm/fastmm-<name>.status`, and `/dev/shm/fastmm-<name>.gw.status` for `fastmm-gateway`) and what reads it: `fastmm-top`, `fastmm-top --json`, and `fastmm-top --metrics <port>`, which serves the snapshot as Prometheus text from its own process ([Monitoring a live session](monitor-with-fastmm-top.md#scrape-it-with-prometheus)). The engine pushes nothing and alerts on nothing: there is no statsd, OTLP or webhook client in the tree.

Everything is a pull at the resolution of the snapshot: the status file is rewritten every 250 ms, and the engine's counters inside it refresh once a second. Anything shorter-lived, such as a burst of rejects or a one-second stall, is visible only in the log. `fastmm-top --json` for an engine carries fewer fields than the snapshot (`format_status_json`, `src/core/status_segment.cpp`): PnL, `started_ns`, `updated_ns`, `dry_run`, the kill counts and the per-reason reject breakdowns are absent, although the text view and the exporter show them.

The control socket and `fastmm-ctl` are the only way to act on a running session ([Operating a running session](operate-a-running-session.md)): pull and resume quoting for the session, a venue or an instrument; change strategy parameters and risk limits; flatten; trip and clear the kill switch; stop; read the status. You cannot change instruments, venues, threads, ring sizes or the strategy, or act on a single order; `ControlCommand::Reload` is not implemented, so anything else needs a restart.

Mitigation: scrape `fastmm-top --metrics`, and take PnL detail from the log's per-second stats line or the shutdown summary. Alert on `fastmm_up`, on `fastmm_status_age_seconds` above a few seconds, on `fastmm_kill_active`, on `fastmm_flatten_state`, on `fastmm_events_total` not rising, and on the process exiting with 5, 6 or 7.

## 5. The journal

One session writes `<journal_dir>/<name>-<session_id>.fmj`, grown in 64 MiB extents. By default there is one file per session with no size cap; `[engine] journal_max_bytes` rolls over to complete parts (`x.fmj`, `x.1.fmj`, ...), and `[engine] journal_retention_days` deletes `.fmj` files older than that from `journal_dir` at start. The only measured rate is 23 MB to 32 MB per hour for a one-symbol Binance Demo session ([Journal files](journals-replay-pnl.md#journal-files)); a full-depth multicast feed writes far more.

Each extent is reserved with `posix_fallocate` before it is mapped, so a full filesystem is an error, not a `SIGBUS`: the session logs `journal write failed (NoSpace): tripping the kill switch and shutting down`, runs the shutdown sequence and exits 5. A full journal ring trips the kill switch with `JournalOverflow` ([Kill switch and shutdown](kill-switch-and-shutdown.md#what-trips-it)).

Mitigation: put `journal_dir` on its own filesystem, set `journal_retention_days` or archive old journals yourself, and alert on free space with room for a full session ([Go-live checklist](go-live-checklist.md#the-host)).

### The journal is durable against a crash, not against power loss

With `[engine] journal_sync = "async"` (the default) the writer copies into an mmap and calls `msync(MS_ASYNC)` every 100 ms (`kSyncInterval`, `src/core/journal_file.cpp`).

- A process crash loses nothing: the page cache outlives the process.
- A power loss or kernel panic loses up to the last 100 ms of events plus anything the kernel had not written back. The file then has no trailer block: `tools/journal_dump.py` reports `trailer MISSING` and `fastmm-replay` refuses it without `--allow-incomplete`.

`journal_sync = "fdatasync"` adds `msync(MS_SYNC)` and `fdatasync()` on the same tick, one write-back per 100 ms, so a record older than one tick survives power loss ([Journal format](../../reference/journal-format.md#durability)).

The [store](../../reference/storage.md) next to the journal has the same failure mode (a batch, not a block, is lost). After an unclean stop, reconcile against the venue rather than either file.

## 6. PnL and accounting

`PositionTracker` books each fill in the instrument's settlement currency: `(px - avg_px) * qty * contract_multiplier` for a linear contract, `qty * contract_multiplier * (1/avg_px - 1/px)` coins for an inverse one (`include/fastmm/core/position.hpp`, [Risk model](../../explanation/risk-model.md#inverse-contracts)).

| Case | What happens | Where it bites |
|---|---|---|
| Two settlement currencies | with `[accounting]`, the totals, `max_loss` and the exposure caps are converted to `reporting_currency` at the mid of each currency's FX source; an order that adds exposure in a currency without a current rate is refused (`FxRateUnknown`). Without it, `fastmm-live` refuses `max_loss` on a mixed table and warns | the rate is the source's mid, not what the venue would convert at; a stale source keeps PnL at its last rate; a currency whose rate was never known is left out of the totals ([Configuration](../../reference/configuration.md#accounting)) |
| Perpetual funding | booked as realized PnL of the instrument, in its settlement currency, once per venue id: Binance USDⓈ-M from `GET /fapi/v1/income` (triggered by the funding `ACCOUNT_UPDATE`, and swept every minute), Bybit linear from `execType` `Funding` executions (stream and `execution/list`). It counts against `max_loss` like a fill; the store's `funding` table and `funding_raw` columns say how much of realized it is | booked up to a minute late when the stream event is missed; Deribit perpetual funding is not booked ([Venues](../../reference/venues.md#funding)) |
| Commission in a third asset | the fee is set to zero and left out of fees and positions, with one WARN line per session | BNB-discounted Binance fees make the engine under-report its costs ([Troubleshooting](troubleshooting.md#orders-and-reconciliation)) |
| Cross-instrument risk | position limits are per instrument; `max_loss` and the exposure caps are the only portfolio-wide limits | a hedged pair and two outright positions look the same to the per-instrument limits |

Mitigation: set `[accounting]` when instruments settle in more than one currency, with a source the session already subscribes. `tools/pnl_report.py` recomputes PnL from the journal's fills and reconciles it against account snapshots; use it, not the engine's number, as the record.

## 7. Rate limits and bans

The connectors limit themselves with fixed-window counters at 90 % of the venue's limit (`include/fastmm/venues/rate_limiter.hpp`), and `[risk] orders_per_sec` / `burst` is a second, engine-side bucket, off by default. An order refused by either is rejected, never queued; cancels are exempt everywhere.

- Binance Spot and USDⓈ-M take their limits from `exchangeInfo` at startup and have no hardcoded fallback. If that response has no usable `rateLimits`, no bucket is active and the client-side throttle is gone.
- A Binance HTTP 418 IP ban is terminal for the process: the connector logs `HTTP 418 IP ban: REST stopped until restart` and trips the venue kill switch with `VenueHardStop`. The kill-switch cancel-all for that venue then fails, so cancel on the website.

Mitigation: set `[risk] orders_per_sec` and `burst` explicitly, and raise `[engine] min_requote_ticks` and `min_requote_interval_ms` to cut the request rate at the source.

## 8. Clocks

Binance and Bybit sign requests with a timestamp and a receive window (`recv_window_ms`, default 3000 ms and 5000 ms). Both connectors measure the venue offset at startup, re-measure every 30 minutes and on a timestamp error, and apply it when signing; an offset above 1000 ms logs `clock offset to venue is <n> ms`. Deribit authenticates without a timestamp and measures `public/get_time` once at startup.

`[risk] stale_md_ms` does not depend on the host clock: a book's age is measured on the engine's own clock from when the engine consumed the update.

Mitigation: run chrony or systemd-timesyncd set to slew rather than step, and watch `clock_offset_ms` in `fastmm-top`.

## 9. Keys

Keys come from the environment through `${VAR}` in `[venues.*]`; a literal secret in the config is refused unless `--allow-inline-secrets` is given. A `${VAR}` that is not set is an error, not an empty string (`include/fastmm/config/env_subst.hpp`), except that `--dry-run` clears a missing `api_key` and `api_secret`. Secrets are printed as `***`, and the configuration embedded in the journal has them redacted.

Nothing checks the key's permissions. A wrong permission surfaces only when the venue refuses a request and the connector trips that venue's kill switch with `VenueFatal`.

`--record-raw <dir>` writes verbatim WebSocket frames to disk. On Binance the order channel's request frames carry `apiKey` and `signature`, so raw-recording the order channel persists key material.

Mitigation: one key per engine, trading permission only, no withdrawal rights, IP-allowlisted where the venue supports it, and `--record-raw` on market data only ([Go-live checklist](go-live-checklist.md#keys-and-access)).

## 10. What the repository does not ship

| Missing | What you have to write |
|---|---|
| A service file for your host | `deploy/fastmm-live.service` sets `Restart=on-failure`, `LimitMEMLOCK=infinity` and `CPUAffinity=2 3`; the paths, the user and the cores are yours to set ([Deploy a release](deploy.md#run-under-systemd)). There is no unit for `fastmm-gateway` |
| An install target for the binaries | `CMakeLists.txt` installs libraries and headers only; `scripts/package-release.sh` makes the release tarball of four binaries |
| Ulimits and cgroup limits for the container | `docker/Dockerfile.production` is non-root with the distro CA bundle and no test certificates, but sets no ulimits and no memory limit ([Deploy a release](deploy.md#run-the-container)); `docker/Dockerfile` and `docker-compose.yml` are the demo, as root |
| A config profile mechanism | `configs/profiles/production-latency.toml` is one file with no loader and no `--profile` flag; copy its `[engine]` table by hand. Everything outside `[engine]` in that file is simulator config, including literal passwords |
| Log rotation | point `[logging] file` at a path your own rotation handles, or let `mirror_level` send warnings to a collector on stderr |
| Any alerting | see [4](#4-you-can-only-watch-it-from-outside) |

The latency profile also needs host settings it cannot make: `isolcpus`, `nohz_full` and `rcu_nocbs` on the engine and network cores, the `performance` governor, NIC interrupts elsewhere, transparent huge pages at `madvise` or `always`, and a raised `ulimit -l`. `scripts/host-setup.sh tune` sets the hugepages, IRQ affinity and governor as root.

## 11. Correctness limits

- A live session is not repeatable; its journal is. A replay needs the same binary and the embedded configuration ([Determinism](../../explanation/determinism.md)).
- The engine consumes L2 books only: `OrderAddL3` and its siblings fall through `Engine::dispatch` unconsumed. The `nasdaq_itch` connector keeps its L3 book on the venue thread and emits L2 deltas, so a strategy never sees order-level data.
- Bybit covers spot and linear perpetuals (`category = "linear"`, one-way position mode only); linear futures and inverse contracts are refused.
- `nasdaq_itch` has market data only in production; order entry exists for `fastmm-sim-itch` (`order_entry = "sim_ouch"`) and nothing else.
- Deribit's open-order reconciliation is skipped whole if any currency's request errors, until the next reconnect.
- The OMS deduplicates executions in a 4096-entry window. A replay longer than that, as a Nasdaq SoupBinTCP re-login from sequence 1 can produce, would double-book.
- Items the connectors could not confirm against a live venue are listed on [Venue connectors](../../reference/venues.md#open-questions-verify-in-the-code).

## Not verified here

- Behaviour against a real main-net venue: no session in this repository has run against one.
- Sustained throughput and the journal write rate on a feed larger than one crypto symbol.
- Whether the 4096-entry execution dedupe window is ever exceeded in practice.

## Related

- [Operations runbook](runbook.md): first deploy, daily checks, what to do when something fires.
- [Errors and exit codes](../../reference/errors.md): every exit code, reject reason and kill reason.
- [Go-live checklist](go-live-checklist.md): the list to run before each session.
- [Kill switch and shutdown](kill-switch-and-shutdown.md): what trips it and what the shutdown does.
