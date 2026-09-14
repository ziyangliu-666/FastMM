// Mdp3Decoder: CME MDP 3.0 fixtures (packed independently of the generated writers) to engine
// events, MBP level semantics, recovery triggers, snapshots and malformed input.
#include "mdp3_test_util.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::mdp3;
using namespace fastmm::codecs::mdp3::test;
using venues::ParseStatus;
using A = schema::MDUpdateAction;
using T = schema::MDEntryTypeBook;
using LV = std::vector<Level>;

static_assert(codecs::Decoder<Mdp3Decoder>);
static_assert(codecs::Decoder<Mdp3Feed>);

namespace {

constexpr std::int64_t kRx = 1'789'000'000'200'000'000;
constexpr std::uint64_t kTransact = 1'789'000'000'123'456'789ULL;

struct Setup {
  RecordingRing ring;
  std::unique_ptr<Mdp3Decoder> dec = std::make_unique<Mdp3Decoder>(Mdp3DecoderConfig{VenueId{3}});
  EngineBooks books{2};

  ParseStatus decode(const Bytes& pkt) {
    const ParseStatus st = dec->decode_packet(pkt, kRx, ring.sink);
    books.apply(ring.drain());
    return st;
  }
};

Level lv(const char* price, std::int64_t qty) {
  return Level{px(price), Qty::from_int(qty)};
}

schema::SnapshotFullRefresh52 snapshot_view(const Bytes& pkt) {
  PacketCursor cur(pkt);
  MessageView m;
  REQUIRE(cur.next(m));
  REQUIRE(m.header.template_id == schema::SnapshotFullRefresh52::kTemplateId);
  return schema::SnapshotFullRefresh52(m.body, m.header.block_length, m.header.version);
}

// Snapshot with bid entries at explicit MDPriceLevels (can describe invalid books).
Bytes bid_snapshot(std::int32_t security_id,
                   std::uint32_t rpt,
                   const std::vector<std::pair<int, const char*>>& levels) {
  return make_packet(1, [&](PacketBuilder& p) {
    schema::SnapshotFullRefresh52Writer w(p.body_space());
    w.set_last_msg_seq_num_processed(1);
    w.set_tot_num_reports(1);
    w.set_security_id(security_id);
    w.set_rpt_seq(rpt);
    w.set_high_limit_price(schema::PRICENULL9::null());
    w.set_low_limit_price(schema::PRICENULL9::null());
    w.set_max_price_variation(schema::PRICENULL9::null());
    const auto g = w.no_md_entries(levels.size());
    REQUIRE(g.ok());
    for (std::size_t i = 0; i < levels.size(); ++i) {
      schema::PRICENULL9 m{};
      REQUIRE(schema::PRICENULL9::from_fixed(px(levels[i].second), m));
      auto e = g[i];
      e.set_md_entry_px(m);
      e.set_md_entry_size(1);
      e.set_md_price_level(static_cast<std::int8_t>(levels[i].first));
      e.set_md_entry_type(schema::MDEntryType::Bid);
    }
    REQUIRE(p.commit(w));
  });
}

}  // namespace

TEST_CASE("codecs.mdp3.decoder: Book46 fixture becomes one BookDeltaMsg") {
  Setup s;
  REQUIRE(s.dec->add_instrument(1234) == 0);
  const Bytes pkt = hex_fixture("cme/book46_two_levels.hex");
  CHECK(s.dec->decode_packet(pkt, kRx, s.ring.sink) == ParseStatus::Ok);
  const auto ev = s.ring.drain();
  REQUIRE(ev.size() == 1);
  const auto& m = as<BookDeltaMsg>(ev[0]);
  CHECK(m.hdr.type == EventType::BookDelta);
  CHECK(m.hdr.len == BookDeltaMsg::size_for(1, 1));
  CHECK(m.hdr.version == kMessageVersion);
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == VenueId{3});
  CHECK(m.hdr.flags == 0);
  CHECK(m.hdr.exch_ts.ns == static_cast<std::int64_t>(kTransact));
  CHECK(m.hdr.recv_ts.ns == kRx);
  CHECK(m.hdr.venue_seq == 2);
  CHECK(m.first_update_id == 1);
  CHECK(m.last_update_id == 2);
  CHECK(m.prev_update_id == 0);
  REQUIRE(m.bid_count == 1);
  REQUIRE(m.ask_count == 1);
  CHECK(m.bids()[0] == lv("4500.25", 12));
  CHECK(m.asks()[0] == lv("4500.5", 7));
  const MbpBookState& b = s.dec->book(0);
  CHECK(b.rpt_seq == 2);
  CHECK(b.transact_time == kTransact);
  CHECK(to_vector(b.bids().view()) == LV({lv("4500.25", 12)}));
  CHECK_FALSE(b.recovering);
  CHECK(s.dec->stats().book_entries == 2);
  CHECK(s.dec->stats().book_deltas == 1);
  CHECK_FALSE(s.dec->take_events().needs_recovery());
}

