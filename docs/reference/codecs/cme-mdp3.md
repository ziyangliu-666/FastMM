# CME MDP 3.0 codec (`fastmm::codecs::sbe`, `fastmm::codecs::mdp3`)

!!! note "Not in the default build"

    No connector subscribes to a CME feed yet, so `fastmm::codecs` leaves this codec out unless
    you configure with `-DFASTMM_CODEC_MDP3=ON`; its tests and `bench/bench_codecs_mdp3.cpp`
    follow the same option. It is here because a CME venue is the next multicast feed after
    Nasdaq ITCH ([ADR-0015](../../adr/0015-multicast-market-data.md)) and it reuses the same
    receive path: what is missing is the `Venue` on top, not the decoding. The headers and the
    generated flyweights stay in the tree, and CI keeps checking that
    `include/fastmm/codecs/mdp3/generated/mdp3_schema.hpp` still matches the committed schema.

FastMM decodes CME Group's MDP 3.0 market data (Simple Binary Encoding over UDP) into the same normalised events as the crypto venues: `BookDeltaMsg` / `BookSnapshotMsg`, `TradeMsg` and `ConnectionStateMsg`. The codec is allocation-free and `noexcept` on the hot path.

| Piece | Where |
|---|---|
| SBE generator (Python, standard library only) | `tools/sbe_gen.py` |
| Schema subset (CME `templates_FixBinary.xml` v13) | `tools/sbe/mdp3_templates_subset.xml` |
| Generated flyweights (committed) | `include/fastmm/codecs/mdp3/generated/mdp3_schema.hpp` |
| SBE runtime (little-endian load/store, groups, decimals) | `include/fastmm/codecs/sbe/sbe.hpp` |
| Packet framing (`PacketCursor`, `MessageFramer`) | `include/fastmm/codecs/mdp3/mdp3_packet.hpp` |
| Instrument table (SecurityID -> InstrumentId) | `include/fastmm/codecs/mdp3/mdp3_instruments.hpp` |
| Decoder (`Mdp3Decoder`, a `codecs::Decoder`) | `mdp3_decoder.hpp`, `src/codecs/mdp3/mdp3_decoder.cpp` |
| Feed handler (A/B arbitration, gaps, snapshot recovery) | `mdp3_feed.hpp`, `src/codecs/mdp3/mdp3_feed.cpp` |
| Encoder for tests / simulation / benchmarks | `mdp3_encoder.hpp`, `src/codecs/mdp3/mdp3_encoder.cpp` |
| Fixtures | `tests/fixtures/cme/` |
| Tests | `tests/codecs/sbe_schema_test.cpp`, `tests/codecs/mdp3_*_test.cpp`, `tests/hotpath/codecs_mdp3_noalloc_test.cpp` |
| Benchmark | `bench/bench_codecs_mdp3.cpp` |

## Sources

CME sources, retrieved on 2026-09-14:

* **Schema.** `templates_FixBinary.xml` from `ftp://ftp.cmegroup.com/SBEFix/Production/Templates/templates_FixBinary.xml`. Its header says `package="mktdata" id="1" version="13" semanticVersion="FIX5SP2" description="20230411"`, and the file's sha256 is `f45ebadf65ccb1a2d416f00ee755480072ecff030970cc170a78540541d78d60`. Only the subset is committed, cut by `sbe_gen.py extract`; element text and attributes are unmodified.
* **CME Group Client Systems Wiki** (`cmegroupclientsite.atlassian.net`, space EPICSANDBOX):
  * "MDP 3.0 - SBE Technical Headers": MsgSeqNum uint32, SendingTime uint64, weekly sequence reset.
  * "MDP 3.0 - Packet Structure with Event Based Messaging"
  * "MDP 3.0 - SBE Decoding Example": a byte-level packet. It is committed verbatim as `tests/fixtures/cme/sbe_decoding_example_template50.hex` and shows that MsgSize counts itself.
  * "MDP 3.0 - Market by Price - Multiple Depth Book": New / Change / Delete semantics.
  * "MDP 3.0 - Incremental Refresh SBE Template Book Processing": template 46 carries MBP + MBO; MBP books have at most ten levels.
  * "MDP 3.0 - Implied Book": implied entries are a separate two-deep book.
  * "MDP 3.0 - Market Data Incremental Refresh - Trade Summary": AggressorSide, and trade MDUpdateAction New / Change (adjustment) / Delete (cancel).
  * "MDP 3.0 - Market Data Snapshot - Full Recovery" and "MDP 3.0 - MBP and MBOFD Market Recovery": LastMsgSeqNumProcessed, RptSeq, TotNumReports, the recovery workflow.
  * "MDP 3.0 - Recovery Services": process both A and B feeds; a packet gap invalidates all books.
  * "MDP 3.0 - Channel Reset"
  * "MDP 3.0 - Market Data Security Definition - Futures": MinPriceIncrement, DisplayFactor, UnitOfMeasureQty, MarketDepth.

