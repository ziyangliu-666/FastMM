# Nasdaq protocol family: ITCH 5.0, MoldUDP64, SoupBinTCP, OUCH 4.2 / 5.0

The wire codecs and session layers depend only on `fastmm::core`. Hot paths (framers, decoders, encoders, session `on_frame` / `on_timer`) are `noexcept` and allocate nothing after construction (`tests/hotpath/codecs_nasdaq_noalloc_test.cpp`).

| Protocol | Namespace | Headers | Specification used |
|---|---|---|---|
| TotalView-ITCH 5.0 | `fastmm::codecs::itch` | `include/fastmm/codecs/itch/` | Nasdaq TotalView-ITCH 5.0, `NQTVITCHspecification.pdf` (revision log entry 2023-04-28) |
| MoldUDP64 1.00 | `fastmm::codecs::moldudp` | `include/fastmm/codecs/moldudp/` | MoldUDP64 Protocol Specification V 1.00, `moldudp64.pdf` (formatting update 2024-08-02) |
| SoupBinTCP 3.00 / 4.00 / 4.10 | `fastmm::codecs::soupbin` | `include/fastmm/codecs/soupbin/` | `soupbintcp.pdf` (3.00), `SoupBinTCP 4.0.pdf` (4.00, 2010-07-12), `SoupBinTCP41.pdf` (4.10, 2012-01-13) |
| OUCH 4.2 | `fastmm::codecs::ouch42` | `include/fastmm/codecs/ouch/ouch42.hpp` | O*U*C*H Version 4.2, `OUCH4.2.pdf` (updated October 2025) |
| OUCH 5.0 | `fastmm::codecs::ouch50` | `include/fastmm/codecs/ouch/ouch50.hpp` | OUCH 5.0 Order Entry Specification, `Ouch5.0.pdf` (updated October 2025) |

All documents were fetched from nasdaqtrader.com / nasdaq.com (September 2026). Shared field helpers (6-byte timestamps, exact price and share conversions, alpha and ASCII numeric fields) live in `include/fastmm/codecs/itch/nasdaq_fields.hpp` (`fastmm::codecs::nasdaq`).

## Wire layouts

Every message is a `#pragma pack(1)` struct built from the alignment-1 `be16_t` / `be32_t` / `be64_t` fields of `codec.hpp` (read and written only through `get()` / `set()`), with `static_assert`s on `sizeof` and on the offsets of the fields the code touches. `tests/fixtures/nasdaq/generate_fixtures.py` packs every message with Python `struct.pack` from the specification tables; the tests check that the C++ structs read those bytes and that the encoders and host-side builders reproduce them byte for byte.

ITCH 5.0 message lengths (bytes): S 12, R 39, H 25, Y 20, L 26, V 35, W 12, K 28, J 35, h 21, A 36, F 40, E 31, C 36, X 23, D 19, U 35, P 44, Q 40, B 19, I 50, N 20, O 48.

OUCH 4.2: inbound O 49, U 47, X 19, M 20; outbound S 10, A 66, U 80, C 28, D 38, E 40, B 32, G 45, J 24, P 23, I 23, T 36, M 28.

OUCH 5.0 (fixed part before Appendage Length, then `Appendage Length(2)` and TagValue options): inbound O 45, U 38, X 9, M 10, C 17, D / E 9, Q 1; outbound S 10 (no appendage), A 62, U 66, C 18, D 32, E 34, B 36, J 29, P / I 13, T 30, M 18, R 14, X 25, G / K 17, Q 13. For C D B J P I T M Q the specification says Appendage Length may be absent; the decoder accepts both forms.

## Prices, quantities, timestamps

* Price(4) (ITCH, OUCH 4.2 4-byte, OUCH 5.0 8-byte) to engine `Price` (1e-8): multiply by 10 000. Always exact for 4-byte fields; 8-byte fields are range-checked.
* Price(8) (ITCH MWCB levels) is the engine scale already.
* Engine price to Price(4) is refused (encoder returns 0) when the price is negative, has more than 4 decimals or does not fit. Share counts must be whole; OUCH enforces `< 1,000,000` and `<= $199,999.9900`.
* ITCH and OUCH timestamps are nanoseconds since midnight. Decoders set `EventHeader::exch_ts = set_midnight() + timestamp`; the caller supplies the Unix nanoseconds of midnight of the trading day in US/Eastern. `recv_ts` is the `rx_ts` argument and `venue_seq` the value passed to `set_venue_seq()` (the MoldUDP64 / SoupBinTCP sequence).

## ITCH 5.0

