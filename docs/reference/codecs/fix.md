# FIX 4.4 codec (`fastmm::codecs::fix`)

A tag=value FIX 4.4 codec: framing, session layer for initiators and acceptors, and a decoder and encoder between FIX messages and the engine's normalised messages. There is no XML dictionary: the tags and enumerations the codec uses are constants in `include/fastmm/codecs/fix/fix_tags.hpp`.

Everything on the hot path is `noexcept` and allocation-free after construction (`tests/hotpath/codecs_fix_noalloc_test.cpp` checks it). The library depends on core only.

```
bytes ─► FixFramer ─► FixSession.on_frame ─┬─ session message: handled, true
                                          └─ application: false ─► FixDecoder ─► EventSink
OrderCommand ─► FixEncoder.encode ─► FixSession.send_app ─► MessageStore + SendFn
```

| header | type | concept |
|---|---|---|
| `fix_framer.hpp` | `FixFramer` | `codecs::Framer` |
| `fix_view.hpp` | `FixView`, `FixRange`, `FixGroupReader` | |
| `fix_builder.hpp` | `FixBuilder` | |
| `fix_session.hpp` | `FixSession`, `FixSessionConfig` | `codecs::SessionLayer` |
| `message_store.hpp` | `MessageStore` | |
| `fix_decoder.hpp` | `FixDecoder` | `codecs::Decoder` |
| `fix_encoder.hpp` | `FixEncoder` | `codecs::Encoder` |
| `fix_symbols.hpp` | `FixSymbolTable` | |
| `fix_time.hpp` | `parse_utc_timestamp`, `format_utc_timestamp` | |
| `fix.hpp` | umbrella + concept `static_assert`s | |

## Wire format

`8=FIX.4.4|9=<BodyLength>|35=<MsgType>|...|10=<CheckSum>|` where `|` is SOH (0x01).

* BeginString(8), BodyLength(9) and MsgType(35) are the first three fields.
* BodyLength counts the bytes after the SOH that ends field 9, up to and excluding `10=`.
* CheckSum is the sum of every byte before `10=`, modulo 256, written as three digits.
* Values cannot be empty. The data fields SecureData(91), RawData(96), XmlData(213) and EncodedText(355) may contain SOH; their length comes from the preceding length field.

### FixFramer

`FixFramer` finds `8=FIX`, reads BodyLength and checks that `10=ddd|` sits exactly where BodyLength says. It returns `kMessage` frames (payload = one message) or `kGarbage` frames (bytes that cannot start a message), and "need more" on partial input. Garbage before a message, a non-numeric BodyLength, a misplaced CheckSum field or a message over `max_message` (64 KiB by default) are skipped until the next `8=FIX`.

### FixView

`FixView::parse` validates the framing above plus the CheckSum value, then indexes every field as (tag, offset, length) into the caller's bytes. Tags below 1024 get O(1) lookups. Getters return `std::optional`: `get_int`, `get_char`, `get_bool` (Y/N), `get_price` / `get_qty` (exact decimal to the 1e-8 fixed point; `+` and exponents rejected), `get_timestamp_ns`. `FixGroupReader` walks a repeating group: entries start with the group's first tag, end at the next first tag, at a tag outside an optional member list, or at CheckSum, and `complete()` checks NumInGroup.

### UTCTimestamp

`YYYYMMDD-HH:MM:SS` or `YYYYMMDD-HH:MM:SS.sss` in FIX 4.4. The parser also accepts 6 and 9 fraction digits; the builder writes milliseconds.

### FixBuilder

`FixBuilder` writes into a caller buffer: `begin_header(type, sender, target, seq, now)` emits 8, 9 (three placeholder digits), 35, 49, 56, 34, 43 + 122 for retransmissions, and 52. `finish()` writes the real BodyLength (shifting the body when it is not three digits) and appends CheckSum. Overflow latches `!ok()` and `finish()` returns 0.

## Session layer

`FixSession(cfg, send, ctx)`; `set_clock()` supplies SendingTime (otherwise the time of the last `on_timer`), `set_event_callback()` reports logon, disconnect, gap detected and gap filled. `SendFn` must not call back into the session.

