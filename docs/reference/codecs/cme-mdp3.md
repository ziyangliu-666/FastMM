# CME MDP 3.0 codec (`fastmm::codecs::sbe`, `fastmm::codecs::mdp3`)

!!! note "Not in the default build"

    No connector subscribes to a CME feed yet. The codec builds only with `-DFASTMM_CODEC_MDP3=ON`, and its tests and benchmark follow the same option. A CME venue would reuse the Nasdaq ITCH multicast receive path ([ADR-0015](../../adr/0015-multicast-market-data.md)).

The codec decodes CME Group MDP 3.0 market data, Simple Binary Encoding over UDP, into `BookDeltaMsg`, `BookSnapshotMsg`, `TradeMsg` and `ConnectionStateMsg`. Its hot path is `noexcept` and allocation-free.

| Piece | Where |
|---|---|
| SBE generator, Python standard library only | `tools/sbe_gen.py` |
| Schema subset of CME `templates_FixBinary.xml` v13 | `tools/sbe/mdp3_templates_subset.xml` |
| Generated flyweights, committed; CI checks they are current | `include/fastmm/codecs/mdp3/generated/mdp3_schema.hpp` |
| SBE runtime: little-endian load and store, groups, decimals | `include/fastmm/codecs/sbe/sbe.hpp` |
| Packet framing (`PacketCursor`, `MessageFramer`) | `include/fastmm/codecs/mdp3/mdp3_packet.hpp` |
| Instrument table, SecurityID to InstrumentId | `include/fastmm/codecs/mdp3/mdp3_instruments.hpp` |
| Decoder (`Mdp3Decoder`, a `codecs::Decoder`) | `mdp3_decoder.hpp`, `src/codecs/mdp3/mdp3_decoder.cpp` |
| Feed handler: A/B arbitration, gaps, snapshot recovery | `mdp3_feed.hpp`, `src/codecs/mdp3/mdp3_feed.cpp` |
| Encoder for tests, simulation and benchmarks | `mdp3_encoder.hpp`, `src/codecs/mdp3/mdp3_encoder.cpp` |
| Fixtures | `tests/fixtures/cme/` |

## Templates

| Template | Id | blockLength | Use |
|---|---|---|---|
| ChannelReset4 | 4 | 9 | empties every book on the channel |
| AdminHeartbeat12 | 12 | 0 | counted |
| SecurityStatus30 | 30 | 30 | trading status stored per instrument |
| MDIncrementalRefreshBook46 | 46 | 11 (entries 32, MBO group 24) | MBP book entries to `BookDeltaMsg` |
| MDIncrementalRefreshTradeSummary48 | 48 | 11 (entries 32) | New trades to `TradeMsg` |
| SnapshotFullRefresh52 | 52 | 59 (entries 22) | recovery snapshots |
| MDInstrumentDefinitionFuture54 | 54 | 224 | instrument table |
| MDIncrementalRefreshVolume37, DailyStatistics49, LimitsBanding50, SessionStatistics51 | 37, 49, 50, 51 | 11 | RptSeq accounting only |

RptSeq runs per instrument across every incremental template, so 37, 49, 50 and 51 are read for it. Other templates and other schemaIds are skipped by MsgSize.

## Generator