`ItchDecoder` (satisfies `Decoder`) dispatches on the first byte:

| ITCH | Engine event |
|---|---|
| A, F Add Order | `OrderAddL3Msg` |
| E Order Executed | `OrderExecL3Msg`, `exec_price` 0 (= at the order price) |
| C Order Executed With Price | `OrderExecL3Msg`, `exec_price` = Execution Price, `exec_flags` `kNonPrintable` unless Printable is `Y` |
| X Order Cancel | `OrderCancelL3Msg`, `canceled_qty` = Cancelled Shares |
| D Order Delete | `OrderCancelL3Msg`, `canceled_qty` 0 (= delete) |
| U Order Replace | `OrderReplaceL3Msg` |
| P Trade (non-displayable) | `TradeMsg` |
| Q Cross Trade | `TradeMsg` (skipped when Shares is 0) |
| R Stock Directory | stock locate -> `InstrumentId` table, no event |
| S H Y L V W K J h B I N O | validated, ignored |

Register symbols with `add_symbol()` before the directory spin (or `map_locate()` for replay). The locate table is a flat 65 536-entry array; messages for unmapped locates are ignored and counted in `stats().unknown_locate`. `ItchEncoder` writes the same messages (simulation, replay) and refuses values it cannot represent exactly.

`decode()` writes into an `EventSink`. `decode_into()` also takes a `ScratchSink`, which holds the one event of a message for callers that consume it in place.

## ITCH to L2 (`ItchL2Bridge`)

`include/fastmm/codecs/itch/itch_l2_bridge.hpp` (ADR-0015, section 5). Keeps one `L3Book` per configured instrument and writes engine events to an `EventSink`. The caller passes each ITCH message with its sequence number and the datagram's `DatagramStamp` (`t0_cycles`, `recv_ts`) to `on_itch_message()`, then calls `end_datagram()`.

| Input | Effect |
|---|---|
| R, S (any locate) | R maps the locate of a symbol registered with `add_instrument()`; S is kept in `last_system_event()` |
| other types, unconfigured locate | skipped after the 11-byte header (`stats().skipped`) |
| A F E C X D U | applied to the instrument's `L3Book`; unknown or duplicate references count in `stats().book_errors` |
| E, C with Printable `Y` | `TradeMsg`: resting order price (E) or Execution Price (C), aggressor opposite to the resting side, `trade_id` = Match Number |
| P, Q | `TradeMsg` from `ItchDecoder` |

Books start incomplete: they are updated, nothing is emitted. `mark_complete(id)` (after GLIMPSE, or before sequence 1) emits a `BookSnapshotMsg` with `kSnapshot` and the top `depth` levels per side (default 20); once every book is complete, `ConnectionStateMsg{Live, channel 0}` follows. `mark_incomplete(reason)` clears every book and emits `ConnectionStateMsg{Resyncing, channel 0}`.

`end_datagram()` emits at most one `BookDeltaMsg` per complete book: the top-`depth` levels that differ from the last emitted top, with absolute quantities (0 deletes). A level that enters the top because another emptied is included, and a level pushed out is deleted, so the engine's book holds exactly the top `depth`. Changes strictly below the emitted top of a full side do not trigger a delta. `first_update_id` / `last_update_id` are the sequences of the first and last message applied since the previous emission, `prev_update_id` is the previous emission's `last_update_id`, and `venue_seq` is `last_update_id`. A delta the sink has no room for is counted in `stats().overflow` and sent with the next one.

Every emitted message carries the stamp's `t0_cycles` and `recv_ts`; `t1_delta` is taken after the decode and the L3 update.

`L3Book` (`core/book/l3_book.hpp`) takes `L3BookConfig{price_window_ticks, max_orders, max_overflow_levels}` and allocates everything in the constructor. The bridge uses ITCH's tick, 0.0001. Levels outside the window, such as stub quotes, go to a bounded overflow store per side. The window moves only when a touch leaves it, to the midpoint of the touches when they are less than a window apart. When they are further apart, the window stays on the touch it holds (moves to the bid when it holds neither).

## MoldUDP64

`parse_packet()` validates a datagram (20-byte header, exactly `count` length-prefixed blocks), `MessageFramer` / `MessageIterator` walk the messages with their implied sequence numbers, `PacketBuilder`, `write_heartbeat()`, `write_end_of_session()` and `write_request()` build packets.

