# Monitoring a live session

`fastmm-live` writes its counters, PnL, kill-switch state, latency percentiles and each venue's status to a memory-mapped file every 250 ms without touching the engine thread; the engine's values in it refresh every second. The file is `/dev/shm/fastmm-<engine name>.status` unless `--status <path>` is given; `--no-status` turns it off.

```bash
./build/release/bin/fastmm-live --config configs/binance-demo.toml &
./build/release/bin/fastmm-top --name binance-demo          # or --path <status file>
```

`fastmm-top` redraws the dashboard in place (`--interval <ms>`, default 500; `--no-color`; `--once` prints one frame; `--json` prints one snapshot as JSON; `--metrics <port>` serves it for Prometheus instead of drawing, [below](#scrape-it-with-prometheus)). It shows:

- the session: engine and strategy name, session id, pid, dry-run flag, uptime and the state (starting, running, stopping, stopped). A running engine that has not published for more than 3 seconds is shown as `STALE`: the process has most likely died. Next to the state, `KILLED (<reason>)` means the global kill switch is engaged (the reason stays in the final `stopped` frame), `VENUE KILLED` that at least one venue's switch is, and `LATCHED` that a `max_loss` trip is recorded in `[engine] kill_file`, so the next start refuses to trade ([Kill switch and shutdown](kill-switch-and-shutdown.md#the-latched-loss-budget));
- engine counters: events, book updates, orders, cancels, replaces, fills, kill switch trips (`kills` global, `venue_kills` per venue) and flags;
- PnL, in the quote currency or `[accounting] reporting_currency`: this session's `realized` (funding included), `unrealized` and `fees`, the `carried` net PnL of earlier sessions and `budget_used`, the sum `[risk] max_loss` is compared against;
- rejects: `risk_rejects` counts orders the engine's pre-trade checks refused (they were never sent), `venue_rejects` orders a venue refused; each is followed by its most frequent reasons, for example `risk_rejects=17 (MaxPosition 12, RateLimit 5) venue_rejects=3 (PostOnlyWouldCross 3)`. Up to six reasons per kind are listed, rejects of further reasons are summed as `other <n>`;
- engine latency per interval as count, p50, p99, p99.9 and max. `decode` is the network thread's parse time; `book_apply` runs from the end of decoding on the network thread to the book update on the engine thread, so it includes the hand-off through the SPSC ring and, with `spin_mode = "adaptive"`, an idle engine's wake-up; `strategy`, `serialize` and `send` are engine thread only; `tick_to_trade` and `wire_to_book` start at the socket read;
- per venue: market-data, user and order channel states (`down`, `connecting`, `live`, `stale`; a user or order channel that is connected but quiet shows `live`), synced books, market-data messages, resyncs, orders, cancels, order events, reconnects, REST errors, clock offset and the network thread's wire tick-to-trade p50, and in the `kill` column the reason a killed venue was tripped for (`-` while it trades);
- per multicast venue (`nasdaq_itch`), a feed line: state, receive backend, packets per line, A/B skew, gaps, recovered and given-up sequences, snapshots, the reorder high-water mark and the kernel-to-T0 p50 and p99 ([Receive a multicast feed](multicast-feeds.md#7-check-the-feed)).

`fastmm-top --gateway <name>` reads a `fastmm-gateway`'s file instead (`/dev/shm/fastmm-<name>.gw.status`); the file says which it is, so `--path` works for either. Its frame and metrics are the gateway's: attachments, the account, positions ([Run behind a gateway](run-behind-a-gateway.md#monitor)).

The file stays after the session ends; the last frame shows `stopped` with the final numbers. The layout is versioned. A `fastmm-top` from another build refuses the file with `<file> was written by a different FastMM build (status segment version <n>, this fastmm-top reads version <m>); use fastmm-top from the same build as fastmm-live` (with `--once`, exit code 3, as for a missing file).

## Scrape it with Prometheus

`fastmm-top --metrics <[host:]port>` serves the same snapshot at `/metrics` in the Prometheus text format instead of drawing it.

```bash
./build/release/bin/fastmm-top --name binance-demo --metrics 9109 &
curl -s http://127.0.0.1:9109/metrics | head
```

The exporter reads the status file in `fastmm-top`'s own process; a scrape never reaches `fastmm-live`. It serves one request at a time and answers `fastmm_up 0` while no session is publishing.

The default host is `127.0.0.1`. `--metrics 0.0.0.0:9109` or `--metrics '[::]:9109'` exposes it on the network, without authentication or TLS.

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
| `fastmm_realized_pnl`, `fastmm_unrealized_pnl`, `fastmm_fees`, `fastmm_pnl_carry` | gauge | quote currency, or `[accounting] reporting_currency` |
| `fastmm_events_total`, `fastmm_book_updates_total`, `fastmm_orders_sent_total`, `fastmm_cancels_sent_total`, `fastmm_replaces_sent_total`, `fastmm_fills_total` | counter | engine counters |
| `fastmm_risk_rejects_total`, `fastmm_venue_rejects_total`, `fastmm_rejects_by_reason_total{kind,reason}` | counter | rejects, and the most frequent reasons the snapshot carries |
| `fastmm_kills_total`, `fastmm_venue_kills_total` | counter | kill switch trips |
| `fastmm_flatten_state`, `fastmm_flatten_instruments_left`, `fastmm_flatten_orders_total` | gauge, gauge, counter | the operator flatten ([Operating a running session](operate-a-running-session.md#flatten)) |
| `fastmm_latency_quantile_seconds{interval,quantile}`, `fastmm_latency_samples_total{interval}` | gauge, counter | the engine intervals above, per publishing window |
| `fastmm_venue_*{venue}` | gauge, counter | channel states, synced books, market-data messages, order traffic, reconnects, REST errors, rate-limit cooldowns, clock offset, wire tick-to-trade quantiles |
| `fastmm_feed_*{venue}` | gauge, counter | multicast venues only: feed state, packets, gaps, recovered and given-up sequences, per-line duplicates |

The quantiles are the engine's (p50, p99, p99.9), not a histogram: they cannot be aggregated across instances. Alert on `fastmm_up`, `fastmm_status_age_seconds`, `fastmm_kill_active` and the reject counters; chart the rest.

## Reject logging

Rejects are counted per reason. A risk reject is logged at WARN the first time its reason occurs, then at most once per reason every 10 seconds (`EngineConfig::reject_log_interval`), with the number of that reason's rejects not logged since its previous line:

```text
WARN  ... risk reject MaxPosition on new order: BTCUSDT Buy 0.01 @ 64250.1
WARN  ... risk reject MaxPosition on new order: BTCUSDT Buy 0.01 @ 64251.3 (11 more suppressed)
```

Venue rejects are logged per order (`order <id> rejected: <reason> (<code>)`). At shutdown `fastmm-live` logs both breakdowns after the engine counters; a kind without rejects has no breakdown line, and a long one continues on further lines with the same prefix:

```text
fastmm-live: events=... fills=812 risk_rejects=17 venue_rejects=3
fastmm-live: risk_rejects by reason: MaxPosition 12, RateLimit 5
fastmm-live: venue_rejects by reason: PostOnlyWouldCross 3
```

`tools/pnl_report.py --engine-log` reads these lines. What each risk reason means and which limit to look at: [Troubleshooting](troubleshooting.md#orders-and-reconciliation).
