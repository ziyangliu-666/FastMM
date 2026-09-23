# Monitoring a live session

`fastmm-live` writes its counters, PnL, kill-switch state, latency percentiles and each venue's status to a memory-mapped file every 250 ms without touching the engine thread; the engine's values in it refresh every second. The file is `/dev/shm/fastmm-<engine name>.status` unless `--status <path>` is given; `--no-status` turns it off.

```bash
./build/release/bin/fastmm-live --config configs/binance-demo.toml &
./build/release/bin/fastmm-top --name binance-demo          # or --path <status file>
```

`fastmm-top` redraws the dashboard in place (`--interval <ms>`, default 500; `--no-color`; `--once` prints one frame; `--json` prints one snapshot as JSON). It shows:

- the session: engine and strategy name, session id, pid, dry-run flag, uptime and the state (starting, running, stopping, stopped). A running engine that has not published for more than 3 seconds is shown as `STALE`, which usually means the process died. Next to the state, `KILLED (<reason>)` means the global kill switch is engaged (the reason stays in the final `stopped` frame), `VENUE KILLED` that at least one venue's switch is, and `LATCHED` that a `max_loss` trip is recorded in `[engine] kill_file`, so the next start refuses to trade ([Kill switch and shutdown](kill-switch-and-shutdown.md#the-latched-loss-budget));
- engine counters: events, book updates, orders, cancels, replaces, fills, kill switch trips (`kills` global, `venue_kills` per venue) and flags;
- PnL: this session's `realized`, `unrealized` and `fees`, the `carried` net PnL of earlier sessions and `budget_used`, the sum `[risk] max_loss` is compared against;
- rejects: `risk_rejects` counts orders the engine's pre-trade checks refused (they were never sent), `venue_rejects` orders a venue refused; each is followed by its most frequent reasons, for example `risk_rejects=17 (MaxPosition 12, RateLimit 5) venue_rejects=3 (PostOnlyWouldCross 3)`. Up to six reasons per kind are listed, rejects of further reasons are summed as `other <n>`;
- engine latency per interval as count, p50, p99, p99.9 and max. `decode` is the network thread's parse time; `book_apply` runs from the end of decoding on the network thread to the book update on the engine thread, so it includes the hand-off through the SPSC ring and, with `spin_mode = "adaptive"`, an idle engine's wake-up; `strategy`, `serialize` and `send` are engine thread only; `tick_to_trade` and `wire_to_book` start at the socket read;
- per venue: market-data, user and order channel states, synced books, market-data messages, resyncs, orders, cancels, order events, reconnects, REST errors, clock offset and the network thread's wire tick-to-trade p50, and in the `kill` column the reason a killed venue was tripped for (`-` while it trades);
- per multicast venue (`nasdaq_itch`), a feed line: state, receive backend, packets per line, A/B skew, gaps, recovered and given-up sequences, snapshots, the reorder high-water mark and the kernel-to-T0 p50 and p99 ([Receive a multicast feed](multicast-feeds.md#7-check-the-feed)).

The file stays after the session ends, so the last frame shows `stopped` with the final numbers. The layout is versioned (magic number and version field). A `fastmm-top` from another build refuses the file with `<file> was written by a different FastMM build (status segment version <n>, this fastmm-top reads version <m>); use fastmm-top from the same build as fastmm-live` (with `--once`, exit code 3, as for a missing file).

## Reject logging

Rejects are counted per reason; logging is rate limited. A risk reject is logged at WARN the first time its reason occurs, then at most once per reason every 10 seconds (`EngineConfig::reject_log_interval`), with the number of that reason's rejects not logged since its previous line:

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
