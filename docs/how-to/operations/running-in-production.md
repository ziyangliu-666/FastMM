# Running this in production

This page lists what an operator will hit, in the order it is likely to cost you, with the file that decides each behaviour and the mitigation where one exists. Read [Economics of the shipped strategies](../../explanation/economics.md) first: the shipped strategies lose money at any positive maker fee.

Every shipped config points at a testnet, Demo Mode or a local simulator. There is no main-net config in the repository; you write it.

## 1. Nothing survives a restart

The engine keeps every piece of trading state in memory (`include/fastmm/core/engine.hpp`). A restart starts from zero:

| State | After a restart | Consequence |
|---|---|---|
| Position | zero on every venue except Binance USDⓈ-M, which resyncs it from `positionRisk` | the engine quotes as if flat while you are not |
| Realised PnL, unrealised PnL, fees | zero | the session summary and `fastmm-top` under-report your real loss |
| `[risk] max_loss` budget | zero consumed | see below |
| Kill switch and its reason | cleared | a process that killed itself for `MaxLoss` will trade again on restart |
| OMS order table | empty | orders left on the venue are invisible; see [2](#2-orders-the-engine-cannot-see) |
| Client order id sequence | continues | `[engine] epoch_file` (default `runs/session_epoch`) keeps ids unique; keep the file |

Mitigation: treat a restart as a new trading decision. Before restarting, read the account's position and open orders on the venue, flatten or accept the inherited position deliberately, and size `[risk] max_position` and `max_loss` for the restarted session.

### The loss budget is per process

`[risk] max_loss` is compared against the cumulative net PnL of the running process (`RiskEngine::on_pnl`, `include/fastmm/core/risk.hpp`). There is no daily counter, no UTC roll-over and no persisted total; grep the tree for `daily` and nothing comes back. A process that trips `MaxLoss`, exits with code 6 and is restarted by a supervisor gets a fresh full budget every time.

Mitigation: do not put `fastmm-live` behind `Restart=always`. Exit codes 5, 6 and 7 mean a human has to look. Use `Restart=no` and alert on those codes ([Errors and exit codes](../../reference/errors.md)).

## 2. Orders the engine cannot see

There is no reconciliation at startup on Binance Spot, Bybit or Deribit. Their reconcile paths are gated on a "was previously live" latch that is false on a first connect (`binance_venue.hpp`, `bybit_venue.hpp`, `deribit_venue.hpp`); only Binance USDⓈ-M queries open orders before it starts. There is no cancel-all-on-start and no config key for one.

Orders a previous run left resting therefore stay on the venue, unknown to the OMS, until the order channel drops and reconnects. At that point they are cancelled with `cancelling unknown live order <id>`. Until then they can fill, and a fill on an unknown order is booked to the position but leaves the OMS without an order to match it to.

Mitigation: cancel all orders on the venue's own interface before every start, and verify it. The [Go-live checklist](go-live-checklist.md#stopping) says the same for stopping.

### Cancel-on-disconnect is armed only on Deribit

`cancel_on_disconnect` exists on exactly one connector (`private/enable_cancel_on_disconnect`, scope `connection`, `src/venues/deribit/deribit_venue.cpp`). Binance Spot, Binance USDⓈ-M, Bybit and `nasdaq_itch` have no venue-side equivalent.

What the others have is `cancel_on_order_channel_loss = true`, a client-side REST cancel-all issued by the connector when its order channel drops. It requires the process to be alive and the network to work. `kill -9`, an OOM kill, a kernel panic or a host that loses power leaves your orders resting on Binance and Bybit with nothing to cancel them.

Mitigation: keep quote sizes at a level you can survive being filled entirely while you are not connected, and know where the venue's own cancel-all is before you need it.

## 3. Fills you will not book

No connector queries trade history. Every reconcile is an open-orders snapshot; there is no `myTrades`, `userTrades`, `execution/list` or `get_user_trades` call anywhere in `src/` or `include/`.

While the private stream is down:

- A partial fill is lost. `Oms::reconcile_open_order` advances `cum_qty` from the snapshot (`include/fastmm/core/oms.hpp`) but never calls `PositionTracker::on_fill`, which is the only place position, PnL and fees are updated (`include/fastmm/core/engine.hpp`). The traded quantity vanishes from the engine's position.
- A complete fill is worse: the order is missing from the snapshot, so `reconcile_end` terminates it as `Canceled`. The engine believes the order was cancelled when it traded.

Binance USDⓈ-M is the only venue that repairs the position afterwards, because it also resyncs `positionRisk` and emits a `Position` reconcile message. Realised PnL and fees stay wrong there too.

Mitigation: reconcile against the account after every disconnect, not only after the session ([Check PnL](journals-replay-pnl.md#check-pnl)). Treat `<venue>: private channel lost` in the log as an accounting event.

## 4. You cannot watch it, and you cannot talk to it

The entire observability surface is the memory-mapped status file (`/dev/shm/fastmm-<name>.status`) and what reads it: `fastmm-top`, `fastmm-top --json`, and `fastmm-top --metrics <port>`, which serves the snapshot as Prometheus text from its own process ([Monitoring a live session](monitor-with-fastmm-top.md#scrape-it-with-prometheus)). The engine itself pushes nothing and alerts on nothing: grep for `statsd`, `otlp`, `webhook`, `pagerduty` or `alertmanager` and there are no hits.

So everything is a pull, at the resolution of the snapshot: the control thread publishes every 250 ms, and the engine's own counters inside it refresh once a second. Anything shorter-lived than that — a burst of rejects, a one-second stall — is visible only in the log. `fastmm-top --json` prints one snapshot as JSON and carries fewer fields than the snapshot does (`format_status_json`, `src/core/status_segment.cpp`): PnL (`realized_pnl_raw`, `unrealized_pnl_raw`, `fees_raw`), `started_ns` and `updated_ns`, `dry_run`, `kills` and `venue_kills`, and the per-reason reject breakdowns are all absent, although the text renderer and the exporter print them.

The operator interface is SIGINT and SIGTERM. `ControlCommand` (pull quotes, resume quotes, reset the kill switch, reload, recalibrate) is pushed only from inside `src/live/session.cpp`; nothing external can send one. You cannot pull quotes, reset a kill switch or change a parameter on a running engine. To change anything, stop the process.

Mitigation: scrape `fastmm-top --metrics`, and take PnL detail from the log's per-second stats line or the shutdown summary. Alert on `fastmm_up`, on `fastmm_status_age_seconds` above a few seconds, on `fastmm_kill_active`, on `fastmm_events_total` not rising, and on the process exiting with 5, 6 or 7.

## 5. The journal

One session writes one file, `<journal_dir>/<name>-<session_id>.fmj`, grown in 64 MiB extents. There is no rotation, no retention policy and no size cap. The only measured rate is 23 MB to 32 MB per hour for a one-symbol Binance Demo session ([Journal files](journals-replay-pnl.md#journal-files)); a full-depth multicast feed is far higher and nothing bounds it.

A full disk is silent. `JournalFileWriter::append` returns without writing when the mapping cannot grow, with no log line and no counter (`src/core/journal_file.cpp`); `ENOSPC` appears nowhere in the tree. On a filesystem that allows sparse extension the failure arrives instead as `SIGBUS` on the memcpy, which kills the process without cancelling anything. A full journal ring, by contrast, trips the kill switch with `JournalOverflow` and shuts the session down cleanly.

Mitigation: put `journal_dir` on its own filesystem, alert on free space with room for a full session, and delete or archive old journals yourself. Check `df -h` before every start, as the [Go-live checklist](go-live-checklist.md#the-host) says.

### The journal is durable against a crash, not against power loss

With `[engine] journal_sync = "async"` (the default) the writer memcpys into an mmap and calls `msync(MS_ASYNC)` every 100 ms (`kSyncInterval`, `src/core/journal_file.cpp`).

- A process crash loses nothing: the page cache outlives the process.
- A power loss or kernel panic loses up to the last 100 ms of events plus anything the kernel had not written back, and the file has no trailer block, so `tools/journal_dump.py` reports `trailer MISSING` and `fastmm-replay` refuses it without `--allow-incomplete`.

`journal_sync = "fdatasync"` adds `msync(MS_SYNC)` and `fdatasync()` on the same tick, which costs one write-back per 100 ms and makes a record older than one tick survive power loss ([Journal format](../../reference/journal-format.md#durability)).

Mitigation: the journal is the byte-exact execution record, and the [store](../../reference/storage.md) next to it holds the interpreted one with the same failure mode (a batch, not a block, is what is lost). Put both on a filesystem and device whose writeback you have measured, and reconcile against the venue rather than either of them after an unclean stop.

## 6. PnL and accounting

`PositionTracker` computes `(px - avg_px) * qty * contract_multiplier` and nothing else (`include/fastmm/core/position.hpp`).

| Case | What happens | Where it bites |
|---|---|---|
| Inverse contracts | the `kInverse` flag is set by the Deribit connector but `PositionTracker` never reads it; PnL is linear | Deribit BTC-PERPETUAL is a USD-denominated inverse contract, so its PnL and its contribution to `max_loss` are wrong. Coin-quoted inverse options are unaffected: linear is correct for them ([Options](../../reference/options.md)) |
| Two quote currencies | `Notional` has no currency tag; `realized_total_`, `unrealized_total_` and `fees_total_` add every instrument's number together | a BTC-settled Deribit PnL and a USDT Binance PnL are summed as bare integers, and `max_loss` is evaluated on that sum |
| Commission in a third asset | the fee is set to zero and dropped from fees and positions, with one WARN line per session | BNB-discounted Binance fees make the engine under-report its costs ([Troubleshooting](troubleshooting.md#orders-and-reconciliation)) |
| Cross-instrument risk | position limits are per instrument; `max_loss` is the only portfolio-wide limit | a hedged pair and two outright positions look the same to the risk layer |

Mitigation: run one engine per quote currency, and do not rely on `max_loss` as a portfolio stop when instruments settle differently. `tools/pnl_report.py` recomputes PnL from the journal's fills and reconciles it against account snapshots; use it, not the engine's number, as the record.

## 7. Rate limits and bans

The connectors limit themselves with fixed-window counters at 90 % of the venue's limit (`include/fastmm/venues/rate_limiter.hpp`), and `[risk] orders_per_sec` / `burst` is a second, engine-side bucket that is off by default. An order refused by either is rejected, never queued; cancels are exempt everywhere.

Two sharp edges:

- Binance Spot and USDⓈ-M take their limits from `exchangeInfo` at startup and have no hardcoded fallback. If that response has no usable `rateLimits`, no bucket is active and the client-side throttle silently disappears.
- A Binance HTTP 418 IP ban is terminal for the process. It sets a hard stop, trips the venue kill switch with `VenueHardStop`, and `RateLimiter::clear_hard_stop()` has no caller anywhere in the tree. The kill-switch cancel-all for that venue then fails, so you cancel on the website.

Mitigation: set `[risk] orders_per_sec` and `burst` explicitly rather than relying on the venue-derived limits, and raise `[engine] min_requote_ticks` and `min_requote_interval_ms` to cut the request rate at the source.

## 8. Clocks

Binance and Bybit sign requests with a timestamp and a receive window (`recv_window_ms`, default 3000 ms and 5000 ms). Both connectors measure the venue offset at startup, re-measure every 30 minutes and on a timestamp error, and apply it when signing. Deribit measures `public/get_time` once at startup and never again; a `ResyncClock` action is a no-op there.

`[risk] stale_md_ms` does not use the correction. `RiskEngine::check_new` subtracts the venue-stamped `exch_ts` from the local engine clock (`include/fastmm/core/risk.hpp`), so a host clock that lags by more than `stale_md_ms` rejects every order as `StaleMarketData` while the offset warning only fires above 1000 ms.

Mitigation: run chrony or systemd-timesyncd and make it slew rather than step, watch `clock_offset_ms` in `fastmm-top`, and keep `stale_md_ms` comfortably above your worst observed offset.

## 9. Keys

Keys come from the environment only. A `${VAR}` that is not set is an error, not an empty string (`include/fastmm/config/env_subst.hpp`), except that `--dry-run` clears a missing `api_key` and `api_secret`. Secrets are wrapped in a type whose formatter prints `***`, and the effective configuration embedded in the journal has them redacted.

Nothing checks the key's permissions. The only statement that a key must not have withdrawal rights is the [Go-live checklist](go-live-checklist.md#keys-and-access); a wrong permission surfaces only when the venue refuses a request and the connector trips that venue's kill switch with `VenueFatal`.

`--record-raw <dir>` writes verbatim WebSocket frames to disk. On Binance the order channel's request frames carry `apiKey` and `signature`, so raw-recording the order channel persists key material.

Mitigation: one key per engine, trading permission only, IP-allowlisted where the venue supports it, and `--record-raw` on market data only.

## 10. What the repository does not ship

| Missing | What you have to write |
|---|---|
| A service file for your host | `deploy/fastmm-live.service` sets `Restart=no`, `LimitMEMLOCK=infinity` and `CPUAffinity=2 3`; the paths, the user and the cores are yours to set ([Deploy a release](deploy.md#run-under-systemd)) |
| An install target for the binaries | `CMakeLists.txt` installs libraries and headers only; `scripts/package-release.sh` makes the release tarball of four binaries |
| Ulimits and cgroup limits for the container | `docker/Dockerfile.production` is non-root with the distro CA bundle and no test certificates, but sets no ulimits and no memory limit ([Deploy a release](deploy.md#run-the-container)); `docker/Dockerfile` and `docker-compose.yml` are the 120-second demo, as root |
| A config profile mechanism | `configs/profiles/production-latency.toml` is one file with no loader and no `--profile` flag; copy its `[engine]` table by hand. Everything outside `[engine]` in that file is simulator config, including literal passwords |
| Log rotation | point `[logging] file` at a path your own rotation handles, or let `mirror_level` send warnings to a collector on stderr |
| Any alerting | see [4](#4-you-cannot-watch-it-and-you-cannot-talk-to-it) |

The host settings the latency profile needs but cannot set are `isolcpus`, `nohz_full` and `rcu_nocbs` on the engine and network cores, the `performance` governor, NIC interrupts elsewhere, transparent huge pages at `madvise` or `always`, and a raised `ulimit -l`. `scripts/host-setup.sh tune` does the hugepages, IRQ affinity and governor part as root.

## 11. Correctness limits

- A live session is not repeatable; its journal is. A replay needs the same binary and the embedded configuration, and a different binary may not match ([Determinism](../../explanation/determinism.md)).
- The engine consumes L2 books only: `OrderAddL3` and its siblings fall through `Engine::dispatch` with `break; // not consumed by the L2 engine`. The `nasdaq_itch` connector keeps its L3 book on the venue thread and emits L2 deltas, so a strategy never sees order-level data.
- Bybit is spot-only. The connector hardcodes `category=spot` in the encoder, the query string and the private parser.
- `nasdaq_itch` has market data only in production; order entry exists for `fastmm-sim-itch` (`order_entry = "sim_ouch"`) and nothing else.
- Deribit's reconciliation is abandoned whole if any currency's request errors, with no retry.
- The OMS deduplicates executions in a 4096-entry ring. A replay longer than that, as a Nasdaq SoupBinTCP re-login from sequence 1 can produce, would double-book.
- Items the connectors could not confirm against a live venue are listed on [Venue connectors](../../reference/venues.md#open-questions-verify-in-the-code).

## Not verified here

- Behaviour under a real main-net venue: no session in this repository has run against one.
- Sustained throughput and the journal write rate on a feed larger than one crypto symbol.
- Whether the 4096-entry execution dedup ring is ever exceeded in practice.
- Whether a `SIGBUS` from a full journal filesystem occurs on a given filesystem; the code path is unhandled either way.

## Related

- [Operations runbook](runbook.md): first deploy, daily checks, what to do when something fires.
- [Errors and exit codes](../../reference/errors.md): every exit code, reject reason and kill reason in one table.
- [Go-live checklist](go-live-checklist.md): the list to run before each session.
- [Kill switch and shutdown](kill-switch-and-shutdown.md): what trips it and what the shutdown does.