TEST_CASE("codecs.mdp3.decoder: TradeSummary48 fixture becomes a TradeMsg with the aggressor") {
  Setup s;
  REQUIRE(s.dec->add_instrument(1234) == 0);
  REQUIRE(s.decode(hex_fixture("cme/book46_two_levels.hex")) == ParseStatus::Ok);
  CHECK(s.dec->decode_packet(hex_fixture("cme/trade48_buy_aggressor.hex"), kRx, s.ring.sink) ==
        ParseStatus::Ok);
  auto ev = s.ring.drain();
  REQUIRE(ev.size() == 1);
  const auto& t = as<TradeMsg>(ev[0]);
  CHECK(t.hdr.type == EventType::Trade);
  CHECK(t.hdr.len == sizeof(TradeMsg));
  CHECK(t.hdr.instrument == InstrumentId{0});
  CHECK(t.hdr.exch_ts.ns == static_cast<std::int64_t>(kTransact + 1));
  CHECK(t.hdr.venue_seq == 3);
  CHECK(t.price == px("4500.5"));
  CHECK(t.qty == Qty::from_int(3));
  CHECK(t.trade_id == 777);
  CHECK(t.aggressor == Side::Buy);
  CHECK(t.pad_[0] == 0);

  // No aggressor, and trade adjustments (Change / Delete) that have no engine event.
  const Bytes more = make_packet(1003, [](PacketBuilder& p) {
    TradeEntry none;
    none.security_id = 1234;
    none.rpt_seq = 4;
    none.price = px("4500.25");
    none.qty = 1;
    none.aggressor = schema::AggressorSide::NoAggressor;
    TradeEntry adjust = none;
    adjust.rpt_seq = 5;
    adjust.action = A::Change;
    TradeEntry sell = none;
    sell.rpt_seq = 6;
    sell.aggressor = schema::AggressorSide::Sell;
    const std::vector<TradeEntry> entries{none, adjust, sell};
    REQUIRE(encode_trade48(p, 5, end_of_event(), entries));
  });
  CHECK(s.dec->decode_packet(more, kRx, s.ring.sink) == ParseStatus::Ok);
  ev = s.ring.drain();
  REQUIRE(ev.size() == 2);
  CHECK(as<TradeMsg>(ev[0]).pad_[0] == kTradeNoAggressor);
  CHECK(as<TradeMsg>(ev[0]).trade_id == 4);  // MDTradeEntryID null: RptSeq instead
  CHECK(as<TradeMsg>(ev[1]).aggressor == Side::Sell);
  CHECK(s.dec->stats().trades == 3);
  CHECK(s.dec->stats().trade_adjustments == 1);
  CHECK(s.dec->book(0).rpt_seq == 6);
}