```bash
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

The header has a reader class and a `...Writer` class per message.

| Feature | Generated code |
|---|---|
| Fields | read with `memcpy` at the schema offset, little-endian |
| Layout checks | a `static_assert` per field (`offset + size <= blockLength`) and per block length |
| Groups | `sbe::GroupView<Entry, Dimension>` reads `groupSize` (3 bytes) and `groupSize8Byte` (8 bytes) headers, bounds-checked once; the stride is the acting block length, so extended blocks decode |
| Schema evolution | a field newer than the acting version, or past a short block, reads as null (tested on `TradeableSize`, since v10) |
| Optional values | `has_x()` and `kXNull`; optional enums get `NullValue`, `is_valid()` and `to_string()` |
| Constants | static accessors, no bytes on the wire |
| Decimals | `PRICE9`, `PRICENULL9`, `Decimal9`, `Decimal9NULL`, `DecimalQty` map to `sbe::Decimal<M, exponent, optional, null>`; `to_fixed()` converts exactly to 1e-8 and returns `false` for null, finer values or overflow; `from_fixed()` is its inverse |

The generator rejects `<data>` fields, nested groups, non-constant exponents, arrays in composites and big-endian schemas.

## Decoding

`Mdp3Decoder::decode_packet(datagram, rx_ts, sink)` walks the messages of one UDP datagram.

MBP entries (template 46, MDEntryType `0` bid, `1` offer) update a book of at most MarketDepth levels (tag 264, up to 10), addressed by MDPriceLevel (1 = best):

| MDUpdateAction | Effect |
|---|---|
| New | inserts, shifts worse levels down; the level past MarketDepth falls off |
| Change | sets the quantity at an unchanged price |
| Delete | removes the level, shifts worse levels up |
| DeleteThru | removes levels 1 to N |
| DeleteFrom | removes level N and everything worse |
| Overlay | replaces price and quantity at the level |

DeleteThru, DeleteFrom and Overlay follow the FIX tag 279 definitions; CME's wiki describes only the first three ([unverified](#unverified-or-simplified)).

| Output | Rule |
|---|---|
| Book changes | each change is a price-keyed `Level` (qty 0 deletes), including a level that falls off; one `BookDeltaMsg` per instrument per message |
| Delta fields | `first_update_id` and `last_update_id` hold the RptSeq range; `exch_ts` is TransactTime; `recv_ts` is `rx_ts` |
| Implied entries `E`, `F` | counted, ignored; the implied book is not merged |
| `J` | empties the instrument's book |
| `w`, `x` (EBS) | counted, ignored |
| Trade summary, New | `TradeMsg`, price exact at 1e-8, qty in contracts, `trade_id` = MDTradeEntryID or RptSeq |
| AggressorSide 1 / 2 / 0 | `Side::Buy` / `Side::Sell` / `Side::Buy` with `pad_[0] = kTradeNoAggressor` |
| Trade Change (adjustment), Delete (bust) | counted only |
| ChannelReset4 | an empty `BookSnapshotMsg` per book; RptSeq reset to 0 |

An `L2Book` fed with these events holds the decoder's top N.

`Mdp3InstrumentTable` holds up to 1024 instruments, keyed by SecurityID, allocated at construction. It records symbol, group, asset, MinPriceIncrement, DisplayFactor, UnitOfMeasureQty, ContractMultiplier, MarketDepth, implied depth and ApplID. Prices stay in Globex display units; `to_instrument()` builds an engine `Instrument`.

### What puts an instrument into recovery

RptSeq is checked per instrument across templates 46, 48, 37, 49, 50 and 51. The instrument's entries are skipped until a snapshot after:

* an RptSeq gap;
* an entry that contradicts the book: level out of range, crossed neighbours, a Change at another price, a Delete of a missing level;
* a price not representable at 1e-8;
* a full sink.

An RptSeq at or below the last applied one is a stale replay, skipped silently.

## Feed handling (`Mdp3Feed`)

| Behaviour | Rule |
|---|---|
| Arbitration | the first copy of a MsgSeqNum is processed, later copies dropped |
| Reorder | a packet ahead of sequence is held, up to `reorder_window` slots (64) |
| Lost | a packet past the window arrives, or the oldest held packet is older than `gap_timeout_ns` (2 ms) |
| Start | MsgSeqNum resets weekly; a first packet with MsgSeqNum 1 starts Live, any other is a late join |
| Packet gap or late join | every instrument recovers |
| RptSeq gap | only that instrument recovers |
| While recovering | emits `ConnectionStateMsg{Resyncing, channel 0}` with a `ResyncReason` (LateJoin, PacketGap, RptSeqGap, BookError, SinkOverflow); healthy instruments keep updating; packets are copied into a buffer of `recovery_packets` (8192) |

A `SnapshotFullRefresh52` recovers an instrument when both hold:

1. The buffer covers every packet after the snapshot's `LastMsgSeqNumProcessed`.
2. The buffered entries after the snapshot's `RptSeq` are contiguous.

The book is then replaced and the buffered entries replayed; otherwise the instrument waits for the next snapshot loop.

Recovery applies CME's rule per entry by RptSeq, not per packet, so events split across packets recover too.

* Instruments without book activity have no snapshot. After a full loop (snapshot packet 1 through TotNumReports), an instrument absent for the whole loop resets to an empty book, and its next RptSeq is the baseline.
* With nothing recovering, the buffer is dropped and `ConnectionStateMsg{Live}` emitted. A ChannelReset also ends recovery.

## Performance

Decoding a four-entry book packet takes about 43 ns, and arbitrating its A and B copies about 49 ns; the table is in [bench/full-results.md](../../../bench/full-results.md) ([method](../../explanation/benchmarks.md)).

## Unverified or simplified

| Item | State |
|---|---|
| DeleteThru, DeleteFrom, Overlay | in the schema's enum but not on CME's MBP wiki pages; FastMM implements FIX 5.0 SP2 semantics |
| Datagram size | not stated by CME; `kMaxPacketBytes` = 1500 bounds buffered copies; larger datagrams decode but end the buffer's coverage |
| Gap timeout | a site-specific A/B skew budget, not a CME value |
| Char padding | NUL-trimmed; CME's padding character unconfirmed |
| ChannelReset scope | every instrument in the decoder, not by ApplID; run one decoder per channel |
| Fixtures | only the wiki's decoding example is real CME bytes; the rest are packed from schema offsets by `tests/fixtures/cme/make_fixtures.py` |

Not handled: MBO entries (template 46's NoOrderIDEntries, template 47), the implied book, statistics, options, spreads and other definition templates, the LongQty templates 64 to 69 (RptSeq would see gaps), TCP replay, and instrument recovery after a packet gap.

## Sources

Retrieved 2026-09-14:

* Schema `templates_FixBinary.xml` from `ftp://ftp.cmegroup.com/SBEFix/Production/Templates/`: version 13, `description="20230411"`, sha256 `f45ebadf65ccb1a2d416f00ee755480072ecff030970cc170a78540541d78d60`. The committed subset keeps element text and attributes unmodified.
* CME Group Client Systems Wiki (`cmegroupclientsite.atlassian.net`, space EPICSANDBOX):

