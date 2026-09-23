# Status file

`fastmm-live` publishes its live state in a memory-mapped file that monitors such as `fastmm-top` read. Code: `include/fastmm/core/status_segment.hpp`. Usage: [Monitor a session](../how-to/operations/monitor-with-fastmm-top.md).

## Location and lifetime

- The path is `/dev/shm/fastmm-<engine name>.status`, where the name is `[engine] name`; `fastmm-live --status <path>` chooses another path and `--no-status` turns it off.
- The control thread creates or truncates the file at startup and rewrites the snapshot every 250 ms. It leaves the file in place at exit, so the last snapshot shows `stopped` and the final numbers.
- The engine refreshes the counters it hands to the control thread once a second (`[engine] latency_publish_ms` for latency).

## Reading it safely

The file holds an 8-byte sequence counter followed by one `StatusSnapshot`. The writer makes the counter odd, writes the snapshot, then makes it even again. A reader copies the snapshot when the counter is even and unchanged across the copy, and retries otherwise; it never blocks the writer.

- `magic` (`0x315441545353464D`, "MFSSTAT1" little-endian) and `version` (currently 6) sit at the same offsets in every version. A reader of another version refuses the file: `fastmm-top` reports `<file> was written by a different FastMM build (status segment version <n>, this fastmm-top reads version <m>)`.
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
| `realized_pnl_raw`, `unrealized_pnl_raw`, `fees_raw` | i64 | quote currency, raw fixed point (divide by 1e8) |
| `latency` | 7 x {count, p50_ns, p99_ns, p999_ns, max_ns} | engine latency intervals, below |
| `venues` | 8 x venue entry | below |

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
| `md`, `user`, `order` | u8 | channel states: 0 down, 1 connecting, 2 live, 3 stale |
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

`fastmm-top --json` prints the snapshot as one JSON object, with states, kill reasons and latency intervals by name ([Command lines](cli.md#fastmm-top)); `scripts/bench-e2e.sh` reads it.