TEST_CASE("codecs.mdp3.decoder: instrument definition fixture fills the instrument table") {
  Setup s;
  CHECK(s.decode(hex_fixture("cme/secdef54_esz6.hex")) == ParseStatus::Ok);
  CHECK(s.dec->stats().definitions == 1);
  const Mdp3Instrument* d = s.dec->instruments().find(1234);
  REQUIRE(d != nullptr);
  CHECK(d->id == InstrumentId{0});
  CHECK(d->symbol == "ESZ6");
  CHECK(d->security_group == "ES");
  CHECK(d->asset == "ES");
  CHECK(d->tick == px("0.25"));
  CHECK(d->display_factor_e9 == 10'000'000);
  CHECK(d->unit_of_measure_qty == Qty::from_int(50));
  CHECK(d->contract_multiplier == 0);
  CHECK(d->appl_id == 310);
  CHECK(d->market_depth == 10);
  CHECK(d->implied_depth == 2);
  CHECK_FALSE(d->deleted);
  const Instrument inst = Mdp3InstrumentTable::to_instrument(*d, VenueId{3});
  CHECK(inst.symbol == "ESZ6");
  CHECK(inst.tick == px("0.25"));
  CHECK(inst.contract_multiplier == Qty::from_int(50));
  CHECK(inst.asset_class == AssetClass::Future);
  CHECK(inst.venue == VenueId{3});

  // A second definition for the same SecurityID updates in place.
  CHECK(s.decode(hex_fixture("cme/secdef54_esz6.hex")) == ParseStatus::Ok);
  CHECK(s.dec->instruments().size() == 1);
  // The book fixture now decodes against the defined instrument.
  CHECK(s.decode(hex_fixture("cme/book46_two_levels.hex")) == ParseStatus::Ok);
  CHECK(s.dec->stats().book_deltas == 1);
  CHECK(s.dec->stats().unknown_security == 0);
}

TEST_CASE("codecs.mdp3.decoder: heartbeat, unknown template, security status and channel reset") {
  Setup s;
  REQUIRE(s.dec->add_instrument(1234) == 0);
  REQUIRE(s.decode(hex_fixture("cme/book46_two_levels.hex")) == ParseStatus::Ok);
  static_cast<void>(s.dec->take_events());
  CHECK(s.dec->decode_packet(hex_fixture("cme/admin_mixed.hex"), kRx, s.ring.sink) ==
        ParseStatus::Ok);
  const auto& st = s.dec->stats();
  CHECK(st.heartbeats == 1);
  CHECK(st.unknown_templates == 1);
  CHECK(st.security_status == 1);
  CHECK(st.channel_resets == 1);
  CHECK(st.malformed == 0);
  CHECK(s.dec->instruments().at(0).trading_status == 2);  // TradingHalt
  CHECK(s.dec->take_events().channel_reset);
  const auto ev = s.ring.drain();
  REQUIRE(ev.size() == 1);
  const auto& snap = as<BookSnapshotMsg>(ev[0]);
  CHECK(snap.hdr.type == EventType::BookSnapshot);
  CHECK(snap.is_snapshot());
  CHECK(snap.bid_count == 0);
  CHECK(snap.ask_count == 0);
  CHECK(s.dec->book(0).bids().count == 0);
  CHECK(s.dec->book(0).rpt_seq == 0);
  // RptSeq restarts at 1 after a channel reset.
  CHECK(s.decode(book_packet(2000, {book_entry(1234, 1, A::New, T::Bid, 1, "4499", 1)})) ==
        ParseStatus::Ok);
  CHECK(s.dec->stats().rpt_gaps == 0);
  CHECK(s.dec->book(0).bids().count == 1);
}