## Templates

| Template | Id | blockLength | Use |
|---|---|---|---|
| ChannelReset4 | 4 | 9 | empty every book on the channel |
| AdminHeartbeat12 | 12 | 0 | counted |
| SecurityStatus30 | 30 | 30 | trading status stored per instrument |
| MDIncrementalRefreshBook46 | 46 | 11 (entries 32, MBO group 24) | MBP book entries -> `BookDeltaMsg` |
| MDIncrementalRefreshTradeSummary48 | 48 | 11 (entries 32) | New trades -> `TradeMsg` |
| SnapshotFullRefresh52 | 52 | 59 (entries 22) | recovery snapshots |
| MDInstrumentDefinitionFuture54 | 54 | 224 | instrument table |
| MDIncrementalRefreshVolume37, DailyStatistics49, LimitsBanding50, SessionStatistics51 | 37, 49, 50, 51 | 11 | RptSeq accounting only |

RptSeq is sequenced per instrument across all incremental templates, so 37, 49, 50 and 51 are read for RptSeq accounting. Any template not in the subset is skipped by MsgSize, and so is any other schemaId.

## Generator

```sh
# cut the subset out of CME's full schema (done once; the subset is committed)
python3 tools/sbe_gen.py extract --schema templates_FixBinary.xml \
    --templates 4,12,30,37,46,48,49,50,51,52,54 --out tools/sbe/mdp3_templates_subset.xml
# regenerate the flyweights (the output is committed)
python3 tools/sbe_gen.py generate --schema tools/sbe/mdp3_templates_subset.xml \
    --schema-label tools/sbe/mdp3_templates_subset.xml \
    --out include/fastmm/codecs/mdp3/generated/mdp3_schema.hpp \
    --namespace fastmm::codecs::mdp3::schema
# the same with --check exits 1 when the committed header is stale
```

The generated header has one reader class and one `...Writer` class per message:

* **Reads.** Every field is read with `memcpy` at its schema offset, little-endian.
* **Layout checks.** The generator validates offsets and sizes, then emits a `static_assert` for every field (`offset + size <= blockLength`) and for every block length.
* **Groups.** `sbe::GroupView<Entry, Dimension>` reads `groupSize` (3 bytes) and `groupSize8Byte` (8 bytes, numInGroup at offset 7) headers. The whole group is bounds-checked once. Entries use the *acting* block length as the stride, so blocks extended by a later schema version still decode.
* **Schema evolution.** A field whose `sinceVersion` is newer than its block reads as null when the acting version is older or the acting block is too short. `TradeableSize` (since v10) is the example tested.
* **Optional values.** Optional primitives get `has_x()` and a `kXNull` constant. Enums get a `NullValue` enumerator when their encoding is optional, plus `is_valid()` and `to_string()`. Constant fields are static accessors and occupy no bytes on the wire.
* **Decimals.** `PRICE9`, `PRICENULL9`, `Decimal9`, `Decimal9NULL` and `DecimalQty` map to `sbe::Decimal<M, exponent, optional, null>`. `to_fixed()` converts to FastMM's 1e-8 fixed point exactly. It returns `false` for null, for values that would need more than 8 decimals (a 1e-9 mantissa not divisible by 10) and on overflow. `from_fixed()` is the exact inverse.

Unsupported, and rejected by the generator: `<data>` (variable-length) fields, nested groups, non-constant exponents, array members in composites, big-endian schemas (none is used by the subset).

## Decoding

`Mdp3Decoder::decode_packet(datagram, rx_ts, sink)` walks the messages of one UDP datagram:

