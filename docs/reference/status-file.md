# Status file

`fastmm-live` publishes its live state in a memory-mapped file that monitors such as `fastmm-top` read; `fastmm-gateway` publishes its own in the same layout (`kind` 1, the [gateway block](#gateway-block)). Code: `include/fastmm/core/status_segment.hpp`. Usage: [Monitor a session](../how-to/operations/monitor-with-fastmm-top.md), [Run behind a gateway](../how-to/operations/run-behind-a-gateway.md#monitor).

## Location and lifetime

- The path is `/dev/shm/fastmm-<engine name>.status`, where the name is `[engine] name`; `fastmm-live --status <path>` chooses another path and `--no-status` turns it off. A gateway's is `/dev/shm/fastmm-<engine name>.gw.status`, with the same flags.
- The control thread creates or truncates the file at startup and rewrites the snapshot every 250 ms. It leaves the file in place at exit, so the last snapshot shows `stopped` and the final numbers.
- The engine refreshes the counters it hands to the control thread once a second (`[engine] latency_publish_ms` for latency).

## Reading it safely

The file holds an 8-byte sequence counter followed by one `StatusSnapshot`. The writer makes the counter odd, writes the snapshot, then makes it even again. A reader copies the snapshot when the counter is even and unchanged across the copy, and retries otherwise; it never blocks the writer.

- `magic` (`0x315441545353464D`, "MFSSTAT1" little-endian) and `version` (currently 9) sit at the same offsets in every version. A reader of another version refuses the file: `fastmm-top` reports `<file> was written by a different FastMM build (status segment version <n>, this fastmm-top reads version <m>)`.
- Use `fastmm-top` from the same build as `fastmm-live`; the layout is internal ([Public API](public-api.md)).

## Snapshot fields

| Field | Type | Meaning |
|---|---|---|
| `magic`, `version` | u64, u32 | see above |
| `pid` | u32 | process id of `fastmm-live` |
| `session_id` | u64 | the journal's session id |
| `started_ns`, `updated_ns` | i64 | wall-clock start and last publish, ns since the epoch |
| `state` | u8 | 0 starting, 1 running, 2 stopping, 3 stopped |
| `dry_run` | u8 | 1 in `--dry-run` |
| `venue_count` | u8 | entries used in `venues` |
| `kill_reason` | u8 | `KillReason` of the global kill switch, 0 while it is not set (below) |
| `engine_name`, `strategy` | char[32] | NUL-terminated |
| `events`, `book_updates` | u64 | events consumed, book updates applied |
| `orders_sent`, `cancels_sent`, `replaces_sent` | u64 | order messages sent |
| `fills` | u64 | executions |
| `risk_rejects`, `venue_rejects` | u64 | orders refused by the pre-trade checks, by a venue |
| `risk_reject_reasons`, `venue_reject_reasons` | 6 x {u64 count, u8 reason} | the most frequent `RejectReason`s, most frequent first; count 0 marks an unused entry |
| `kills`, `kill_flags`, `venue_kills` | u64, u32, u32 | global kill switch trips, the current flag word, per-venue kill switch trips |
| `kill_latched` | u8 | 1 while a `max_loss` trip is latched in `[engine] kill_file` |
| `flatten_state` | u8 | operator flatten: 0 off, 1 working, 2 flat, 3 timed out, 4 stopped ([Operating a running session](../how-to/operations/operate-a-running-session.md#flatten)) |
| `flatten_instruments_left` | u32 | instruments in the flatten's scope that still hold a position |
| `flatten_orders` | u64 | reduce-only orders the flatten has sent |
| `kind` | u8 | 0 `fastmm-live`, 1 `fastmm-gateway` |
| `realized_pnl_raw`, `unrealized_pnl_raw`, `fees_raw` | i64 | settlement currency, or `[accounting] reporting_currency` when it is set; raw fixed point (divide by 1e8) |
| `quoting_elapsed_ns`, `quoting_two_sided_ns` | i64 | time since the first order rested, and how much of it had a live order on both sides; a market-maker programme measures its rebate this way |
| `latency` | 7 x {count, p50_ns, p99_ns, p999_ns, max_ns} | engine latency intervals, below |
| `venues` | 8 x venue entry | below |
| `gateway` | gateway block | `kind` 1 only, zero otherwise ([below](#gateway-block)) |

A `fastmm-top` session is `STALE` when `state` is running and `updated_ns` is more than 3 s old.

### Kill flags

| Bit | Value | Meaning |
|---|---|---|
| 0 | `0x1` | global kill switch: no new orders on any venue |
| 1 + venue id | `0x2` (venue 0), `0x4` (venue 1), ... | one venue only; venues from id 30 share bit 31 |

### Kill reasons

`KillReason` (`core/enums.hpp`), the first reason each flag was set for; `fastmm-top` shows it as `KILLED (<reason>)` next to the state and in each venue's `kill` column.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `None` | not killed |
| 1 | `Requested` | shutdown or operator (`ControlCommand::TripKill`) |
| 2 | `MaxLoss` | `[risk] max_loss` reached |
| 3 | `TransportFull` | the outbound ring to a venue was full |
| 4 | `JournalOverflow` | the journal ring was full |
| 5 | `AllVenuesKilled` | every venue with instruments has its own kill bit set |
| 6 | `VenueFatal` | venue error map: bad key, signature or permission, failed authentication |
| 7 | `VenueHardStop` | venue error map: REST stopped (IP ban) |
| 8 | `OrderRingOverflow` | a venue's order-event ring overflowed |
| 9 | `StrategyError` | a strategy hook reported an error; `fastmm-top` shows `StrategyError` |
| 10 | `FeedLost` | a multicast venue cannot rebuild its books ([Venue connectors](venues.md#startup-and-recovery)) |
| 11 | `OrderIdsExhausted` | the session's client order id sequence ran out |
| 12 | `DeadMansSwitchLost` | a venue-side countdown could not be refreshed within its window |
| 13 | `GatewayMaxLoss` | the gateway's `[gateway] max_loss` over every strategy |
| 14 | `GatewayOperator` | an operator's `kill` on the gateway's control socket |

### Latency intervals

Index order of `latency`, each from the named stamps ([Architecture](../explanation/architecture.md#latency-instrumentation)):

| Index | Interval | Stamps |
|---:|---|---|
| 0 | decode | T0 (socket read) to T1 (decoded) |
| 1 | book apply | T1 to T2 (book updated on the engine thread) |
| 2 | strategy | T2 to T3 (hook returned) |
| 3 | serialize | T3 to T4 (batch handed to the transport) |
| 4 | send | T4 to T5 (push into the outbound ring; with `spin_mode = "adaptive"` also the eventfd write that wakes the network thread) |
| 5 | tick to trade | T0 to T5 |
| 6 | wire to book | T0 to T2 |

### Venue entry

| Field | Type | Meaning |
|---|---|---|
| `name` | char[24] | `[venues.<name>]` |
| `md`, `user`, `order` | u8 | channel states: 0 down, 1 connecting, 2 live, 3 stale (market data only: a quiet user or order channel is normal and shows as live) |
| `killed`, `kill_reason` | u8, u8 | this venue's kill switch is engaged, and its `KillReason` |
| `books_synced`, `books_total` | u32 | books in sync out of subscribed |
| `md_messages`, `resyncs` | u64 | market-data messages, book resynchronisations |
| `orders_sent`, `cancels_sent`, `replaces_sent`, `order_events` | u64 | order traffic on this venue |
| `reconnects`, `rest_errors`, `rate_limit_cooldowns` | u64 | connection health |
| `clock_offset_ms` | i64 | venue clock minus local clock, ms |
| `wire_tick_to_trade` | {count, p50_ns, p99_ns, p999_ns, max_ns} | socket read to order write on the network thread (the return of the write that carried the order; one write per drain of the outbound ring) |
| `feed` | feed entry | multicast venues only (below); `state` 0 for the others |

### Multicast feed

The `feed` entry of a `nasdaq_itch` venue ([Venue connectors](venues.md#nasdaq-totalview-itch-nasdaq_itch)); counters are cumulative for the session.

| Field | Type | Meaning |
|---|---|---|
| `state` | u8 | 0 none (not a multicast venue), 1 down, 2 snapshot (buffering while a GLIMPSE snapshot is taken), 3 live, 4 lost |
| `backend`, `xdp_mode` | u8, u8 | 0 `kernel`, 1 `af_xdp`, 2 `dpdk`; the attach mode: 1 zerocopy, 2 native_copy, 3 generic |
| `packets`, `bytes` | u64 | MoldUDP64 packets accepted on every line, datagram payload bytes |
| `line_packets`, `line_duplicates` | 2 x u64 | per line A, B: packets, copies that arrived after the first copy |
| `line_skew_mean_ns`, `line_skew_max_ns` | 2 x i64 | per line: delay of a duplicate behind the first copy, ns |
| `gaps`, `recovered`, `unrecovered` | u64 | gaps declared, messages delivered from re-requests, sequences given up |
| `snapshot_recoveries`, `recovery_overflows` | u64 | GLIMPSE snapshots after the first, recovery buffer overflows |
| `reorder_high_water`, `requests` | u64 | most packets held ahead of a gap, re-request packets sent |
| `malformed`, `book_errors` | u64 | datagrams that are not MoldUDP64 packets (and bad frames on `af_xdp` and `dpdk`), L3 book inconsistencies |
| `kernel_to_t0` | {count, p50_ns, p99_ns, p999_ns, max_ns} | kernel receive timestamp to T0 while live (`kernel` backend) |
| `xdp_rx_dropped`, `xdp_rx_invalid_descs`, `xdp_rx_ring_full`, `xdp_fill_ring_empty` | u64 | `XDP_STATISTICS` summed over the sockets; on `dpdk`, `xdp_rx_dropped` is the port's `imissed` plus `rx_nombuf` |
| `xdp_fallback` | u64 | subscribed datagrams passed to the kernel because their RX queue has no socket |

## Gateway block

A gateway (`kind` 1) fills the header (`pid`, times, `state`, `dry_run`, `engine_name`, `venue_count`, `venues`), `kill_flags` (bit 0 while the account is killed), `kill_reason`, `kill_latched` (the kill file records the trip, which needs `[gateway] max_loss`), and `realized_pnl_raw`, `unrealized_pnl_raw`, `fees_raw`, `pnl_carry_raw` with the account's. The engine counters and latencies stay zero. Everything in it comes from totals the network threads keep anyway; publishing asks them nothing.

| Field | Type | Meaning |
|---|---|---|
| `attachment_count`, `position_count` | u32, u32 | entries used below |
| `kill_active` | u8 | the account's kill switch is tripped |
| `net_pnl_raw`, `gross_raw`, `net_raw` | i64 | the account's net PnL (carried + realized + unrealized − fees), gross and net exposure |
| `trip_net_raw` | i64 | the net PnL when it tripped |
| `max_loss_raw`, `max_gross_raw`, `max_net_raw`, `max_open_notional_raw` | i64 | `[gateway]` limits, 0 off |
| `venues` | 8 x routing entry | per venue: `md_discarded`, `order_discarded`, `unrouted`, `gateway_cancels`, `untracked`, `stale_replays`, `account_skipped`, `account_md_lost` (u64), `refused` (7 x u64), and the account on that venue: `realized_raw`, `unrealized_raw`, `fees_raw`, `gross_raw`, `net_raw` |
| `attachments` | 16 x attachment | `engine` (char[32]), `pid`, `id` (u32), `epoch` (u16), `blocks` (u8: its engine sleeps when idle), `attached_ns` (i64), `md_dropped` (u64: market data its rings dropped), `refused` (7 x u64) |
| `positions` | 256 x position | one per instrument of the gateway's table: `symbol` (char[24]), `venue` (u8, index into `venues`), `owner_epoch` (u16, the attachment that trades it, 0 none), `qty_raw` (i64) |

`refused` counts in the order `GatewayNotOwner`, `GatewayAccountKilled`, `GatewayOpenNotional`, `GatewayGrossNotional`, `GatewayNetNotional`, `GatewayRateLimit`, `GatewayFxRateUnknown` ([Reject reasons](errors.md#gateway)). An attachment's instruments are the positions whose `owner_epoch` is its epoch.

`fastmm-top --json` prints the snapshot as one JSON object, with states, kill reasons and latency intervals by name ([Command lines](cli.md#fastmm-top)); `scripts/bench-e2e.sh` reads it.