`Receiver<Handler>` (satisfies `SessionLayer`) delivers messages to `Handler::on_message(seq, msg)` strictly in sequence. A gap is detected from a data packet starting beyond the next expected sequence or from a heartbeat / end-of-session announcing a higher next sequence. It then sends a Request Packet through `Handler::send_request()` (capped at `max_request_count` messages), asks for the next chunk as soon as a retransmission makes progress, and re-sends the request from `on_timer()` after `request_timeout_ns`. Packets ahead of the gap are dropped, not buffered: the request covers them. `Handler::on_end_of_session()` fires once every message before End of Session was delivered. `Transmitter` keeps a fixed-capacity history, builds live packets up to `max_datagram` and answers Request Packets.

## SoupBinTCP

`SoupBinFramer` splits the TCP stream (payload excludes the type byte, `FrameView::kind` is the packet type); builders and parsers cover every packet type (`+ A J S U H Z L R O`).

`ClientSession<Writer>` (satisfies `SessionLayer`): `login(now)` sends the Login Request with the requested session and sequence number (4.10 layout adds the heartbeat timeout). `on_frame()` returns `false` for Sequenced (S) and server Unsequenced (U, 4.00+) data so the decoder sees them, counting S packets for the implied sequence number. `on_timer()` sends a Client Heartbeat after `heartbeat_interval_ns` without sending and drops the session after `server_timeout_ns` without receiving or `login_timeout_ns` without an answer. After `on_disconnect()` the next `login()` resumes the accepted session at the next expected sequence number.

`ServerSession<Writer, Handler>` (tests, simulation) validates logins (username and password case-insensitive, blank or matching session), answers Accepted / Rejected, replays its sequenced history from the requested sequence, sends Server Heartbeats, forwards client Unsequenced Data and closes on logout, login timeout or client timeout (the 4.10 Heartbeat Timeout overrides the configured one). A requested sequence past the end starts at the next message generated.

## OUCH 4.2

Order Token (14 alphanumeric characters, day-unique per account) is `encode_cl_ord_id(cl_ord_id)` (`"fm"` + 12 lowercase hex digits); `decode_cl_ord_id()` reverses it.

`OuchEncoder` (satisfies `Encoder`): New -> Enter Order, Replace -> Replace Order (existing token = `orig_cl_ord_id`, replacement = `cl_ord_id`), Cancel -> Cancel Order with Shares 0. Time in Force: GTC 99999 (system hours), DAY 99998 (market hours), IOC 0, FOK 0 with Minimum Quantity = Shares. PostOnly uses Display `P`. Market orders are refused. Firm, display, capacity, intermarket sweep, cross type, customer type and the replace TIF come from `EncoderConfig`.

`OuchDecoder` (satisfies `Decoder`) keeps a fixed-capacity order table (client order id, reference number, leaves, cumulative quantity, side, instrument):

| OUCH outbound | Engine event |
|---|---|
| A Accepted | `OrderAckMsg` (venue order id = Order Reference Number); plus `OrderExpiredMsg` when Order State is D |
| U Replaced | `OrderAckMsg` for the replacement token (the OMS completes the replace on the pending id); cumulative quantity carries over the chain |
| C Canceled, D AIQ Canceled | once no shares are left: `OrderExpiredMsg` for reasons I (IOC) and T (timeout), `OrderCancelAckMsg` otherwise; partial reductions only update the table |
| E Executed, G Executed with Reference Price | `OrderFillMsg` (exec id = Match Number, liquidity from the flag) |
| J Rejected | `OrderRejectMsg` (`VenueReject`, `venue_code` = reason character) |
| I Cancel Reject | `OrderCancelRejectMsg` |
| S System Event, B Broken Trade, P Cancel Pending, T Priority Update, M Order Modified | no event (M updates leaves) |

## OUCH 5.0

