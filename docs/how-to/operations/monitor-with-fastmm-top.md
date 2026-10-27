# Monitor a live session

You need a running `fastmm-live` or `fastmm run` session, and `fastmm-top` from a C++ build or the [release tarball](deploy.md); the pip package does not install it.

The session writes its counters, PnL, kill state, latencies and venue states to a memory-mapped status file without touching the engine thread. The engine's values in it refresh every second. `fastmm-live --status <path>` moves the file and `--no-status` turns it off.

## Open the dashboard

```bash
./build/release/bin/fastmm-live --config configs/binance-demo.toml &
./build/release/bin/fastmm-top --name binance-demo          # or --path <status file>
```

| Option | Effect |
|---|---|
| `--interval <ms>` | redraw period, 500 ms by default |
| `--no-color` | plain text |
| `--once` | print one frame and exit |
| `--json` | print one snapshot as JSON |
| `--metrics <[host:]port>` | serve Prometheus metrics instead of drawing ([below](#scrape-it-with-prometheus)) |
| `--gateway <name>` | read a `fastmm-gateway`'s file, `/dev/shm/fastmm-<name>.gw.status` ([Run behind a gateway](run-behind-a-gateway.md#monitor)) |

For a dry run, use `--name <name>-dryrun`; its orders show as `paper orders=`, what the engine would have sent.

The file stays after the session ends, with a final `stopped` frame. Its layout is versioned, and a `fastmm-top` from another build refuses it; with `--once` that exits 3, as a missing file does:

```text
<file> was written by a different FastMM build (status segment version <n>, this fastmm-top reads version <m>); use fastmm-top from the same build as fastmm-live
```

## Read the dashboard

### Session state

| Shows | Means |
|---|---|
| `starting`, `running`, `stopping`, `stopped` | the session state, next to engine and strategy name, session id, pid, dry-run flag and uptime |
| `STALE` | a running engine has not published for over 3 s; the process has most likely died |
| `KILLED (<reason>)` | the global kill switch is engaged; the reason stays in the final frame |
| `VENUE KILLED` | at least one venue's kill switch is engaged |
| `LATCHED` | a `max_loss` trip is recorded in `[engine] kill_file`, so the next start refuses to trade ([Kill switch and shutdown](kill-switch-and-shutdown.md#the-latched-loss-budget)) |
| `FLATTENING (<n> left)` | an [operator flatten](operate-a-running-session.md#flatten) is running |

### Lines

| Line | Shows |
|---|---|
| engine counters | events, book updates, orders, cancels, replaces, fills, `kills` (global) and `venue_kills` |
| PnL | this session's `realized` (funding included), `unrealized` and `fees`; `carried` from earlier sessions; `budget_used`, the sum `[risk] max_loss` is compared against. In the quote currency or `[accounting] reporting_currency` |
| rejects | `risk_rejects` (refused by pre-trade checks, never sent) and `venue_rejects`, each with up to six top reasons and `other <n>` |
| `balance_withheld` | quote orders held back after a requote because the balance no longer covered them, usually because the old order filled; that side stays unquoted until the strategy quotes again |
| `estimates` | after a private-stream outage ([below](#estimated-fills)) |
| `store` | once the store failed a write or dropped a record: `rows`, `errors`, `dropped`. The next start restores from it ([Storage](../../reference/storage.md#what-a-backend-guarantees)) |
| `treasury` | with a [pool treasury](../../reference/configuration.md#venuestreasury), summed over pools: `in_flight`, `sent`, `done`, `failed`, `timed_out`, `limited`, `dry_run_plans`, `errors` |
| latency | per interval: count, p50, p99, p99.9 and max ([below](#latency-intervals)) |
| per venue | channel states, synced books, messages, resyncs, orders, cancels, reconnects, REST errors, clock offset, wire tick-to-trade p50, and the `kill` reason (`-` while trading) |
| `quotes` | per instrument and side ([below](#quotes)) |
| `metrics` | the values the strategy publishes with `ctx.metric` |
| `process` | the host's view of the session ([below](#process)) |
| feed | per `nasdaq_itch` venue: state, receive backend, packets per line, A/B skew, gaps, recoveries, snapshots, reorder high-water mark, kernel-to-T0 p50 and p99 ([Receive a multicast feed](multicast-feeds.md#7-check-the-feed)) |

A channel state is `down`, `connecting`, `live` or `stale`; a connected but quiet user or order channel shows `live`. A rejects line reads:

```text
risk_rejects=17 (MaxPosition 12, RateLimit 5) venue_rejects=3 (PostOnlyWouldCross 3)
```

### Latency intervals

| Interval | Measures | Thread |
|---|---|---|
| `decode` | parsing a message | network |
| `book_apply` | end of decoding to the book update, including the ring hand-off and, with `spin_mode = "adaptive"`, an idle engine's wake-up | network to engine |
| `strategy`, `serialize`, `send` | each step | engine |
| `tick_to_trade`, `wire_to_book` | from the socket read | both |

### Quotes

The `quotes` table has a row per instrument and side that asks for something, works, or carries a note. It shows what the strategy asked for (level-0 price x quantity over every level), what works (best price x leaves, in how many orders), the first obstacle and how long it has held, and the strategy's note and budget. Stuck sides are yellow.

The obstacles, in the order the engine checks them:

| Obstacle | Means |
|---|---|
| `not_wanted` | the strategy asks for nothing |
| `quoting_off` | quoting is disabled: pulled, killed, reconciling or parameters stale |
| `venue_killed` | the venue's kill switch is engaged |
| `pulled` | `fastmm-ctl pull` |
| `feed_lag` | the feed-lag gate |
| `quoting` | the working orders cover what was asked |
| `backoff` | a venue refusal put the side in reject backoff |
| `starved` | waiting for a `[risk] orders_per_sec` token |
| `refused:<reason>` | the last order on the side was refused in the last 2 s |
| `pending` | orders on their way, or a requote waiting its turn |

Why a model asks for less, or nothing, only the strategy knows; `ctx.note_quote` puts it in the note column ([Strategy API](../../reference/strategy-api.md#context)).

### Estimated fills

After a private-stream outage the `estimates` line counts fills booked without a fill message ([Executions the private stream never delivered](../../reference/venues/connector-behaviour.md#execution-replay)):

| Counter | Means |
|---|---|
| `fills` | booked from a cumulative quantity (a cancel ack, an expiry, a reconciliation snapshot), at the order's price with no fee |
| `pending` | estimates no execution has named yet; the next execution replay corrects price and fee |
| `forgotten` | estimates whose order was forgotten (4096 ended orders later) before an execution named it |

A replayed execution of a forgotten order is booked again on top of its estimate. So `forgotten` above 0 on a venue that replays executions means the position may be over by that order's quantity, until the venue reports its own.

### Process

`fastmm-top` reads `/proc/<pid>` itself; the session publishes nothing for it. It shows resident memory, host load, and per thread (`fm-engine`, `fm-store`, the venue threads) its state, last CPU, allowed CPUs, CPU use, context switches per second and major page faults. With `--once` the counters are totals; `--json` has them under `"process"` as cumulative counters.

An engine thread with involuntary switches is being preempted by another task on its core, so a latency spike at that time is the host's. The section works only in the session's host and pid namespace; a reused pid is refused with `"ok": false` and the reason.

## Scrape it with Prometheus

`fastmm-top --metrics <[host:]port>` serves the same snapshot at `/metrics` in the Prometheus text format instead of drawing it.

```bash
./build/release/bin/fastmm-top --name binance-demo --metrics 9109 &
curl -s http://127.0.0.1:9109/metrics | head
```

A scrape reads the status file in `fastmm-top`'s process and never reaches `fastmm-live`. The exporter serves one request at a time and answers `fastmm_up 0` while no session publishes.

It listens on `127.0.0.1` by default. `--metrics 0.0.0.0:9109` or `--metrics '[::]:9109'` exposes it on the network, with no authentication or TLS.

```yaml
# prometheus.yml
scrape_configs:
  - job_name: fastmm
    scrape_interval: 5s          # the engine publishes every 250 ms
    static_configs:
      - targets: ["127.0.0.1:9109"]
```

| Metric | Type | Meaning |
|---|---|---|
| `fastmm_up` | gauge | 1 while a snapshot can be read |
| `fastmm_info{engine,strategy,pid,session_id}` | gauge | constant 1, labelled with what is running |
| `fastmm_state` | gauge | 0 starting, 1 running, 2 stopping, 3 stopped |
| `fastmm_status_age_seconds` | gauge | age of the snapshot; alert above a few seconds, as `fastmm-top` does with `STALE` |
| `fastmm_uptime_seconds`, `fastmm_dry_run` | gauge | session wall clock, dry-run flag |
| `fastmm_kill_active`, `fastmm_kill_latched`, `fastmm_kill_reason` | gauge | the global kill switch, the latched `max_loss` trip, and the `KillReason` |
| `fastmm_realized_pnl`, `fastmm_unrealized_pnl`, `fastmm_fees`, `fastmm_pnl_carry`, `fastmm_max_loss` | gauge | quote currency, or `[accounting] reporting_currency`; `fastmm_max_loss` is the limit the engine applies now, 0 when off |
| `fastmm_underlying_net{underlying}`, `fastmm_underlying_max_net{underlying}` | gauge | `[risk.underlying]`: the net position in base units over every instrument of the base asset (absent while an inverse contract with a position has no mark), and the limit applied now, 0 when off; only with the section |
| `fastmm_balance_free`, `fastmm_balance_locked`, `fastmm_balance_total`, `fastmm_balance_equity`, `fastmm_balance_maintenance` `{venue,asset,account}` | gauge | the balance table, asset units: free, locked and total as the engine estimates them, equity and maintenance margin as the venue reported; `account="1"` for a venue's account-wide margin; absent until the venue reports ([Balance check](../../explanation/risk-model.md#balance-check)) |
| `fastmm_perp_mark`, `_index`, `_funding_rate`, `_funding_interval_seconds`, `_next_funding_seconds`, `_open_interest`, `_mark_age_seconds`, `_funding_age_seconds`, `_valued_at_mark` `{venue,symbol}` | gauge | the venues' mark, index and funding per derivative ([Perpetuals](../../reference/strategy-api.md#perpetuals)); the rate per funding interval, ages -1 before the first report, `_valued_at_mark` 1 while the position is valued at the mark; absent until the venue reports |
| `fastmm_events_total`, `fastmm_book_updates_total`, `fastmm_orders_sent_total`, `fastmm_cancels_sent_total`, `fastmm_replaces_sent_total`, `fastmm_fills_total` | counter | engine counters |
| `fastmm_risk_rejects_total`, `fastmm_venue_rejects_total`, `fastmm_rejects_by_reason_total{kind,reason}` | counter | rejects, and the most frequent reasons the snapshot carries |
| `fastmm_quotes_withheld_balance_total` | counter | quote orders held back because the balance no longer covered them |
| `fastmm_estimated_fills_total`, `fastmm_estimates_pending`, `fastmm_estimates_forgotten_total` | counter, gauge, counter | fills booked from a cumulative quantity at the order's price, the orders whose estimate no execution has named yet, and estimates forgotten before one did; alert on any increase of the last |
| `fastmm_store_enabled`, `fastmm_store_rows_total`, `fastmm_store_errors_total`, `fastmm_store_records_dropped_total` | gauge, counter, counter, counter | a `[storage]` backend records the session, the rows it wrote, its failed writes and the records the ring dropped before it; alert on any increase of the last two |
| `fastmm_kills_total`, `fastmm_venue_kills_total` | counter | kill switch trips |
| `fastmm_treasury_in_flight`, `fastmm_treasury_sent_total`, `_done_total`, `_failed_total`, `_timed_out_total`, `_limited_total`, `_dry_run_plans_total`, `_errors_total` | gauge, counter | the pool treasuries, summed over the pools; only with a treasury |
| `fastmm_flatten_state`, `fastmm_flatten_instruments_left`, `fastmm_flatten_orders_total` | gauge, gauge, counter | the operator flatten ([Operating a running session](operate-a-running-session.md#flatten)) |
| `fastmm_latency_quantile_seconds{interval,quantile}`, `fastmm_latency_samples_total{interval}` | gauge, counter | the engine intervals above, per publishing window |
| `fastmm_venue_*{venue}` | gauge, counter | channel states, synced books, market-data messages, order traffic, reconnects, REST errors, rate-limit cooldowns, clock offset, wire tick-to-trade quantiles over the session and, in `fastmm_venue_tick_to_trade_recent_quantile_seconds{venue,window}` (`1m`, `1h`), over the last minute and hour |
| `fastmm_quote_obstacle{symbol,side}`, `fastmm_quote_obstacle_seconds{symbol,side}` | gauge | the quote table's obstacle as a number (0 quoting, 1 not wanted, 2 quoting off, 3 pulled, 4 venue killed, 5 feed lag, 6 backoff, 7 starved, 8 refused, 9 pending) and how long it has held as of the snapshot |
| `fastmm_quote_asked_qty{symbol,side}`, `fastmm_quote_working_qty{symbol,side}`, `fastmm_quote_budget{symbol,side}` | gauge | what the strategy asks for and what works, base units; the budget of its note, quote currency (0 none) |
| `fastmm_position{symbol}` | gauge | position per instrument, base units |
| `fastmm_strategy_metric{name}` | gauge | each `ctx.metric` the strategy publishes |
| `fastmm_thread_cpu_seconds_total`, `fastmm_thread_involuntary_switches_total`, `fastmm_thread_voluntary_switches_total`, `fastmm_thread_major_faults_total`, `fastmm_thread_cpu` `{thread,tid}` | counter, gauge | per thread of the session's process, from `/proc`: CPU time, times another task took its core, times it gave the core up, page faults it waited on a disk for, and the CPU it last ran on; with `fastmm_process_resident_bytes` and `fastmm_host_load1`. Absent once the session has stopped |
| `fastmm_feed_*{venue}` | gauge, counter | multicast venues only: feed state, packets, gaps, recovered and given-up sequences, per-line duplicates, L3 book errors and window moves |

The quantiles are the engine's (p50, p99, p99.9), not a histogram: they cannot be aggregated across instances.

## Alerts

`deploy/prometheus/fastmm-alerts.yml`, also in the release tarball, holds alert rules for these metrics. Add it to `rule_files` and route the alerts with Alertmanager.

```yaml
# prometheus.yml
rule_files:
  - /opt/fastmm/deploy/prometheus/fastmm-alerts.yml
```

| Severity | Fires when |
|---|---|
| `critical`, act now | the snapshot is stale (process hung or dead), the kill switch is engaged, a venue is killed, or the store failed a write or dropped records (`FastMMStoreFailing`) |
| `warning`, look soon | net loss past 80% of `max_loss`, a latched `max_loss` trip, a channel down, stale market data, books out of sync, reconnect or REST-error bursts, clock offset past 1 s; for a gateway, account loss past 80% of its `max_loss`, no strategy attached, or a strategy behind on market data |

## Reject logging

Rejects are counted per reason. A risk reject is logged at WARN the first time its reason occurs, then at most once per reason every 10 s, with the count suppressed since:

```text
WARN  ... risk reject MaxPosition on new order: binance:BTCUSDT Buy 0.01 @ 64250.1
WARN  ... risk reject MaxPosition on new order: binance:BTCUSDT Buy 0.01 @ 64251.3 (11 more suppressed)
```

Venue rejects are logged per order: `order <id> rejected: <reason> (<code>)`. At shutdown `fastmm-live` logs a breakdown for each kind that had rejects:

```text
fastmm-live: events=... fills=812 risk_rejects=17 venue_rejects=3
fastmm-live: risk_rejects by reason: MaxPosition 12, RateLimit 5
fastmm-live: venue_rejects by reason: PostOnlyWouldCross 3
```

`tools/pnl_report.py --engine-log` reads these lines. Each risk reason and the limit behind it: [Troubleshooting](troubleshooting.md#orders-and-reconciliation).