TEST_CASE("codecs.mdp3.decoder: MBP level actions follow MDPriceLevel semantics") {
  Setup s;
  REQUIRE(s.dec->add_instrument(7, 5) == 0);
  std::uint32_t seq = 0;
  std::uint32_t rpt = 0;
  const auto apply =
      [&](A action, T type, std::uint8_t level, const char* price, std::int32_t qty) {
        ++seq;
        ++rpt;
        return s.decode(book_packet(seq, {book_entry(7, rpt, action, type, level, price, qty)}));
      };
  const auto bids = [&]() { return to_vector(s.dec->book(0).bids().view()); };
  const auto engine_bids = [&]() { return l2_levels(*s.books.books[0], Side::Buy); };

  CHECK(apply(A::New, T::Bid, 1, "100", 1) == ParseStatus::Ok);
  CHECK(apply(A::New, T::Bid, 1, "101", 2) == ParseStatus::Ok);
  CHECK(apply(A::New, T::Bid, 3, "99", 3) == ParseStatus::Ok);
  CHECK(bids() == LV({lv("101", 2), lv("100", 1), lv("99", 3)}));
  CHECK(apply(A::Change, T::Bid, 2, "100", 5) == ParseStatus::Ok);
  CHECK(bids() == LV({lv("101", 2), lv("100", 5), lv("99", 3)}));
  CHECK(apply(A::Delete, T::Bid, 1, "101", 0) == ParseStatus::Ok);
  CHECK(bids() == LV({lv("100", 5), lv("99", 3)}));
  CHECK(apply(A::Overlay, T::Bid, 2, "98", 4) == ParseStatus::Ok);
  CHECK(bids() == LV({lv("100", 5), lv("98", 4)}));
  CHECK(engine_bids() == bids());
  CHECK(apply(A::New, T::Bid, 3, "97", 6) == ParseStatus::Ok);
  CHECK(apply(A::New, T::Bid, 4, "96", 7) == ParseStatus::Ok);
  CHECK(apply(A::New, T::Bid, 5, "95", 8) == ParseStatus::Ok);
  CHECK(apply(A::New, T::Bid, 1, "102", 9) == ParseStatus::Ok);  // 95 falls off the 5-deep book
  CHECK(bids() == LV({lv("102", 9), lv("100", 5), lv("98", 4), lv("97", 6), lv("96", 7)}));
  CHECK(engine_bids() == bids());
  CHECK(apply(A::DeleteFrom, T::Bid, 4, "96", 0) == ParseStatus::Ok);
  CHECK(bids() == LV({lv("102", 9), lv("100", 5), lv("98", 4)}));
  CHECK(apply(A::DeleteThru, T::Bid, 2, "100", 0) == ParseStatus::Ok);
  CHECK(bids() == LV({lv("98", 4)}));
  CHECK(engine_bids() == bids());
  CHECK(apply(A::New, T::Offer, 1, "103", 1) == ParseStatus::Ok);
  CHECK(apply(A::New, T::Offer, 2, "104", 2) == ParseStatus::Ok);
  CHECK(to_vector(s.dec->book(0).asks().view()) == LV({lv("103", 1), lv("104", 2)}));
  CHECK(l2_levels(*s.books.books[0], Side::Sell) == LV({lv("103", 1), lv("104", 2)}));
  // Several entries of one message for one instrument become a single BookDeltaMsg.
  const std::size_t deltas = s.books.deltas;
  CHECK(s.decode(book_packet(++seq,
                             {book_entry(7, rpt + 1, A::Change, T::Offer, 1, "103", 3),
                              book_entry(7, rpt + 2, A::Delete, T::Bid, 1, "98", 0)})) ==
        ParseStatus::Ok);
  rpt += 2;
  CHECK(s.books.deltas == deltas + 1);
  CHECK(s.dec->book(0).bids().count == 0);
  CHECK(s.books.books[0]->depth(Side::Buy) == 0);
  CHECK(s.dec->stats().book_errors == 0);
  CHECK(s.dec->book(0).rpt_seq == rpt);
}