UserRefNum is a 4-byte number that must be day-unique and strictly increasing per port. It is not derived from `ClientOrderId`. `UserRefMap` assigns numbers from a counter (the first value is configurable, and `set_next()` takes an Account Query Response's NextUserRefNum), recording `ClientOrderId` <-> UserRefNum in both directions. The encoder assigns a number on New and on the replacement leg of Replace, and looks it up for Cancel and for OrigUserRefNum. The ClOrdID field always carries `encode_cl_ord_id(cl_ord_id)`, which Nasdaq echoes in Accepted, Replaced, Rejected and Broken Trade. The decoder resolves UserRefNum through the shared map (falling back to the echoed ClOrdID) and erases both directions when an order is done.

Time In Force: DAY and GTC -> `0` (Day; OUCH 5.0 has no good-till-cancel), IOC -> `3`, FOK -> `3` with the MinQty option. PostOnly uses the PostOnly option `P`; a non-blank firm adds the Firm option. The event mapping matches OUCH 4.2; Rejected carries a 2-byte numeric reason.

## Simulation validation

* `tests/codecs/itch_sim_property_test.cpp`: seeded random flow (limit, IOC, FOK, post-only, market, cancels, in-place amends and re-entering replaces) into the simulator's `MatchingEngine`. Its book events are published as ITCH (A/F, E/C, X, D, U, P), decoded and applied to an `L3Book`. After every step the L3 book must equal the engine's book: levels, quantities and each level's FIFO of (reference number, leaves). 16 seeds x 1 500 steps.
* `tests/codecs/ouch_session_sim_test.cpp`: an OUCH client (encoder, SoupBinTCP client session, decoder) against a small OUCH acceptor (SoupBinTCP server session bridged to `MatchingEngine`), with a second account trading against it. Fills (count, quantity and notional per side) match the engine ledger, and open orders match the engine (ids, leaves, side), for OUCH 4.2 and 5.0, 12 seeds each.
* `tests/codecs/itch_sim_property_test.cpp` also runs the flow with 5 % stub quotes and a drifting reference price through a 64-tick L3 window, so most levels sit in the overflow store and the window recentres.
* `tests/codecs/itch_l2_bridge_test.cpp`: the same flow with stub quotes, a drifting market and C executions with Printable N and Y, grouped into datagrams of 1 to 12 messages and fed to `ItchL2Bridge`. After every datagram an `L2Book` built from the bridge's events equals the top `depth` of a `std::map` book rebuilt from the same ITCH bytes, and the trades match its executions. Each run starts incomplete, is marked complete, loses its books and recovers from a GLIMPSE-like spin. 12 seeds x 2 000 steps, depth 5 and 20, window 1 024 and 65 536 ticks.
* `tests/codecs/moldudp_loss_test.cpp`: 20 seeds x 3 000 messages through a channel that drops 15 % of packets (10 % of retransmissions), duplicates and reorders. Every message is delivered once, in order, with the published contents.

## Benchmarks

`bench/bench_codecs_nasdaq.cpp`. Each benchmark iteration times a batch of 64 operations with rdtsc; `p50_ns` is the median per-operation time. Measured on the development machine (WSL2, gcc 13.3, RelWithDebInfo, `-march=x86-64-v2`, pinned with `--cpu=5`, 3 repetitions of 1 s) while other builds were running (load average about 6.7; see [Benchmarks](../../explanation/benchmarks.md#caveats)):

| Benchmark | What is timed | p50 |
|---|---|---|
| `BM_Itch_DecodeAddOrder` | ITCH 'A' (36 bytes) -> `OrderAddL3Msg` committed to an `EventSink` | 15.4 ns |
| `BM_Itch_DecodeOrderExecuted` | ITCH 'E' (31 bytes) -> `OrderExecL3Msg` committed to an `EventSink` | 15.9 ns |
| `BM_ItchL2Bridge_Message` | `ItchL2Bridge::on_itch_message()` per message of a replayed A/E/C/D/U stream around one touch, `end_datagram()` every 8 messages (a delta for about 1 in 8 messages). Measured separately: `release-native`, `taskset -c 5`, load average 1.5 | 60 ns |
| `BM_MoldUdp64_FramePacket` | `parse_packet()` + iterate a 10-message packet | 19.5 ns per packet |
| `BM_SoupBin_FrameSequenced` | `SoupBinFramer::next()` + `ClientSession::on_frame()` for a Sequenced Data packet | 2.4 ns |
| `BM_Ouch42_EncodeEnterOrder` | `OrderCommand` -> OUCH 4.2 Enter Order (49 bytes) | 18.4 ns |
| `BM_Ouch50_EncodeEnterOrder` | `OrderCommand` -> OUCH 5.0 Enter Order (47 bytes, UserRefNum lookup) | 15.9 ns |

Run them with `build/<dir>/bin/bench/bench_codecs_nasdaq --cpu=N --benchmark_min_time=1s`.

## Limitations

* ITCH: Attribution (F), Cross Type (Q) and the P Buy/Sell Indicator (always `B` since 2014) have no field in the engine messages and are dropped.
* MoldUDP64: the receiver does not rotate to a new session after End of Session.
* SoupBinTCP: packet arrival time is attributed to the next `on_timer()` tick. A requested sequence number of 0 starts the server at the next new message, not at the most recently generated one.
* OUCH: the Replace Shares field is sent as the command's quantity (OUCH defines it as total liable including prior executions). Modify Order, Mass Cancel, Disable / Enable Order Entry and Account Query are laid out but not produced by the encoders.
