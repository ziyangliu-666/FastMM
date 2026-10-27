# Nasdaq TotalView-ITCH

`kind = "nasdaq_itch"` reads TotalView-ITCH 5.0 market data over MoldUDP64 and enters orders over OUCH 5.0 to [fastmm-sim-itch](../sim-itch.md) (`configs/nasdaq-itch-sim.toml`).

| | |
|---|---|
| Markets | Nasdaq-listed stocks |
| Market data | ITCH 5.0 over MoldUDP64, lines A and B, multicast or unicast; initial book from GLIMPSE 5.0 |
| Order entry | OUCH 5.0 over SoupBinTCP to `fastmm-sim-itch`, or none |
| Keys | none; OUCH logs in with `ouch_username` and `ouch_password` |
| Reference data | tick and lot from `[[instruments]]` |
| Example config | `configs/nasdaq-itch-sim.toml` |

There is no REST connection. The connector declares that it needs no credentials, so `fastmm-live` never asks for `api_key` or `api_secret`. Setup: [Receive a multicast feed](../../how-to/operations/multicast-feeds.md). Protocol details: [Nasdaq codecs](../codecs/nasdaq.md).

## Pipeline

| Stage | What it does |
|---|---|
| receiver, `rx_backend` | reads lines A and B, keeps the first copy of each packet, reorders, re-requests gaps |
| recovery buffer | holds messages while a GLIMPSE snapshot loads |
| ITCH to L2 bridge | one L3 book per instrument |
| market-data ring | `BookSnapshot`, `BookDelta`, `Trade` |

`rx_backend` picks the datagram source for both lines: the kernel socket or AF_XDP.

## Startup and recovery

1. `connect()` joins both lines and buffers up to `recovery_buffer_packets` datagrams while it logs in to GLIMPSE. Once the Stock Directory has named every subscribed stock's locate, only those stocks' messages and R and S are kept. The gaps the rest of the market leaves in the sequence numbers are not taken for lost messages.
2. The snapshot's Stock Directory, Trading Action and Add Order messages build the L3 books. End of Snapshot gives the sequence number to continue from.
3. The buffered messages from that number on are applied. When the buffer starts after it, the receiver moves back and re-requests the missing messages; without `rerequest` another snapshot is taken.
4. Each book emits a `BookSnapshotMsg` with the top `depth` levels per side, and the venue a `ConnectionStateMsg{Live}`.

The same procedure runs again after a `ConnectionStateMsg{Resyncing}` that clears the engine's books:

| trigger | why |
|---|---|
| a gap the receiver gives up: no `rerequest`, or `max_request_attempts` unanswered requests | the book misses messages |
| any L3 book error, such as an unknown or duplicate order reference | later messages cannot repair it |
| the recovery buffer filling during a snapshot | the snapshot is taken again |

A book whose capacity (`max_orders`, `max_overflow_levels`) refuses an order is not resynchronised, because a new snapshot would refuse it again. That book alone leaves service with a `Resyncing` for its instrument (`reason_code` 1), which clears it in the engine. Its messages are skipped and the venue logs an error. The other books stay live, and the full one comes back with the next resync.

| event | result |
|---|---|
| the buffer fills during two snapshots in a row | the feed stops: the venue's kill switch trips with `KillReason::FeedLost`, feed state `lost` |
| GLIMPSE fails or closes before End of Snapshot | retried after 1 s |
| no `glimpse_url` | the books start empty at sequence 1; a gap given up stops the feed the same way |

## Events

Each datagram yields at most one `BookDeltaMsg` per instrument, with absolute quantities of the top `depth` levels that changed; 0 deletes. Each printable execution yields a `TradeMsg`: E, C with Printable Y, P, Q. A datagram's trades come before its deltas, which already show what they took. The engine's queue estimate counts each trade once in that order.

| field | value |
|---|---|
| `t0_cycles` (T0) | the `rdtscp` of the receive batch that carried the datagram, also for messages delivered later from the reorder buffer |
| `recv_ts` | the kernel receive time; the NIC time with `hw_clock = "phc_synced"`; the T0 wall clock on `af_xdp` |
| `venue_seq` | the MoldUDP64 sequence number of the last message applied |
| T1 | taken after decoding and the L3 update |

## Orders

| `order_entry` | orders |
|---|---|
| `none` | refused at once: `OrderRejectMsg` with `RejectReason::VenueReject`, text `order_entry = none` |
| `sim_ouch` | OUCH 5.0 over SoupBinTCP to `ouch_url`: Enter, Replace, Cancel; Accepted, Replaced, Canceled, Executed and Rejected become order events |

With `sim_ouch`, an Enter or Replace triggered by market data carries a sequence token as ClOrdID: `T` + 13 digits, the first ITCH message of its receive batch. The simulator times it wire to wire. The engine copies only `t0_cycles` into outbound messages, so the venue maps each of the last 4096 batches' `t0_cycles` to its sequence number.

The simulator cancels a connection's orders when it closes. A lost OUCH connection is reported as `ConnectionStateMsg{Disconnected}` on channel 1 and a reconciliation without open orders; the connection is retried after 1 s. `cancel_all()` shuts the OUCH connection down from the calling thread.

## Status

The venue fills the feed block of its status entry ([Status file](../status-file.md#multicast-feed)):

- per-line packets, duplicates and A/B skew;
- gaps, recovered and given-up sequences, snapshots, requests;
- the reorder high-water mark, malformed datagrams and L3 book errors;
- the kernel-to-T0 histogram;
- on `af_xdp`, the XDP statistics and mode.

The connector reports no balances and no mark price.