| message | behaviour |
|---|---|
| Logon (A) | Initiator `logon(now)` sends EncryptMethod(98)=0, HeartBtInt(108), ResetSeqNumFlag(141)=Y when `reset_seq_num_on_logon` (both sequence numbers restart at 1, the store is cleared). The acceptor adopts HeartBtInt, resets on 141=Y, answers with its own Logon. A Logon without a valid HeartBtInt is refused; `logon_timeout_ns` bounds the wait. |
| Heartbeat (0) | Sent when nothing was sent for HeartBtInt; echoes TestReqID(112). |
| TestRequest (1) | Sent when nothing was received for HeartBtInt + `transmission_grace_pct` (20%); disconnect (`TestRequestTimeout`) when that passes again without any message. Received ones are answered with Heartbeat(112). |
| ResendRequest (2) | Sent on a gap with BeginSeqNo(7) = expected, EndSeqNo(16) = 0 (infinity). Received ones are served from the store (below). |
| Reject (3) | Sent with RefSeqNum(45), RefTagID(371), RefMsgType(372), SessionRejectReason(373), Text(58). |
| SequenceReset (4) | GapFill mode (123=Y) follows the MsgSeqNum rules and moves the expected number to NewSeqNo(36); NewSeqNo not above MsgSeqNum is rejected (373=5). Reset mode ignores MsgSeqNum; a NewSeqNo below the expected number is rejected (373=5). |
| Logout (5) | `logout(text)` sends Logout and waits `logout_timeout_ns` for the confirmation; a received Logout is confirmed and the session goes down. |

Inbound MsgSeqNum(34):

* equal to the expected number: processed;
* higher: ResendRequest, state `Recovering`, the message is discarded (EndSeqNo=0 covers it). Recovery ends when the expected number passes the highest number seen. If, while recovering, a newer message arrives after the resend has already advanced the expected number, a new gap opened (a message lost mid-recovery) and the session asks again from there;
* lower with PossDupFlag(43)=Y: duplicate, ignored;
* lower without it: Logout with `MsgSeqNum too low, expecting N but received M`, disconnect.

PossDupFlag=Y without OrigSendingTime(122) gets Reject 373=1 (RefTagID 122). OrigSendingTime later than SendingTime gets Reject 373=10, then Logout and disconnect. Messages failing BodyLength or CheckSum are ignored (not counted against sequence numbers). A wrong BeginString after logon, a CompID mismatch (Reject 373=9 + Logout) or a missing MsgSeqNum end the session. Acceptors drop anything but Logon while down.

### MessageStore

`send_app()` appends each outbound application message (MsgType, original SendingTime, body bytes after the standard header) to a store preallocated at construction (`store_max_messages` entries, `store_max_bytes` arena). It is append-only and bounded: once full, further messages are not kept. On ResendRequest each stored message in the range is sent again with its original MsgSeqNum, PossDupFlag=Y, OrigSendingTime = original SendingTime and a new SendingTime; every run of administrative or unavailable sequence numbers becomes one SequenceReset-GapFill (PossDupFlag=Y, NewSeqNo = next number to resend). Header fields other than 8/9/35/49/56/34/43/52/122 are not preserved on resend.

## Decoder

`FixDecoder(symbols, venue)`; `decode(frame, rx_ts, sink)` parses and validates, `decode_view(view, rx_ts, sink)` reuses `FixSession::view()`. Messages are built in place in the sink (BookDelta is reserved with `BookDeltaMsg::size_for` and filled in place). `hdr.venue_seq` = MsgSeqNum, `exch_ts` = TransactTime(60) or SendingTime(52), `recv_ts` = `rx_ts`.