* **MBP entries** (template 46, MDEntryType `0` bid / `1` offer) update a per-instrument book of at most MarketDepth levels (tag 264 of the GBX feed type, up to 10). Levels are addressed by MDPriceLevel (1 = best):
  * New inserts and shifts worse levels down; the level past MarketDepth falls off.
  * Change sets the quantity at an unchanged price.
  * Delete removes the level and shifts worse levels up.
  * DeleteThru removes levels 1..N. DeleteFrom removes level N and everything worse. Overlay replaces the price and quantity at the level. These three follow the FIX tag 279 definitions, see *Unverified* below.
* **Events.** Every change becomes a price-keyed `Level` (qty 0 = delete), including the level that falls off the bottom. All changes of one instrument within one message become a single `BookDeltaMsg`, built in place in the sink with `BookDeltaMsg::size_for()`. Its fields: `first_update_id`/`last_update_id` hold the RptSeq range, `exch_ts` is TransactTime and `recv_ts` is `rx_ts`. An `L2Book` fed with these events holds the decoder's top N.
* **Implied entries** (`E`/`F`) are counted and ignored; the implied book is not merged. `J` (book reset) empties the instrument. `w`/`x` are counted and ignored (EBS only).
* **Trades.** Trade summary entries with MDUpdateAction New become `TradeMsg`:
  * Price is exact at 1e-8; qty is in contracts.
  * `trade_id` is MDTradeEntryID, or RptSeq when that is null.
  * AggressorSide 1/2 maps to `Side::Buy`/`Side::Sell`. AggressorSide 0 (no aggressor) gives `Side::Buy` plus `pad_[0] = kTradeNoAggressor`, because the core `TradeMsg` has no "none" side.
  * Change (adjustment) and Delete (bust) are counted only.
* **Instrument definitions** fill `Mdp3InstrumentTable`. It has fixed capacity (1024), maps SecurityID to InstrumentId through an `OpenHashMap` allocated at construction, and records symbol, group, asset, MinPriceIncrement, DisplayFactor, UnitOfMeasureQty, ContractMultiplier, MarketDepth, implied depth and ApplID. Prices stay in Globex display units, as on the wire. `to_instrument()` builds an engine `Instrument`.
* **ChannelReset4** empties every book (one empty `BookSnapshotMsg` each) and resets RptSeq to 0, because MBP RptSeq restarts at 1 after a reset.

### What puts an instrument into recovery

RptSeq is checked per instrument across templates 46/48/37/49/50/51. The instrument's entries are skipped until a snapshot when any of these happen:

* an RptSeq gap;
* an entry that contradicts the book (level out of range, crossed neighbours, a Change whose price differs, a Delete of a missing level);
* a price that is not representable at 1e-8;
* a full sink.

An RptSeq at or below the last applied one is a stale replay and is skipped silently.

## Feed handling (`Mdp3Feed`)

* **A/B arbitration.**
  * The first copy of a MsgSeqNum is processed and later copies are dropped.
  * A packet ahead of sequence is held, up to `reorder_window` slots (default 64), while the other line may still deliver the missing one.
  * The hole is declared lost when a packet beyond the window arrives, or when the oldest held packet is older than `gap_timeout_ns` (default 2 ms, via `rx_ts` or `on_timer`).
* **Start.** MsgSeqNum resets weekly. A first packet with MsgSeqNum 1 starts Live; any other first packet is a late join.
* **Resyncing.**
  * A packet gap, or a late join, marks every instrument as recovering. An RptSeq gap marks only that instrument.
  * The feed emits `ConnectionStateMsg{Resyncing, channel 0}`. `reason_code` is a `ResyncReason`: LateJoin, PacketGap, RptSeqGap, BookError or SinkOverflow.
  * Incremental packets keep being processed for the instruments that are not recovering, and are copied into a recovery buffer (`recovery_packets`, default 8192).
* **Snapshot recovery.** For each `SnapshotFullRefresh52` of a recovering instrument:
  1. The buffer must cover every packet after LastMsgSeqNumProcessed (tag 369). Otherwise the next loop iteration is awaited.
  2. The buffered entries of that instrument with RptSeq greater than the snapshot's RptSeq (tag
     83) must be contiguous. Otherwise the next loop iteration is awaited.
  3. The book is replaced (`BookSnapshotMsg`, `kSnapshot`) and the buffered entries are replayed (`BookDeltaMsg`, `TradeMsg`).

  This is CME's recovery rule applied per entry by RptSeq rather than per packet by LastMsgSeqNumProcessed and TransactTime; it also covers events split across packets.