TEST_CASE(
    "codecs.mdp3.decoder: inconsistent entries and RptSeq gaps put the instrument into recovery") {
  Setup s;
  REQUIRE(s.dec->add_instrument(7, 5) == 0);
  REQUIRE(s.decode(book_packet(1, {book_entry(7, 1, A::New, T::Bid, 1, "100", 1)})) ==
          ParseStatus::Ok);
  const auto expect_error = [&](const MbpEntry& e) {
    CHECK(s.decode(book_packet(2, {e})) == ParseStatus::Ok);
    CHECK(s.dec->book(0).recovering);
    CHECK(s.dec->recovering_count() == 1);
    CHECK(s.dec->take_events().book_error);
  };
  SUBCASE("Change at a different price") {
    expect_error(book_entry(7, 2, A::Change, T::Bid, 1, "101", 2));
    // Entries are skipped until a snapshot.
    CHECK(s.decode(book_packet(3, {book_entry(7, 3, A::New, T::Bid, 1, "102", 1)})) ==
          ParseStatus::Ok);
    CHECK(s.ring.drain().empty());
    CHECK(s.dec->book(0).bids().count == 1);
  }
  SUBCASE("New below the last level") {
    expect_error(book_entry(7, 2, A::New, T::Bid, 3, "98", 1));
  }
  SUBCASE("New that would cross the level it displaces") {
    expect_error(book_entry(7, 2, A::New, T::Bid, 1, "99", 1));
  }
  SUBCASE("MDPriceLevel beyond MarketDepth") {
    expect_error(book_entry(7, 2, A::New, T::Bid, 6, "90", 1));
  }
  SUBCASE("MDPriceLevel zero") {
    expect_error(book_entry(7, 2, A::Delete, T::Bid, 0, "100", 0));
  }
  SUBCASE("Delete of a level that does not exist") {
    expect_error(book_entry(7, 2, A::Delete, T::Offer, 1, "101", 0));
  }
  SUBCASE("price with more than 8 decimals") {
    Bytes pkt = book_packet(2, {book_entry(7, 2, A::New, T::Bid, 1, "101", 1)});
    const std::size_t px_offset = kPacketHeaderSize + kMessagePrefixSize +
                                  schema::MDIncrementalRefreshBook46::kBlockLength + 3;
    sbe::store_le<std::int64_t>(pkt.data() + px_offset, 101'000'000'001);
    CHECK(s.decode(pkt) == ParseStatus::Ok);
    CHECK(s.dec->stats().price_rejects == 1);
    CHECK(s.dec->book(0).recovering);
  }
  SUBCASE("RptSeq gap") {
    CHECK(s.decode(book_packet(2, {book_entry(7, 3, A::New, T::Bid, 1, "101", 1)})) ==
          ParseStatus::Ok);
    CHECK(s.dec->stats().rpt_gaps == 1);
    CHECK(s.dec->book(0).recovering);
    const Mdp3DecodeEvents ev = s.dec->take_events();
    CHECK(ev.rpt_gap);
    CHECK(ev.needs_recovery());
    CHECK(s.dec->book(0).bids().count == 1);  // the gapped entry was not applied
  }
  SUBCASE("replayed RptSeq is stale, not a gap") {
    CHECK(s.decode(book_packet(2, {book_entry(7, 1, A::New, T::Bid, 1, "101", 1)})) ==
          ParseStatus::Ok);
    CHECK(s.dec->stats().stale_entries == 1);
    CHECK_FALSE(s.dec->book(0).recovering);
    CHECK_FALSE(s.dec->take_events().needs_recovery());
  }
  SUBCASE("statistics templates consume RptSeq") {
    const Bytes stats = make_packet(2, [](PacketBuilder& p) {
      schema::MDIncrementalRefreshVolume37Writer w(p.body_space());
      w.set_transact_time(1);
      const auto g = w.no_md_entries(1);
      REQUIRE(g.ok());
      g[0].set_md_entry_size(10);
      g[0].set_security_id(7);
      g[0].set_rpt_seq(2);
      g[0].set_md_update_action(A::New);
      REQUIRE(p.commit(w));
    });
    CHECK(s.decode(stats) == ParseStatus::Ok);
    CHECK(s.dec->stats().stat_entries == 1);
    CHECK(s.decode(book_packet(3, {book_entry(7, 3, A::New, T::Bid, 1, "101", 1)})) ==
          ParseStatus::Ok);
    CHECK(s.dec->stats().rpt_gaps == 0);
    CHECK(s.dec->book(0).bids().count == 2);
  }
  SUBCASE("implied entries are counted and ignored") {
    CHECK(s.decode(book_packet(2, {book_entry(7, 2, A::New, T::ImpliedBid, 1, "101", 1)})) ==
          ParseStatus::Ok);
    CHECK(s.dec->stats().implied_ignored == 1);
    CHECK(s.ring.drain().empty());
    CHECK(s.decode(book_packet(3, {book_entry(7, 3, A::New, T::Bid, 1, "101", 1)})) ==
          ParseStatus::Ok);
    CHECK(s.dec->stats().rpt_gaps == 0);
    CHECK(s.dec->book(0).bids().count == 2);
  }
  SUBCASE("unknown SecurityID") {
    CHECK(s.decode(book_packet(2, {book_entry(8, 1, A::New, T::Bid, 1, "101", 1)})) ==
          ParseStatus::Ok);
    CHECK(s.dec->stats().unknown_security == 1);
    CHECK_FALSE(s.dec->book(0).recovering);
  }
}

TEST_CASE("codecs.mdp3.decoder: a snapshot replaces the book and sets the RptSeq baseline") {
  Setup s;
  REQUIRE(s.dec->add_instrument(7, 3) == 0);
  REQUIRE(s.decode(book_packet(1, {book_entry(7, 1, A::New, T::Bid, 1, "100", 1)})) ==
          ParseStatus::Ok);
  const Bytes snap = make_packet(1, [](PacketBuilder& p) {
    const SnapshotSpec spec{10, 1, 7, 20, 555};
    const LV bids{lv("101", 1), lv("100", 2)};
    const LV asks{lv("102", 3)};
    REQUIRE(encode_snapshot52(p, spec, bids, asks));
  });
  REQUIRE(s.dec->apply_snapshot(snapshot_view(snap), kRx, s.ring.sink));
  const auto ev = s.ring.drain();
  REQUIRE(ev.size() == 1);
  const auto& m = as<BookSnapshotMsg>(ev[0]);
  CHECK(m.hdr.type == EventType::BookSnapshot);
  CHECK(m.is_snapshot());
  CHECK(m.bid_count == 2);
  CHECK(m.ask_count == 1);
  CHECK(m.last_update_id == 20);
  CHECK(m.hdr.exch_ts.ns == 555);
  s.books.apply(ev);
  CHECK(l2_levels(*s.books.books[0], Side::Buy) == LV({lv("101", 1), lv("100", 2)}));
  CHECK(s.dec->book(0).rpt_seq == 20);
  CHECK(s.dec->stats().snapshots_applied == 1);

  CHECK(s.decode(book_packet(2, {book_entry(7, 20, A::Change, T::Bid, 1, "101", 9)})) ==
        ParseStatus::Ok);
  CHECK(s.dec->stats().stale_entries == 1);
  CHECK(s.decode(book_packet(3, {book_entry(7, 21, A::Change, T::Bid, 1, "101", 9)})) ==
        ParseStatus::Ok);
  CHECK(s.dec->book(0).bids().levels[0].qty == Qty::from_int(9));

  const auto rejected = [&](const Bytes& pkt) {
    CHECK_FALSE(s.dec->apply_snapshot(snapshot_view(pkt), kRx, s.ring.sink));
    CHECK(s.dec->book(0).rpt_seq == 21);
    CHECK(s.ring.drain().empty());
  };
  rejected(bid_snapshot(7, 30, {{1, "101"}, {3, "99"}}));                         // hole
  rejected(bid_snapshot(7, 30, {{1, "101"}, {2, "100"}, {3, "99"}, {4, "98"}}));  // > depth
  rejected(bid_snapshot(7, 30, {{1, "100"}, {2, "101"}}));                        // unordered
  rejected(bid_snapshot(7, 30, {{1, "100"}, {1, "100"}}));                        // duplicate
  rejected(bid_snapshot(8, 30, {{1, "100"}}));                                    // unknown id
  CHECK(s.dec->stats().snapshots_rejected == 5);
}

TEST_CASE("codecs.mdp3.decoder: truncated packets are rejected without reading past the end") {
  for (const char* name : {"cme/book46_two_levels.hex",
                           "cme/trade48_buy_aggressor.hex",
                           "cme/secdef54_esz6.hex",
                           "cme/admin_mixed.hex"}) {
    INFO(name);
    const Bytes full = hex_fixture(name);
    Setup s;
    REQUIRE(s.dec->add_instrument(1234) == 0);
    const bool single_message = std::string(name).find("admin") == std::string::npos;
    for (std::size_t len = 0; len < full.size(); ++len) {
      const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
      const ParseStatus st = s.dec->decode_packet(cut, kRx, s.ring.sink);
      if (single_message) {
        CHECK(st != ParseStatus::Ok);
      } else {
        CHECK(st != ParseStatus::Overflow);
      }
      static_cast<void>(s.ring.drain());
    }
    CHECK(s.dec->stats().book_entries == 0);
    CHECK(s.dec->stats().trades == 0);
    CHECK(s.dec->stats().definitions == 0);
  }
  // Another schema id is skipped by MsgSize.
  Bytes other = hex_fixture("cme/book46_two_levels.hex");
  sbe::store_le<std::uint16_t>(other.data() + kPacketHeaderSize + kMsgSizeFieldSize + 4, 2);
  Setup s;
  REQUIRE(s.dec->add_instrument(1234) == 0);
  CHECK(s.decode(other) == ParseStatus::Ignored);
  CHECK(s.dec->stats().other_schema == 1);
}
