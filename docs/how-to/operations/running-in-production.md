# Run in production

What to set before a keyed session runs unattended, and what the engine does on a restart, a lost connection or a full disk.

The shipped configs point at a testnet, Demo Mode or a local simulator; a main-net config changes the URLs, `testnet = false` and the keys.

## Restarts

| State | After a restart |
|---|---|
| Position | restored per venue and symbol from the store (`[engine] restore_position`, default on), then brought up to date by the venue's executions since the last stored fill, counted in the venue's clock: fills of orders that were resting when the process died, and trades made on the account outside FastMM. No order goes out until every venue has replayed and reconciled. A stored position the newest session booked executions twice into exits 9 unless the venue reports its own ([Runbook](runbook.md#a-start-refuses-the-state-it-carries-over)) |
| Orders the dead process left | every connector sweeps the venue's open orders on its first connect; their ids belong to an earlier session epoch, so the engine cancels them (`cancelling unknown live order <id>`) |
| Kill switch, `[risk] max_loss` budget | carried in `<journal_dir>/<name>.kill`: a latched kill exits 6 at every start until cleared, also when the file is gone and the store recorded the trip; the loss budget is for the deployment, not the process |
| Client order id sequence | continues: `[engine] epoch_file` (default `runs/session_epoch`) keeps ids unique; keep the file. Without it a start exits 9 while the store holds sessions ([Runbook](runbook.md#a-start-refuses-the-state-it-carries-over)) |
| Realised PnL of the session | starts at zero; the previous session's is logged at start and kept in the store |
| A running flatten | abandoned ([Operating a running session](operate-a-running-session.md#a-restart-during-a-flatten)) |

The shipped systemd unit therefore restarts after a crash ([Deploy](deploy.md#run-under-systemd)). To replace a running process with a new build or configuration, hand it over instead of stopping it first: the new process loads its reference data while the old one still trades, and `[engine] instance_lock` keeps the two from trading at once ([Hand over a running session](hand-over-a-session.md)). Against `fastmm-sim-exchange`: `kill -9` while quoting, restart after 2 s, the one order that filled in between booked from the executions, the other cancelled as unknown, and engine and venue at the same position with no open orders.

## Venue-side cancel switches

A process that is gone cancels nothing. Its orders rest until the next session's start-up sweep, or until the venue's own switch clears them.

`cancel_on_order_channel_loss = true` (every connector, default on) is a REST cancel-all the connector issues when its order channel drops. It needs the process alive and the network working, so after `kill -9`, an OOM kill, a kernel panic or a power loss only the venue can clear the orders:

| venue | venue-side switch | default | after a hard kill |
|---|---|---|---|
| Binance USDⓈ-M | `POST /fapi/v1/countdownCancelAll` per symbol, refreshed every third of `dead_mans_switch_ms` | 60 s | orders gone within the window |
| Deribit | `private/enable_cancel_on_disconnect`, scope `connection`, plus `public/set_heartbeat` | on, 10 s heartbeat | orders gone once the heartbeat misses |
| Bybit | `POST /v5/order/disconnected-cancel-all` (product `SPOT`, or `DERIVATIVES` for `category = "linear"`) plus the `dcp.spot` / `dcp.future` topic | off | orders rest until you cancel them |
| Binance Spot | none exists | — | orders rest until you cancel them |
| `nasdaq_itch` | none | — | orders rest until you cancel them |

Deribit's cancel-on-disconnect fires promptly because the connector runs heartbeats; without them the venue waits out a ten-minute inactivity timeout before it notices a socket that died without a FIN.

Bybit grants DCP only on request: its documentation says the feature "is only available for Ins clients" and an account manager has to enable it. Set `dead_mans_switch_s` once the account has it; the connector then arms the window and subscribes the `dcp.*` topic that DCP needs in order to fire. On an account without it the connector logs `disconnect-cancel-all refused` and keeps quoting.

Binance Spot has no countdown, no session auto-cancel and no cancel-on-disconnect in its REST, WebSocket or FIX APIs. The FIX "countdown" message counts down to a maintenance logout and leaves orders alone. The only venue-side primitive is the manual `openOrders.cancelAll`.

On Binance Spot, and on Bybit without DCP, size quotes so that being filled on all of them while disconnected is survivable, and know where the venue's own cancel-all is.

### A lapsed switch is a kill

On Binance USDⓈ-M, if the countdown cannot be refreshed for a whole window while the process is still running, the venue has cancelled every order on that symbol. The connector does not put them back: it sets its fatal flag and trips the venue's kill bit with `DeadMansSwitchLost`, so the quoter stops rather than race a kill switch it cannot see. Restarting is an operator decision ([Kill switch and shutdown](kill-switch-and-shutdown.md)).

## Missed fills

Every connector with order entry replays the account's executions before each open-orders snapshot and once a minute while connected, and books what the private stream missed with the venue's price and fee ([Executions the private stream never delivered](../../reference/venues.md#executions-the-private-stream-never-delivered)).

A replay can fail: the query errors, is rate limited, or returns a full page. Until the connector's retry succeeds, that reconciliation is an estimate: a partial fill it reports is booked at the order's own price with no fee (`EngineStats::estimated_reconciles`), and an order it drops with quantity still working is counted in `EngineStats::unresolved_orders` and logged, because the position may be short by that much.

Reconcile against the account after every disconnect, not only after the session ([Check PnL](journals-replay-pnl.md#check-pnl)). Treat a lost private channel in the log (`<venue>: private channel lost` on Bybit and Deribit, `<venue>: user channel -> Disconnected` on Binance) as an accounting event.

## Monitoring

Each process publishes a memory-mapped status file (`/dev/shm/fastmm-<name>.status`, and `/dev/shm/fastmm-<name>.gw.status` for `fastmm-gateway`). `fastmm-top` reads it, `fastmm-top --json` prints it, and `fastmm-top --metrics <port>` serves it as Prometheus text from its own process. `deploy/prometheus/fastmm-alerts.yml` holds the alerting rules for those metrics ([Monitoring a live session](monitor-with-fastmm-top.md#scrape-it-with-prometheus)).

The status file is rewritten every 250 ms, and the engine's counters inside it refresh once a second. A burst of rejects or a one-second stall shorter than that shows in the log. `fastmm-top --json` for an engine carries fewer fields than the snapshot (`format_status_json`, `src/core/status_segment.cpp`): PnL, `started_ns`, `updated_ns`, `dry_run`, the kill counts and the per-reason reject breakdowns are in the text view and the exporter only.

The control socket and `fastmm-ctl` act on a running session ([Operating a running session](operate-a-running-session.md)): pull and resume quoting for the session, a venue or an instrument; change strategy parameters and risk limits; flatten; trip and clear the kill switch; stop; read the status. Instruments, venues, threads, ring sizes and the strategy change with a restart; `ControlCommand::Reload` is not implemented.

Scrape `fastmm-top --metrics`, load the shipped rules into Prometheus, and alert on the process exiting with 5, 6 or 7 from whatever starts it. PnL detail is in the log's per-second stats line and the shutdown summary.

## The journal

One session writes `<journal_dir>/<name>-<session_id>.fmj`, grown in 64 MiB extents. By default there is one file per session with no size cap; `[engine] journal_max_bytes` rolls over to complete parts (`x.fmj`, `x.1.fmj`, ...), and `[engine] journal_retention_days` deletes `.fmj` files older than that from `journal_dir` at start. A one-symbol Binance Demo session writes 23 MB to 32 MB per hour ([Journal files](journals-replay-pnl.md#journal-files)); a full-depth multicast feed writes far more.

Each extent is reserved with `posix_fallocate` before it is mapped, so a full filesystem is an error, not a `SIGBUS`: the session logs `journal write failed (NoSpace): tripping the kill switch and shutting down`, runs the shutdown sequence and exits 5. A full journal ring trips the kill switch with `JournalOverflow` ([Kill switch and shutdown](kill-switch-and-shutdown.md#what-trips-it)).

Put `journal_dir` on its own filesystem, set `journal_retention_days` or archive old journals yourself, and alert on free space with room for a full session ([Go-live checklist](go-live-checklist.md#the-host)).

### Durability

With `[engine] journal_sync = "async"` (the default) the writer writes each block into the page cache as it fills, and at the latest every 100 ms (`kSyncInterval`, `src/core/journal_file.cpp`); the kernel writes it back.

- A process crash loses nothing: the page cache outlives the process.
- A power loss or kernel panic loses up to the last 100 ms of events plus anything the kernel had not written back. The file then has no trailer block: `tools/journal_dump.py` reports `trailer MISSING` and `fastmm-replay` refuses it without `--allow-incomplete`.

`journal_sync = "fdatasync"` calls `fdatasync()` every 100 ms, one write-back per tick, so a record older than one tick survives power loss ([Journal format](../../reference/journal-format.md#durability)).

The [store](../../reference/storage.md) next to the journal behaves the same way (a batch, not a block, is lost). After an unclean stop, reconcile against the venue rather than either file.

## PnL and accounting

`PositionTracker` books each fill in the instrument's settlement currency: `(px - avg_px) * qty * contract_multiplier` for a linear contract, `qty * contract_multiplier * (1/avg_px - 1/px)` coins for an inverse one (`include/fastmm/core/position.hpp`, [Risk model](../../explanation/risk-model.md#inverse-contracts)).

| Case | What happens | Where it bites |
|---|---|---|
| Two settlement currencies | with `[accounting]`, the totals, `max_loss` and the exposure caps are converted to `reporting_currency` at the mid of each currency's FX source; an order that adds exposure in a currency without a current rate is refused (`FxRateUnknown`). Without it, `fastmm-live` refuses `max_loss` on a mixed table and warns | the rate is the source's mid, not what the venue would convert at; a stale source keeps PnL at its last rate; a currency whose rate was never known is left out of the totals ([Configuration](../../reference/configuration.md#accounting)) |
| Perpetual funding | booked as realized PnL of the instrument, in its settlement currency, once per venue id: Binance USDⓈ-M from `GET /fapi/v1/income` (triggered by the funding `ACCOUNT_UPDATE`, and swept every minute), Bybit linear from `execType` `Funding` executions (stream and `execution/list`). It counts against `max_loss` like a fill; the store's `funding` table and `funding_raw` columns say how much of realized it is | booked up to a minute late when the stream event is missed; Deribit perpetual funding is not booked ([Venues](../../reference/venues.md#funding)) |
| Commission in a third asset | the fee is set to zero and left out of fees and positions, with one WARN line per session | BNB-discounted Binance fees make the engine under-report its costs ([Troubleshooting](troubleshooting.md#orders-and-reconciliation)) |
| Cross-instrument risk | position limits are per instrument; `max_loss` and the exposure caps are the only portfolio-wide limits | a hedged pair and two outright positions look the same to the per-instrument limits |

Set `[accounting]` when instruments settle in more than one currency, with a source the session already subscribes. `tools/pnl_report.py` recomputes PnL from the journal's fills and reconciles it against account snapshots; use it, not the engine's number, as the record.

## Rate limits and bans

The connectors limit themselves with fixed-window counters at 90 % of the venue's limit (`include/fastmm/venues/rate_limiter.hpp`), and `[risk] orders_per_sec` / `burst` is a second, engine-side bucket, off by default. An order refused by either is rejected, never queued; cancels are exempt everywhere.

- Binance Spot and USDⓈ-M take their limits from `exchangeInfo` at startup and have no hardcoded fallback. If that response has no usable `rateLimits`, no bucket is active.
- A Binance HTTP 418 IP ban is terminal for the process: the connector logs `HTTP 418 IP ban: REST stopped until restart` and trips the venue kill switch with `VenueHardStop`. The kill-switch cancel-all for that venue then fails, so cancel on the website.

Set `[risk] orders_per_sec` and `burst` explicitly, and raise `[engine] min_requote_ticks` and `min_requote_interval_ms` to cut the request rate at the source.

## Clocks

Binance and Bybit sign requests with a timestamp and a receive window (`recv_window_ms`, default 3000 ms and 5000 ms). Both connectors measure the venue offset at startup, re-measure every 30 minutes and on a timestamp error, and apply it when signing; an offset above 1000 ms logs `clock offset to venue is <n> ms`. Deribit authenticates without a timestamp and measures `public/get_time` once at startup.

`[risk] stale_md_ms` does not depend on the host clock: a book's age is measured on the engine's own clock from when the engine consumed the update.

Run chrony or systemd-timesyncd set to slew rather than step, and watch `clock_offset_ms` in `fastmm-top`.

## Keys

Keys come from the environment through `${VAR}` in `[venues.*]`; a literal secret in the config is refused unless `--allow-inline-secrets` is given. A `${VAR}` that is not set is an error, not an empty string (`include/fastmm/config/env_subst.hpp`), except that `--dry-run` clears a missing `api_key`, `api_secret` and `api_passphrase`. Secrets are printed as `***`, and the configuration embedded in the journal has them redacted.

A key's permissions are checked by the venue: a wrong permission surfaces when the venue refuses a request and the connector trips that venue's kill switch with `VenueFatal`.

`--record-raw <dir>` writes verbatim WebSocket frames to disk. On Binance the order channel's request frames carry `apiKey` and `signature`, so raw-recording the order channel persists key material.

Use one key per engine, trading permission only, no withdrawal rights, IP-allowlisted where the venue supports it, and `--record-raw` on market data only ([Go-live checklist](go-live-checklist.md#keys-and-access)).

## Host setup

| Item | What to set |
|---|---|
| Service files | `deploy/fastmm-live.service`, `deploy/fastmm-live@.service` and `deploy/fastmm-gateway.service` set `Restart=on-failure`; the two `fastmm-live` units also set `LimitMEMLOCK=infinity` and `CPUAffinity=2 3`. The paths, the user and the cores are yours to set ([Deploy a release](deploy.md#run-under-systemd)) |
| Binaries | `CMakeLists.txt` installs libraries and headers; `scripts/package-release.sh` makes the release tarball with `fastmm-live`, `fastmm-gateway`, `fastmm-ctl`, `fastmm-top`, `fastmm-pnl`, `fastmm-replay`, `fastmm-sim-exchange` and `fastmm-sim-itch` |
| Container limits | `deploy/docker/Dockerfile.production` is non-root with the distro CA bundle and no test certificates; set ulimits and a memory limit in your runtime ([Deploy a release](deploy.md#run-the-container)). `deploy/docker/Dockerfile` and `deploy/docker/compose.yml` are the demo, as root |
| Latency profile | `fastmm init --profile production` writes `production.toml`, a Binance Spot config with these settings and comments on choosing the cores. `configs/profiles/production-latency.toml` is the same `[engine]` table for the Nasdaq simulator; everything outside `[engine]` in that file is simulator config, including literal passwords |
| Log rotation | point `[logging] file` at a path your own rotation handles, or let `mirror_level` send warnings to a collector on stderr |

The latency profile also needs host settings: `isolcpus`, `nohz_full` and `rcu_nocbs` on the engine and network cores, the `performance` governor, NIC interrupts elsewhere, transparent huge pages at `madvise` or `always`, and a raised `ulimit -l`. `scripts/host-setup.sh tune` sets the hugepages, IRQ affinity and governor as root.

On the VM of [Idle states](#idle-states) a thread spinning on a core without `nohz_full` was stopped for 5 to 10 µs by every timer tick, a thousand times a second; with `nohz_full` on its core, by none. Isolation does not stop a TLB shootdown: when any thread of the process unmaps or write-protects memory, every CPU the process runs on takes an interrupt, 1 to 2 µs each. The journal writer uses `pwrite` for that reason, not a mapping of the file.

## Host tuning

The `[engine]` keys below are off by default. One the process lacks the permission for logs a warning and the session runs without it, as `lock_memory` does.

### Cores

Before its hot threads start, `fastmm-live` moves every other thread of the process (journal, store, audit, log, control, DNS resolution) off the CPUs in `[engine] cpu` and `net_cpus`; when those are all the CPUs the process may use, off `cpu` alone. Threads started later inherit the mask. The log shows `thread affinity: <n> thread(s) of this process moved to CPUs <list>`. `fastmm-gateway` does the same for `net_cpus`.

`fastmm-live` logs a topology check before its threads start (`topology:` lines, `core/host_tuning.hpp`). The first line names each hot thread (the engine, then one network thread per `[venues.*]` section) with its CPU, the physical cores and logical CPUs online, and the CPUs the process's other threads may use. A warning follows for each of these:

| Warning | Why it costs latency |
|---|---|
| two hot threads share a CPU, or are hyperthreads of one core | they take turns on one core; with `spin_mode = "busy"` each spins through the other's time |
| more hot threads than physical cores | the same, for some of them |
| a pinned CPU is not online | the thread runs wherever the scheduler puts it |
| the journal, store, log and control threads may run on a hot thread's core | they preempt it or share its core; the line names the CPUs to start the process on with `taskset -c` |
| `spin_mode = "adaptive"` with threads alone on their cores | an idle thread sleeps and pays a wake-up on the next event |

A session with five accounts behind one venue and a hedge venue has seven hot threads: seven physical cores and one more for everything else, 16 vCPUs on a VM where CPU n and n + 8 are one core. `fastmm init --profile production` writes a config laid out this way.

Venues that `net_cpus` puts on one core share a network thread when that thread polls (`spin_mode = "busy"`, or `"adaptive"` with `net_spin_dedicated = true`) and the core is not the engine's. The thread gives each venue its turn of the loop in order and never sleeps, so an order for any of them waits at most for the other venues' turns, where threads that take turns on the core sleep and pay a wake-up on each order. The topology line names them as one hot thread (`net 1 (spot_b) + net 2 (spot_c)`). Without polling, each venue keeps a thread of its own, which blocks when idle on a shared core.

### Idle states

A core in a deep idle state (C-state) takes tens to hundreds of microseconds to wake. A busy-spinning thread keeps its own core awake; an adaptive thread blocked in a wait, and the core that takes the NIC's interrupt, do not.

Cloud VMs have these states too. An 8-vCPU Sapphire Rapids VM (`intel_idle`, C6 at 190 µs exit latency) measured:

| | default | `cpu_dma_latency_us = 10` | `cpu_dma_latency_us = 0` |
|---|---:|---:|---:|
| A thread blocked in `epoll_wait` on another CPU, woken by an `eventfd` write, p50 / p99 | 114 / 166 µs | 8.1 / 8.9 µs | 3.3 / 3.8 µs |

With the NIC's interrupts on a CPU allowed into C6, a UDP request to the network's DNS resolver was answered to a busy-polling thread in 118 µs at the median; with C6 disabled on that one CPU, in 64 µs. A busy session pays the second cost on every packet that arrives after a quiet spell.

When no request is held, `fastmm-live` names the slow states at start: `idle states: CPUs 0-7 may enter C6 (190 us exit latency): ...`.

`[engine] cpu_dma_latency_us = <us>` opens `/dev/cpu_dma_latency`, writes the value and keeps the descriptor open until the session ends: while it is open, no CPU enters an idle state whose exit latency exceeds `<us>`. `0` keeps every CPU polling in C0; a few microseconds still allows C1. The request applies to every CPU of the host, which then draws more power and leaves less turbo headroom to the others. The log says `cpu_dma_latency: CPUs held at <us> us exit latency or less (intel_idle)`, naming the cpuidle driver; on a host with none the request changes nothing and the log says so.

The device is root's, mode 0600. Let the service's group write it with a udev rule, then `sudo udevadm trigger --name-match=cpu_dma_latency`:

```text
# /etc/udev/rules.d/99-cpu-dma-latency.rules
KERNEL=="cpu_dma_latency", GROUP="fastmm", MODE="0660"
```

Without it the log says `cpu_dma_latency: 0 not held: /dev/cpu_dma_latency: Permission denied`.

To limit only some cores, and for longer than one process, disable their deep states in sysfs until the next reboot:

```bash
scripts/host-setup.sh cstates --cpus 2-3                     # each state's exit latency and whether it is disabled
sudo scripts/host-setup.sh cstates limit 10 --cpus 2-3       # disable the states slower than 10 us on CPUs 2 and 3
sudo scripts/host-setup.sh cstates restore --cpus 2-3        # enable them again
```

`--dry-run` prints the changes without making them, and needs no root. `scripts/host-setup.sh tune --cstate-max-latency 10 --cstate-cpus 2-3` runs the same limit after the other tuning. Include the CPUs that take the NIC's interrupts.

### NIC interrupts

A receive interrupt runs the kernel's network processing on the CPU it lands on. Put a venue's queues on or next to its network thread's core and off the engine's core:

```bash
sudo systemctl disable --now irqbalance                    # it moves interrupts back otherwise
scripts/host-setup.sh irq-affinity eth1 6 --dry-run        # the queue interrupts and where each would go
sudo scripts/host-setup.sh irq-affinity eth1 6,7           # queue 0 to CPU 6, queue 1 to 7, queue 2 to 6, ...
```

The queue interrupts are the `/proc/interrupts` lines named after the interface (`eth1-TxRx-0`), or else the device's MSI vectors without its configuration vector (virtio, mlx5). `tune` first sends every interrupt to CPU 0, so run `irq-affinity` after it. A managed interrupt refuses the change and the script says so.

`[engine] log_irq_affinity = true` makes `fastmm-live` log, at start, each interface's queue interrupts with their CPUs and which of them are `[engine] cpu` or `net_cpus`. An interrupt that may run on the engine's CPU is a warning:

```text
irq affinity: eth1 irq 45 eth1-TxRx-0 -> CPUs 6 (net 0)
irq affinity: eth1 irq 46 eth1-TxRx-1 -> CPUs 0-7 (engine net 0): it interrupts the engine's core
```

It only reads `/proc` and `/sys`; what it cannot read is left out of the log.

Most NICs, virtual ones especially, also hold a receive interrupt back to batch packets: `ethtool -c eth1` shows `Adaptive RX: on` or a non-zero `rx-usecs`. A market-data packet then waits up to that long before the kernel hands it to the socket, and a network thread that spins does not shorten the wait. Turn it off on the trading interface (until reboot; a udev rule or the network configuration keeps it):

```bash
sudo ethtool -C eth1 adaptive-rx off rx-usecs 0
```

On a virtual NIC with adaptive moderation, a packet's delay from the sender's interface to the receiver's socket went from 17 µs to 15 µs at the median, from 48 µs to 22 µs at the 90th percentile and from 60 µs to 46 µs at the 99th. With `log_irq_affinity` an interface that holds receive interrupts back is a warning (`ioctl(SIOCETHTOOL)`, no privilege needed):

```text
irq affinity: eth1 holds receive interrupts back (adaptive-rx on, rx-usecs 20, rx-frames 0): a packet can wait that long before the kernel reads it; ethtool -C eth1 adaptive-rx off rx-usecs 0
```

### Real-time scheduling

`[engine] rt_priority = <1..99>` runs the engine thread under `SCHED_FIFO` at that priority, and `net_rt_priority` the network threads (with `threading = "single"` the engine thread runs the network loop and `rt_priority` covers it). A FIFO thread is not preempted by ordinary threads on its core: a stray process or kernel worker scheduled there waits instead of taking the core for a time slice. The log says `rt_priority: fm-engine runs SCHED_FIFO at priority <n>`.

The process needs `CAP_SYS_NICE` or an `RLIMIT_RTPRIO` (`ulimit -r`) at least as high as the priority. Under systemd, add one of these to the unit's `[Service]` section:

```ini
LimitRTPRIO=50                     # an unprivileged user may then use priorities up to 50
AmbientCapabilities=CAP_SYS_NICE   # or: any priority, and other scheduling changes too
```

Without either the session logs `rt_priority: SCHED_FIFO 50 for fm-engine refused: Operation not permitted (needs CAP_SYS_NICE or LimitRTPRIO)` and runs on the default scheduler. On a kernel built with `CONFIG_RT_GROUP_SCHED`, the unit's cgroup also needs an RT runtime budget (`cpu.rt_runtime_us`), or the call fails the same way.

A busy-spinning thread never yields. Under `SCHED_FIFO` it starves everything of lower priority on its core, including the kernel's per-CPU threads (`ksoftirqd`, `kworker`, RCU callbacks, the TCP stack's softirq work for that core). Use it only when:

- the core is isolated: `isolcpus`, `nohz_full` and `rcu_nocbs` on the kernel command line, and nothing else pinned there (`CPUAffinity`, `[engine] cpu`, `net_cpus`, one thread per core);
- no NIC interrupt lands on the core ([NIC interrupts](#nic-interrupts));
- RT throttling stays on. `kernel.sched_rt_runtime_us` (default 950000 of every `sched_rt_period_us` = 1000000) leaves 5 % of each second to other threads, which is what lets a starved kernel thread run at all; the spinning thread can then be held off for up to 50 ms of every second. `sysctl -w kernel.sched_rt_runtime_us=-1` removes the stall and the safety net with it; with it, a FIFO thread on a core the kernel needs can hang the host (RCU stall warnings, a hung network stack). Set -1 only on cores proven isolated, and watch `dmesg` for `rcu_sched detected stalls`.

With `spin_mode = "adaptive"` the threads sleep when idle and the starvation risk is smaller, but the same rules apply under load.

## Connectors and the OMS

- A live session is not repeatable; its journal is. A replay needs the same binary and the embedded configuration ([Determinism](../../explanation/determinism.md)).
- The engine consumes L2 books: `OrderAddL3` and its siblings fall through `Engine::dispatch` unconsumed. The `nasdaq_itch` connector keeps its L3 book on the venue thread and emits L2 deltas.
- Bybit covers spot and linear perpetuals (`category = "linear"`, one-way position mode only); linear futures and inverse contracts are refused.
- `nasdaq_itch` order entry exists for `fastmm-sim-itch` (`order_entry = "sim_ouch"`).
- Deribit's open-order reconciliation is skipped whole if any currency's request errors, until the next reconnect.
- The OMS deduplicates executions in a 4096-entry window. A replay longer than that, as a Nasdaq SoupBinTCP re-login from sequence 1 can produce, would double-book.
- Items the connectors could not confirm against a live venue are listed on [Venue connectors](../../reference/venues.md#open-questions-verify-in-the-code).

Next: [Operations runbook](runbook.md) for first deploy, daily checks and incidents.