* **Instruments without book activity** have no snapshot. After a complete loop (snapshot packet 1 through TotNumReports messages), instruments that were recovering for the whole loop and never appeared are reset to an empty book, and their next RptSeq is taken as the baseline.
* **Back to Live.** When nothing is recovering, the buffer is dropped and `ConnectionStateMsg{Live}` is emitted. A ChannelReset also ends recovery.

## Simulation validation

`tests/codecs/mdp3_sim_test.cpp` drives seeded random order flow (limit, IOC and cancels over three instruments with MarketDepth 10, 5 and 3) into `sim::MatchingEngine`. After every order, `MbpPublisher` turns the engine's top N into New/Change/Delete entries, which are sent after the trade summary as packets on lines A and B. A snapshot loop is published on demand. The receiver's decoder books and the engine-side `L2Book`s built from its events must equal the matching engine's top N:

| Test | Seeds x events | Network |
|---|---|---|
| books equal after every event | 16 x 1500 | line A only |
| loss on A covered by B | 8 x 1500 | 30 % loss on A, random A/B order |
| loss on both lines | 8 x 2500 | bursts of 1-6 packets lost on both lines, snapshot loops every 20 events while resyncing; books compared whenever caught up, and fully at the end |
| duplicates and reordering | 8 x 1500 | A and B copies, neighbour swaps, late duplicates of old packets |

## Benchmarks

`bench/bench_codecs_mdp3.cpp`, from [bench/README.md](../../../bench/README.md) (`scripts/bench.sh`, preset `release-native`, gcc 13, WSL2 on a shared 8-core desktop, 2026-09-23; see [Benchmarks](../../explanation/benchmarks.md#caveats)):

| Benchmark | p50 | What is timed |
|---|---|---|
| `BM_Mdp3_DecodeBook46_4Entries` | 39.5 ns / packet | `Mdp3Decoder::decode_packet` of a Book46 packet with 4 Change entries (2 bid, 2 offer levels) into a `MsgRing` sink, one `BookDeltaMsg`, plus draining the ring |
| `BM_Mdp3_ArbitrationAB_Heartbeat` | 9.7 ns / packet copy | a heartbeat packet on line A (processed) then line B (dropped) |
| `BM_Mdp3_FeedAB_Book46_4Entries` | 44.5 ns / A+B pair | the 4-entry Book46 packet through `Mdp3Feed` on A, its duplicate on B, ring drain |

Run: `bench_codecs_mdp3 --cpu=5 --benchmark_repetitions=7 --benchmark_min_time=0.3s`.

## Unverified or simplified

* **DeleteThru, DeleteFrom, Overlay.** They are defined in the schema's MDUpdateAction enum, but the current wiki MBP pages only describe New/Change/Delete, and the LongQty page lists 0, 1, 2, 5 for EBS. FastMM implements the FIX 5.0 SP2 semantics (see [Decoding](#decoding)).
* **Maximum datagram size.** CME's pages above do not state one. `kMaxPacketBytes` = 1500 bounds the buffered copies; larger datagrams are still decoded but end the buffer's coverage.
* **Gap timeout** is a site-specific A/B skew budget, not a CME value.
* **Char padding.** Char arrays are NUL-trimmed; CME's padding character was not confirmed.
* **Not handled:**
  * MBO entries (the NoOrderIDEntries group of template 46, and template 47).
  * The implied book.
  * Statistics.
  * Options, spreads and the other instrument definition templates.
  * The LongQty templates 64-69. On channels that use them, RptSeq accounting would see gaps.
  * TCP replay.
  * Instrument recovery after a packet gap (new definitions must come from the instrument feed).
* **Channel scope.** ChannelReset applies to every instrument in the decoder; ApplID is not used to scope it. Run one decoder per channel.
* **Fixtures.** The only real CME bytes are the wiki's decoding example. The other fixtures are packed from the schema offsets by `tests/fixtures/cme/make_fixtures.py`, independently of the generated writers, and not captured from a production feed.