| FIX | engine message |
|---|---|
| ExecutionReport ExecType(150) 0 New, 5 Replaced | `OrderAck` (ClOrdID 11, OrderID 37) |
| 150=8 Rejected | `OrderReject` (OrdRejReason 103: 1 InstrumentDisabled, 5 VenueUnknownOrder, 6 DuplicateId, 13 InvalidLot, else VenueReject; Text 58) |
| 150=4 Canceled | `OrderCancelAck` for OrigClOrdID(41), else ClOrdID; CumQty(14) |
| 150=C Expired, 3 Done for day | `OrderExpired` (CumQty 14) |
| 150=F Trade (and pre-4.3 1/2) | `OrderFill` (LastQty 32, LastPx 31, CumQty 14, LeavesQty 151, ExecID 17, Side 54, LastLiquidityInd 851: 1 Maker, 2 Taker) |
| 150=6, A, E, D, I, G, H | ignored |
| OrderCancelReject (9), CxlRejResponseTo(434)=1 | `OrderCancelReject` for OrigClOrdID (CxlRejReason 102=1 VenueUnknownOrder, else VenueReject) |
| OrderCancelReject, 434=2 | `OrderReject` for the replacement ClOrdID |
| MarketDataSnapshotFullRefresh (W) | one `BookSnapshot` (kSnapshot) with MDEntryType 0 bids and 1 offers |
| MarketDataIncrementalRefresh (X) | one `BookDelta` per run of book entries on one instrument (MDUpdateAction 2 Delete = qty 0); one `Trade` per MDEntryType 2 entry (trade id = numeric MDEntryID 278) |

Fills are deduplicated by ExecID over the last 256 executions. ClOrdIDs are the engine's encoding (`encode_cl_ord_id`, "fm" + 12 hex digits); order events for other ids are ignored (`foreign_ids`). Order events are delivered even when Symbol(55) is unknown (instrument left invalid); market data needs a known symbol. In X, an entry without Symbol inherits the previous entry's.

## Encoder

`FixEncoder(session, symbols)`; `encode(cmd, out)` writes a full message for MsgSeqNum = `session.next_sender_seq()`, `send(session, cmd)` encodes and calls `send_app`.

| command | message and fields |
|---|---|
| New | NewOrderSingle (D): 11 ClOrdID, 18 ExecInst=6 for post-only, 55 Symbol, 54 Side, 60 TransactTime, 38 OrderQty, 40 OrdType (1 Market, 2 Limit and post-only), 44 Price (not for Market), 59 TimeInForce (GTC 1, IOC 3, FOK 4, Day 0) |
| Cancel | OrderCancelRequest (F): 41 OrigClOrdID, 37 OrderID when known, 11 = order id + `c` + counter, 55, 54, 60, 38 |
| Replace | OrderCancelReplaceRequest (G): 37, 41, 11, 18, 55, 54, 60, 38, 40, 44, 59 |

F and G require Side, OrderQty and OrdType, which `OutCancelMsg` / `OutReplaceMsg` do not carry. The encoder remembers side, type, time in force, instrument, price and quantity of every order it encoded in a preallocated direct-mapped table keyed by the client order id (16384 slots by default); `remember()` seeds orders sent elsewhere. A cancel or replace for an unknown order fails (returns 0). `reduce_only` has no FIX 4.4 field and is not sent.

## Simulation validation

`tests/codecs/fix_sim_test.cpp` joins an initiator and an acceptor session with an in-memory byte pipe (random 1 to 64 byte chunks through `FixFramer`). The acceptor bridges D/F/G to the sim `MatchingEngine` and answers with ExecutionReports, OrderCancelRejects and MarketDataIncrementalRefresh built from the engine's callbacks. For three seeds, 3000 random steps of new orders (limit, post-only, IOC), cancels (including unknown ids), replaces (in-place amends and cancel-then-new) and counter-party liquidity run while about 2% of the venue's first transmissions are dropped, so recovery by ResendRequest runs throughout. At the end the client, which only sees decoded events, must match the engine exactly: fills per order and the net position, open orders with their leaves, and both sides of the book.

`tests/codecs/fix_session_test.cpp` covers logon, heartbeats, test requests and their timeout, logout, gap detection with resend (PossDup, OrigSendingTime, GapFill for administrative messages), a full store, too-low MsgSeqNum, the PossDup rules, both SequenceReset modes, garbled messages and CompID checks.

## Performance

`bench/bench_codecs_fix.cpp`, from [bench/README.md](../../../bench/README.md) (`scripts/bench.sh`, preset `release-native`, gcc 13, an 8-core desktop under WSL2, pinned, median of 5 repetitions, 2026-09-23):