| Page | Gives |
|---|---|
| MDP 3.0 - SBE Technical Headers | MsgSeqNum uint32, SendingTime uint64, weekly sequence reset |
| MDP 3.0 - Packet Structure with Event Based Messaging | packet layout |
| MDP 3.0 - SBE Decoding Example | a byte-level packet, committed as `tests/fixtures/cme/sbe_decoding_example_template50.hex`; MsgSize counts itself |
| MDP 3.0 - Market by Price - Multiple Depth Book | New, Change, Delete |
| MDP 3.0 - Incremental Refresh SBE Template Book Processing | template 46 carries MBP and MBO; MBP books have at most ten levels |
| MDP 3.0 - Implied Book | implied entries are a separate two-deep book |
| MDP 3.0 - Market Data Incremental Refresh - Trade Summary | AggressorSide; trade New, Change (adjustment), Delete (cancel) |
| MDP 3.0 - Market Data Snapshot - Full Recovery; MBP and MBOFD Market Recovery | LastMsgSeqNumProcessed, RptSeq, TotNumReports, the recovery workflow |
| MDP 3.0 - Recovery Services | process both A and B; a packet gap invalidates all books |
| MDP 3.0 - Channel Reset | channel reset |
| MDP 3.0 - Market Data Security Definition - Futures | MinPriceIncrement, DisplayFactor, UnitOfMeasureQty, MarketDepth |
