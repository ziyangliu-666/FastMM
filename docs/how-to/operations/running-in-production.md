# Run in production

What to set before a keyed session runs unattended, and what the engine does on a restart, a lost connection or a full disk. The shipped configs point at a testnet, Demo Mode or a local simulator; a main-net config changes the URLs, sets `testnet = false` and supplies the keys.

| Section | Covers |
|---|---|
| [Restarts](#restarts) | what a new process carries over |
| [Venue-side cancel switches](#venue-side-cancel-switches) | who cancels after a hard kill |
| [Missed fills](#missed-fills) | executions the private stream did not deliver |
| [Monitoring](#monitoring) | status file, metrics, control socket |
| [The journal](#the-journal) | disk space and durability |
| [PnL and accounting](#pnl-and-accounting) | currencies, funding, fees |
| [Rate limits](#rate-limits-and-bans), [clocks](#clocks), [keys](#keys) | venue-facing settings |
| [Host setup](#host-setup), [host tuning](#host-tuning) | services, cores, idle states, interrupts, real-time scheduling |

## Restarts

| State | After a restart |
|---|---|
| position | restored per venue and symbol from the store (`[engine] restore_position`, default on), then updated by the venue's executions since the last stored fill |
| orders | none go out until every venue has replayed and reconciled |
| orders the dead process left | swept on first connect; their ids belong to an earlier epoch, so the engine cancels them (`cancelling unknown live order <id>`) |
| kill switch, `max_loss` budget | carried in `<journal_dir>/<name>.kill`; a latched kill exits 6 at every start until cleared ([The latched loss budget](kill-switch-and-shutdown.md#the-latched-loss-budget)) |
| client order ids | stay unique through `[engine] epoch_file` (default `runs/session_epoch`); keep the file |
| session realised PnL | starts at zero; the previous session's is logged and kept in the store |
| a running flatten | abandoned ([Operating a running session](operate-a-running-session.md#a-restart-during-a-flatten)) |

The executions replayed on start include fills of orders resting when the process died and trades made on the account outside FastMM. Two states make a start exit 9: a missing epoch file while the store holds sessions, and a stored position the newest session booked twice into, unless the venue reports its own ([Runbook](runbook.md#a-start-refuses-the-state-it-carries-over)).

The shipped systemd unit restarts after a crash ([Deploy](deploy.md#run-under-systemd)). To replace a running process with a new build or configuration, hand it over instead of stopping it first ([Hand over a running session](hand-over-a-session.md)). The new process loads its reference data while the old one trades, and `[engine] instance_lock` keeps them from trading at once.

## Venue-side cancel switches

A process that is gone cancels nothing. Its orders rest until the next session's start-up sweep, or until the venue's own switch clears them.

`cancel_on_order_channel_loss = true`, on by default, makes the connector cancel over REST when its order channel drops. That needs the process alive. After `kill -9`, an OOM kill, a kernel panic or a power loss, only the venue can clear the orders:

| Venue | Venue-side switch | Default | After a hard kill |
|---|---|---|---|
| Binance USDⓈ-M | `POST /fapi/v1/countdownCancelAll` per symbol, refreshed every third of `dead_mans_switch_ms` | 60 s | orders gone within the window |
| Deribit | `private/enable_cancel_on_disconnect`, scope `connection`, plus `public/set_heartbeat` | on, 10 s heartbeat | orders gone once the heartbeat misses |
| Bybit | `POST /v5/order/disconnected-cancel-all` plus the `dcp.spot` or `dcp.future` topic | off | orders rest until you cancel them |
| Binance Spot | none exists | | orders rest until you cancel them |
| `nasdaq_itch` | none | | orders rest until you cancel them |

On Binance Spot, and on Bybit without DCP, size quotes so that being filled on all of them while disconnected is survivable, and know where the venue's own cancel-all is.

- Deribit fires promptly because the connector sends heartbeats; without them the venue waits out a ten-minute inactivity timeout.
- Bybit enables DCP only on request through an account manager. Once the account has it, set `dead_mans_switch_s`; the connector arms the window and subscribes the `dcp.*` topic. Without it the connector logs `disconnect-cancel-all refused` and keeps quoting.
- Binance Spot has no countdown or cancel-on-disconnect in its REST, WebSocket or FIX APIs. The only venue-side primitive is the manual `openOrders.cancelAll`.

### A lapsed switch is a kill

On Binance USDⓈ-M, if the countdown cannot be refreshed for a whole window while the process runs, the venue has cancelled every order on that symbol. The connector does not put them back. It trips the venue's kill switch with `DeadMansSwitchLost`, so the quoter stops instead of racing a switch it cannot see. Restarting is an operator decision ([Kill switch and shutdown](kill-switch-and-shutdown.md)).

## Missed fills

Every connector with order entry replays the account's executions before each open-orders snapshot and once a minute. It books what the private stream missed at the venue's price and fee ([Executions the private stream never delivered](../../reference/venues/connector-behaviour.md#execution-replay)).

A replay can fail: the query errors, is rate limited or returns a full page. Until the retry succeeds, a partial fill is booked at the order's own price with no fee, and an order dropped with quantity still working is logged as unresolved. The engine counters `estimated_reconciles` and `unresolved_orders` count them.

Treat a lost private channel as an accounting event and reconcile against the account after it ([Check PnL](journals-replay-pnl.md#check-pnl)). The log line is `<venue>: private channel lost` on Bybit and Deribit, and `<venue>: user channel -> Disconnected` on Binance.

## Monitoring

| Tool | What it gives |
|---|---|
| `/dev/shm/fastmm-<name>.status` (`.gw.status` for a gateway) | the process's state, rewritten every 250 ms; engine counters refresh once a second |
| `fastmm-top` | reads the status file; `--json` prints it |
| `fastmm-top --metrics <port>` | Prometheus text; alerting rules in `deploy/prometheus/fastmm-alerts.yml` ([Monitoring a live session](monitor-with-fastmm-top.md#scrape-it-with-prometheus)) |
| `fastmm-ctl` | pull, resume, change parameters and limits, flatten, kill, stop ([Operating a running session](operate-a-running-session.md)) |
| the log | the per-second stats line, bursts shorter than a second, the shutdown summary |

`fastmm-top --json` for an engine leaves out PnL, `started_ns`, `updated_ns`, `dry_run`, the kill counts and the reject breakdowns; the text view and the exporter have them. Instruments, venues, threads, ring sizes and the strategy change only with a restart.

Alert on the process exiting with 5, 6 or 7 from whatever starts it.

## The journal

A session writes `<journal_dir>/<name>-<session_id>.fmj`, grown in 64 MiB extents. One-symbol Binance Demo sessions write 23 to 32 MB per hour; a full-depth multicast feed writes far more ([Journal files](journals-replay-pnl.md#journal-files)).

1. Put `journal_dir` on its own filesystem.
2. Set `[engine] journal_max_bytes` to roll over into parts (`x.fmj`, `x.1.fmj`, ...) and `journal_retention_days` to delete old files at start, or archive them yourself.
3. Alert on free space with room for a full session ([Go-live checklist](go-live-checklist.md#the-host)).

Each extent is reserved before it is written, so a full filesystem is an error, not a crash. The session logs `journal write failed (NoSpace): tripping the kill switch and shutting down` and exits 5. A full journal ring trips the kill switch with `JournalOverflow` ([What trips it](kill-switch-and-shutdown.md#what-trips-it)).

### Durability

| `[engine] journal_sync` | Process crash | Power loss or kernel panic |
|---|---|---|
| `"async"` (default) | loses nothing; the page cache outlives the process | loses up to the last 100 ms plus what the kernel had not written back |
| `"fdatasync"` | loses nothing | loses at most the last 100 ms |

After a power loss the file has no trailer: `tools/journal_dump.py` reports `trailer MISSING` and `fastmm-replay` needs `--allow-incomplete` ([Journal format](../../reference/journal-format.md#durability)). The [store](../../reference/storage.md) behaves the same way, losing a batch. After an unclean stop, reconcile against the venue, not either file.

## PnL and accounting

Each fill is booked in the instrument's settlement currency: `(px - avg_px) * qty * contract_multiplier` for a linear contract, `qty * contract_multiplier * (1/avg_px - 1/px)` coins for an inverse one ([Risk model](../../explanation/risk-model.md#inverse-contracts)).

| Case | What happens | Where it bites |
|---|---|---|
| two settlement currencies | with `[accounting]`, totals, `max_loss` and exposure caps convert to `reporting_currency` at each FX source's mid; an order adding exposure without a current rate is refused (`FxRateUnknown`). Without it, `max_loss` on a mixed table is refused | the rate is a mid, not the venue's conversion; a stale source keeps its last rate ([Configuration](../../reference/configuration.md#accounting)) |
| perpetual funding | booked as realized PnL, once per venue id: Binance USDⓈ-M from `GET /fapi/v1/income`, Bybit linear from `Funding` executions; counts against `max_loss`; the store's `funding` table shows it | up to a minute late when the stream event is missed; Deribit funding is not booked ([Venues](../../reference/venues/deribit.md#funding)) |
| commission in a third asset | the fee is set to zero and left out, with one WARN per session | BNB-discounted Binance fees under-report costs ([Troubleshooting](troubleshooting.md#orders-and-reconciliation)) |
| cross-instrument risk | position limits are per instrument; `max_loss` and the exposure caps are the only portfolio-wide limits | a hedged pair and two outright positions look the same to per-instrument limits |

Use `tools/pnl_report.py`, which recomputes PnL from the journal and reconciles it with account snapshots, as the record, not the engine's number.

## Rate limits and bans

The connectors hold themselves to 90% of the venue's limit; `[risk] orders_per_sec` and `burst` add an engine-side bucket, off by default. An order refused by either is rejected, never queued; cancels are exempt.

- Binance takes its limits from `exchangeInfo` at start-up. If that response has no usable `rateLimits`, no bucket is active.
- A Binance HTTP 418 IP ban stops REST until restart and trips the venue with `VenueHardStop`. Its cancel-all then fails, so cancel on the website.

Set `[risk] orders_per_sec` and `burst` explicitly, and raise `[engine] min_requote_ticks` and `min_requote_interval_ms` to cut the request rate at the source.

## Clocks

Binance and Bybit sign requests with a timestamp and a receive window (`recv_window_ms`, default 3000 and 5000 ms). Both measure the venue's clock offset at start-up, every 30 minutes and after a timestamp error, and apply it when signing. An offset above 1000 ms logs `clock offset to venue is <n> ms`. Deribit needs no timestamp.

`[risk] stale_md_ms` uses the engine's own clock, not the host's. Run chrony or systemd-timesyncd set to slew, not step, and watch `clock_offset_ms` in `fastmm-top`.

## Keys

Keys come from the environment through `${VAR}` in `[venues.*]`. A literal secret is refused unless `--allow-inline-secrets` is given. An unset `${VAR}` is an error, except that `--dry-run` clears a missing `api_key`, `api_secret` and `api_passphrase`. Secrets print as `***` and are redacted from the journal.

The venue checks a key's permissions: a wrong one surfaces when a request is refused, and the venue trips with `VenueFatal`.

`--record-raw <dir>` writes WebSocket frames verbatim. On Binance the order channel's frames carry `apiKey` and `signature`, so record market data only.

Use one key per engine, trading permission only, no withdrawals, IP-allowlisted where the venue supports it ([Go-live checklist](go-live-checklist.md#keys-and-access)).

## Host setup

| Item | What to set |
|---|---|
| service files | `deploy/fastmm-live.service`, `fastmm-live@.service` and `fastmm-gateway.service` restart on failure; the `fastmm-live` units also set `LimitMEMLOCK=infinity` and `CPUAffinity=2 3`. Set the paths, user and cores ([Deploy a release](deploy.md#run-under-systemd)) |
| binaries | `scripts/package-release.sh` makes the release tarball with every `fastmm-*` program |
| container | `deploy/docker/Dockerfile.production` runs non-root with the distro CA bundle; set ulimits and a memory limit ([Deploy a release](deploy.md#run-the-container)). `deploy/docker/Dockerfile` and `compose.yml` are the demo, as root |
| latency profile | `fastmm init --profile production` writes `production.toml`, a Binance Spot config with these settings. `configs/profiles/production-latency.toml` is the same `[engine]` table for the Nasdaq simulator |
| log rotation | point `[logging] file` at a path your rotation handles, or let `mirror_level` send warnings to stderr |

The latency profile needs these host settings:

- `isolcpus`, `nohz_full` and `rcu_nocbs` on the engine and network cores;
- the `performance` governor;
- NIC interrupts on other cores;
- transparent huge pages at `madvise` or `always`;
- a raised `ulimit -l`.

`sudo scripts/host-setup.sh tune` sets the huge pages, IRQ affinity and governor.

Without `nohz_full`, a spinning thread is stopped for 5 to 10 µs by every timer tick, a thousand times a second; with it, by none. Isolation does not stop a TLB shootdown: when the process unmaps memory, every CPU it runs on takes a 1 to 2 µs interrupt. The journal writer avoids mapping files for that reason.

## Host tuning

These `[engine]` keys are off by default. When the process lacks the permission for one, it logs a warning and runs without it.

| Key | Effect | Section |
|---|---|---|
| `cpu`, `net_cpus` | pin the engine and network threads | [Cores](#cores) |
| `cpu_dma_latency_us` | keep CPUs out of slow idle states | [Idle states](#idle-states) |
| `log_irq_affinity` | log where NIC interrupts land | [NIC interrupts](#nic-interrupts) |
| `rt_priority`, `net_rt_priority` | `SCHED_FIFO` for the hot threads | [Real-time scheduling](#real-time-scheduling) |

### Cores

Before its hot threads start, `fastmm-live` moves every other thread of the process off the CPUs in `[engine] cpu` and `net_cpus`; if those are all the CPUs it may use, off `cpu` alone. Threads started later inherit the mask. The log shows `thread affinity: <n> thread(s) of this process moved to CPUs <list>`. `fastmm-gateway` does the same for `net_cpus`.

The start-up `topology:` lines name each hot thread with its CPU, the cores online and where the other threads may run. A warning follows for each problem:

| Warning | Why it costs latency |
|---|---|
| two hot threads share a CPU, or are hyperthreads of one core | they take turns; with `spin_mode = "busy"` each spins through the other's time |
| more hot threads than physical cores | the same, for some of them |
| a pinned CPU is not online | the thread runs wherever the scheduler puts it |
| the journal, store, log and control threads may run on a hot core | they preempt it; the line names the CPUs to start on with `taskset -c` |
| `spin_mode = "adaptive"` with threads alone on their cores | an idle thread sleeps and pays a wake-up on the next event |

Count one hot thread for the engine and one per venue. Five accounts on one venue plus a hedge venue make seven: seven physical cores and one more for everything else, 16 vCPUs on a VM where CPU n and n + 8 share a core.

Venues that `net_cpus` puts on one core, other than the engine's, share one polling network thread when it polls (`spin_mode = "busy"`, or `"adaptive"` with `net_spin_dedicated = true`). It gives each venue its turn and never sleeps, so an order waits at most for the other venues' turns. The topology line shows them as one thread (`net 1 (spot_b) + net 2 (spot_c)`). Without polling, each venue keeps its own thread.

### Idle states

A core in a deep idle state (C-state) takes tens to hundreds of microseconds to wake. A busy-spinning thread keeps its own core awake; an adaptive thread blocked in a wait, and the core taking the NIC's interrupt, do not.

On an 8-vCPU Sapphire Rapids cloud VM with C6 at 190 µs exit latency:

| Wake-up of a thread blocked in `epoll_wait` on another CPU | default | `cpu_dma_latency_us = 10` | `cpu_dma_latency_us = 0` |
|---|---:|---:|---:|
| p50 / p99 | 114 / 166 µs | 8.1 / 8.9 µs | 3.3 / 3.8 µs |

On the same VM a DNS reply reached a busy-polling thread in 118 µs at the median with the NIC's interrupt CPU allowed into C6, and in 64 µs with C6 disabled on that CPU. A busy session pays that on every packet after a quiet spell.

When no limit is held, `fastmm-live` names the slow states at start: `idle states: CPUs 0-7 may enter C6 (190 us exit latency): ...`.

To hold every CPU of the host out of slow states while the session runs:

1. Let the service's group write `/dev/cpu_dma_latency`, then `sudo udevadm trigger --name-match=cpu_dma_latency`:

    ```text
    # /etc/udev/rules.d/99-cpu-dma-latency.rules
    KERNEL=="cpu_dma_latency", GROUP="fastmm", MODE="0660"
    ```

2. Set `[engine] cpu_dma_latency_us = 0` to keep every CPU in C0, or a few microseconds to allow C1.
3. Check the log for `cpu_dma_latency: CPUs held at <us> us exit latency or less (intel_idle)`. Without permission it says `cpu_dma_latency: 0 not held: /dev/cpu_dma_latency: Permission denied`.

The host then draws more power and leaves less turbo headroom to other cores. To limit only some cores, until the next reboot, disable their deep states in sysfs:

```bash
scripts/host-setup.sh cstates --cpus 2-3                     # each state's exit latency and whether it is disabled
sudo scripts/host-setup.sh cstates limit 10 --cpus 2-3       # disable the states slower than 10 us on CPUs 2 and 3
sudo scripts/host-setup.sh cstates restore --cpus 2-3        # enable them again
```

`--dry-run` prints the changes and needs no root. `scripts/host-setup.sh tune --cstate-max-latency 10 --cstate-cpus 2-3` applies the limit after the other tuning. Include the CPUs that take the NIC's interrupts.

### NIC interrupts

A receive interrupt runs the kernel's network processing on the CPU it lands on. Put a venue's queues on or next to its network thread's core and off the engine's:

```bash
sudo systemctl disable --now irqbalance                    # it moves interrupts back otherwise
scripts/host-setup.sh irq-affinity eth1 6 --dry-run        # the queue interrupts and where each would go
sudo scripts/host-setup.sh irq-affinity eth1 6,7           # queue 0 to CPU 6, queue 1 to 7, queue 2 to 6, ...
```

Run `irq-affinity` after `tune`, which first sends every interrupt to CPU 0. A managed interrupt refuses the change and the script says so.

With `[engine] log_irq_affinity = true`, `fastmm-live` logs each queue interrupt and its CPUs at start. One that may run on the engine's CPU is a warning:

```text
irq affinity: eth1 irq 45 eth1-TxRx-0 -> CPUs 6 (net 0)
irq affinity: eth1 irq 46 eth1-TxRx-1 -> CPUs 0-7 (engine net 0): it interrupts the engine's core
```

Most NICs, virtual ones especially, also hold receive interrupts back to batch packets (`ethtool -c eth1` shows `Adaptive RX: on` or a non-zero `rx-usecs`). A packet then waits that long, and a spinning thread does not shorten the wait. Turn it off on the trading interface, until reboot:

```bash
sudo ethtool -C eth1 adaptive-rx off rx-usecs 0
```

On a virtual NIC this cut a packet's delay from the sender's interface to the receiving socket from 17 to 15 µs at p50, 48 to 22 µs at p90 and 60 to 46 µs at p99. `log_irq_affinity` warns while it is on:

```text
irq affinity: eth1 holds receive interrupts back (adaptive-rx on, rx-usecs 20, rx-frames 0): a packet can wait that long before the kernel reads it; ethtool -C eth1 adaptive-rx off rx-usecs 0
```

For busy polling and kernel-bypass stacks on the order path, see [Low-latency TCP](low-latency-tcp.md); for AF_XDP and DPDK market data, [Receive a multicast feed](multicast-feeds.md).

### Real-time scheduling

`[engine] rt_priority = <1..99>` runs the engine thread under `SCHED_FIFO`, and `net_rt_priority` the network threads; with `threading = "single"` `rt_priority` covers both. A stray process or kernel worker on the core then waits instead of taking a time slice. The log says `rt_priority: fm-engine runs SCHED_FIFO at priority <n>`.

The process needs `CAP_SYS_NICE`, or an `RLIMIT_RTPRIO` at least as high. Under systemd, add one to `[Service]`:

```ini
LimitRTPRIO=50                     # an unprivileged user may then use priorities up to 50
AmbientCapabilities=CAP_SYS_NICE   # or: any priority, and other scheduling changes too
```

Without either the log says `rt_priority: SCHED_FIFO 50 for fm-engine refused: Operation not permitted (needs CAP_SYS_NICE or LimitRTPRIO)` and the thread runs on the default scheduler. A kernel built with `CONFIG_RT_GROUP_SCHED` also needs an RT budget (`cpu.rt_runtime_us`) for the unit's cgroup.

A busy-spinning FIFO thread never yields, so it starves the kernel's per-CPU threads on its core. Use it only when:

- the core is isolated (`isolcpus`, `nohz_full`, `rcu_nocbs`) with one pinned thread on it;
- no NIC interrupt lands on it ([NIC interrupts](#nic-interrupts));
- RT throttling stays on: `kernel.sched_rt_runtime_us` (default 950000 per 1000000) leaves 5% of each second to other threads, so the spinning thread can be held off up to 50 ms a second.

`sysctl -w kernel.sched_rt_runtime_us=-1` removes that stall and the safety net with it: a FIFO thread on a core the kernel needs can then hang the host. Set it only on cores proven isolated, and watch `dmesg` for `rcu_sched detected stalls`. With `spin_mode = "adaptive"` the threads sleep when idle and the risk is smaller, but the same rules apply under load.

## Connectors and the OMS

- A live session is not repeatable; its journal is. A replay needs the same binary and the embedded configuration ([Determinism](../../explanation/determinism.md)).
- The engine consumes L2 books. The `nasdaq_itch` connector keeps its L3 book on the venue thread and emits L2 deltas.
- Bybit covers spot and linear perpetuals (`category = "linear"`, one-way mode only); linear futures and inverse contracts are refused.
- `nasdaq_itch` order entry exists for `fastmm-sim-itch` (`order_entry = "sim_ouch"`).
- Deribit skips its whole open-order reconciliation if any currency's request errors, until the next reconnect.
- The OMS deduplicates executions in a 4096-entry window. A longer replay, as a Nasdaq SoupBinTCP re-login from sequence 1 can produce, would double-book.
- Items not yet confirmed against a live venue are listed on [Venue connectors](../../reference/venues.md#capabilities).

Next: the [Operations runbook](runbook.md) for first deploy, daily checks and incidents.