| benchmark | p50 |
|---|---|
| `BM_Fix_ParseExecReportFill` (validate + index a 25-field, ~250-byte fill, one getter) | 221 ns |
| `BM_Fix_DecodeExecReportFill` (parse + `OrderFillMsg` into a MsgRing, ExecID dedup) | 404 ns |
| `BM_Fix_BuildNewOrderSingle` (`FixEncoder::encode`, post-only limit, 9 body fields) | 122 ns |
| `BM_Fix_FramerNext` (one complete message) | 8.3 ns |

```bash
./build/<dir>/bin/bench/bench_codecs_fix --cpu=5 --benchmark_repetitions=9 \
  --benchmark_report_aggregates_only=true --benchmark_min_time=0.5s
```

Budgets: `bench/ci_budget.toml`.

## Sources

* FIX Trading Community, FIX 4.4 specification with Errata 20030618:
  <https://www.fixtrading.org/standards/fix-4-4/>
* OnixS FIX 4.4 dictionary, <https://www.onixs.biz/fix-dictionary/4.4/>: StandardHeader (`compBlock_StandardHeader.html`), messages Logon (`msgType_A_65.html`), Heartbeat (`msgType_0_0.html`), TestRequest (`msgType_1_1.html`), ResendRequest (`msgType_2_2.html`), Reject (`msgType_3_3.html`), SequenceReset (`msgType_4_4.html`), Logout (`msgType_5_5.html`), ExecutionReport (`msgType_8_8.html`), OrderCancelReject (`msgType_9_9.html`), NewOrderSingle (`msgType_D_68.html`), OrderCancelRequest (`msgType_F_70.html`), OrderCancelReplaceRequest (`msgType_G_71.html`), MarketDataSnapshotFullRefresh (`msgType_W_87.html`), MarketDataIncrementalRefresh (`msgType_X_88.html`); tags 9, 10, 18, 39, 40, 54, 59, 97, 102, 103, 123, 150, 269, 279, 373, 434, 851 (`tagNum_<n>.html`).
* OnixS, CheckSum calculation (FIX 4.2 appendix B; the algorithm is unchanged in 4.4):
  <https://www.onixs.biz/fix-dictionary/4.2/app_b.html>
* B2BITS FIXopaedia FIX 4.4 data types (UTCTimestamp, int, float, Boolean, MultipleValueString):
  <https://www.b2bits.com/fixopaedia/fixdic44/data_types.html>
* FIX Trading Community, FIX session-level test cases and expected behaviours (MsgSeqNum too low, PossDup handling, OrigSendingTime later than SendingTime), from secondary summaries only.
* Known-answer check: the ExecutionReport example of the Wikipedia article "Financial Information eXchange" (BodyLength 178, CheckSum 128).

## Limitations and unverified details

* Session rules come from the dictionary's message descriptions and secondary summaries of the session test cases. Not confirmed against FIX 4.4 Volume 2 (session protocol):
  * GapFill messages sent in reply to a ResendRequest carry PossDupFlag=Y (implemented);
  * the 20% "reasonable transmission time" (a configurable default);
  * a ResendRequest or Logout with a too-high MsgSeqNum is still acted on (implemented: serve or confirm, then request the gap);
  * an acceptor silently drops non-Logon first messages.
* Session: no persistent sequence numbers or store (in memory, per process); no NextExpectedMsgSeqNum (789), Username/Password, encryption or Logon-time TestRequest; a lost ResendRequest is not retried until another too-high message arrives; messages arriving above the gap are discarded, not queued; `Reject` is gap-filled like other administrative messages.
* Decoder: FIX 4.4 market data has no aggressor side, so `TradeMsg::aggressor` is always Buy; incremental entries must carry MDEntryType and MDEntryPx (entries addressed only by MDEntryID are not supported); trades in W snapshots are ignored; Commission(12) is not mapped, so `fee` is 0; pending, restated, order-status and trade-correction reports are ignored.
* Encoder: Account(1) and HandlInst(21) are not sent.
* The simulation bridge sends AvgPx(6) = LastPx (or 0) rather than a running average.
