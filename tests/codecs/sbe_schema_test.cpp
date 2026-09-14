// Generated SBE flyweights (tools/sbe_gen.py over the CME MDP 3.0 schema subset): the byte-level
// example from CME's wiki, exact decimal conversion, group bounds, schema evolution (acting block
// length and version) and writer / reader round trips.
#include "mdp3_test_util.hpp"

#include "fastmm/codecs/mdp3/generated/mdp3_schema.hpp"
#include "fastmm/codecs/mdp3/mdp3_packet.hpp"
#include "fastmm/codecs/sbe/sbe.hpp"

#include <cstdint>
#include <limits>
#include <vector>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::mdp3;
using namespace fastmm::codecs::mdp3::test;

// Template ids, block lengths and schema version as published in templates_FixBinary.xml v13.
static_assert(schema::kSchemaId == 1 && schema::kSchemaVersion == 13);
static_assert(schema::ChannelReset4::kTemplateId == 4 && schema::ChannelReset4::kBlockLength == 9);
static_assert(schema::AdminHeartbeat12::kTemplateId == 12 &&
              schema::AdminHeartbeat12::kBlockLength == 0);
static_assert(schema::SecurityStatus30::kTemplateId == 30 &&
              schema::SecurityStatus30::kBlockLength == 30);
static_assert(schema::MDIncrementalRefreshBook46::kTemplateId == 46 &&
              schema::MDIncrementalRefreshBook46::kBlockLength == 11 &&
              schema::MDIncrementalRefreshBook46::NoMDEntries::kBlockLength == 32 &&
              schema::MDIncrementalRefreshBook46::NoOrderIDEntries::kBlockLength == 24);
static_assert(schema::MDIncrementalRefreshTradeSummary48::kTemplateId == 48 &&
              schema::MDIncrementalRefreshTradeSummary48::NoMDEntries::kBlockLength == 32);
static_assert(schema::SnapshotFullRefresh52::kTemplateId == 52 &&
              schema::SnapshotFullRefresh52::kBlockLength == 59 &&
              schema::SnapshotFullRefresh52::NoMDEntries::kBlockLength == 22);
static_assert(schema::MDInstrumentDefinitionFuture54::kTemplateId == 54 &&
              schema::MDInstrumentDefinitionFuture54::kBlockLength == 224);
static_assert(schema::GroupSize::kSize == 3 && schema::GroupSize8Byte::kSize == 8 &&
              schema::GroupSize8Byte::kNumInGroupOffset == 7);

TEST_CASE("codecs.sbe: the CME wiki decoding example decodes field by field") {
  const Bytes pkt = hex_fixture("cme/sbe_decoding_example_template50.hex");
  REQUIRE(pkt.size() == kPacketHeaderSize + 56);
  PacketCursor cur(pkt);
  REQUIRE(cur.valid());
  CHECK(cur.header().msg_seq_num == 703398U);
  CHECK(cur.header().sending_time == 1633099253939247451ULL);
  MessageView m;
  REQUIRE(cur.next(m));
  CHECK(m.size == 56);
  CHECK(m.header.block_length == 11);
  CHECK(m.header.template_id == 50);
  CHECK(m.header.schema_id == 1);
  CHECK(m.header.version == 9);
  const schema::MDIncrementalRefreshLimitsBanding50 msg(
      m.body, m.header.block_length, m.header.version);
  REQUIRE(msg.valid());
  CHECK(msg.size_bytes() == m.body.size());
  CHECK(msg.transact_time() == 1633099253937623627ULL);
  CHECK(msg.match_event_indicator().bits == 0);
  const auto g = msg.no_md_entries();
  REQUIRE(g.valid());
  REQUIRE(g.count() == 1);
  CHECK(g.block_length() == 32);
  const auto e = g[0];
  CHECK(e.high_limit_price().is_null());
  Price low{};
  REQUIRE(e.low_limit_price().to_fixed(low));
  CHECK(low == Price::from_int(9000));
  Price variation{};
  REQUIRE(e.max_price_variation().to_fixed(variation));
  CHECK(variation == Price::from_int(10));
  CHECK(e.security_id() == 5620);
  CHECK(e.rpt_seq() == 1869U);
  CHECK(e.md_update_action() == 0);  // constant, not on the wire
  CHECK(e.md_entry_type() == 'g');   // constant, not on the wire
  CHECK_FALSE(cur.next(m));
  CHECK_FALSE(cur.malformed());
}

TEST_CASE("codecs.sbe: decimals convert exactly to 1e-8 fixed point or are rejected") {
  Price p{};
  CHECK(schema::PRICE9{4'500'250'000'000}.to_fixed(p));
  CHECK(p == px("4500.25"));
  CHECK(schema::PRICE9{-12'340}.to_fixed(p));
  CHECK(p == px("-0.00001234"));
  CHECK_FALSE(schema::PRICE9{1}.to_fixed(p));  // 1e-9 needs a ninth decimal
  CHECK_FALSE(schema::PRICE9{123'456'789}.to_fixed(p));
  CHECK_FALSE(schema::PRICENULL9::null().to_fixed(p));
  CHECK(schema::PRICENULL9::null().is_null());
  CHECK_FALSE(schema::PRICE9{std::numeric_limits<std::int64_t>::max()}.is_null());

  schema::PRICENULL9 d{};
  CHECK(schema::PRICENULL9::from_fixed(px("0.25"), d));
  CHECK(d.mantissa == 250'000'000);
  schema::PRICE9 big{};
  CHECK_FALSE(schema::PRICE9::from_fixed(Price::max(), big));  // x10 overflows int64

  Qty q{};
  CHECK(schema::DecimalQty{10'000}.to_fixed(q));  // exponent -4
  CHECK(q == Qty::from_int(1));
  CHECK_FALSE(schema::DecimalQty::null().to_fixed(q));
  schema::DecimalQty lot{};
  CHECK_FALSE(schema::DecimalQty::from_fixed(Qty::from_int(1'000'000), lot));  // int32 range

  std::int64_t raw = 0;
  CHECK(sbe::decimal_to_fixed_raw(7, 2, raw));
  CHECK(raw == 70'000'000'000);
  CHECK_FALSE(sbe::decimal_to_fixed_raw(std::numeric_limits<std::int64_t>::max() / 5, 1, raw));
  CHECK(sbe::fixed_raw_to_decimal(250'000'000, -9, raw));
  CHECK(raw == 2'500'000'000);
}

namespace {
// Book46 as a newer schema version would encode it: root block 13 bytes, entries 36 bytes.
Bytes extended_book46(std::uint16_t entry_block) {
  const std::size_t root = 13;
  Bytes body(root + 3 + 2 * std::size_t{entry_block} + 8);
  sbe::store_le<std::uint64_t>(body.data(), 77);
  sbe::store_le<std::uint16_t>(body.data() + root, entry_block);
  body[root + 2] = std::byte{2};
  for (std::size_t i = 0; i < 2; ++i) {
    std::byte* e = body.data() + root + 3 + i * entry_block;
    sbe::store_le<std::int64_t>(e, static_cast<std::int64_t>(i + 1) * 1'000'000'000);
    sbe::store_le<std::int32_t>(e + 8, 5 + static_cast<std::int32_t>(i));
    sbe::store_le<std::int32_t>(e + 12, 9);
    sbe::store_le<std::uint32_t>(e + 16, 10 + static_cast<std::uint32_t>(i));
    sbe::store_le<std::uint8_t>(e + 24, 1);
    sbe::store_le<char>(e + 26, i == 0 ? '0' : '1');
    if (entry_block >= 31) sbe::store_le<std::int32_t>(e + 27, 99);
  }
  sbe::store_le<std::uint16_t>(body.data() + root + 3 + 2 * std::size_t{entry_block}, 24);
  return body;
}
}  // namespace

TEST_CASE("codecs.sbe: group views honour the acting block length and reject truncation") {
  const Bytes body = extended_book46(36);
  const schema::MDIncrementalRefreshBook46 msg(body, 13, 13);
  REQUIRE(msg.valid());
  CHECK(msg.size_bytes() == body.size());
  CHECK(msg.transact_time() == 77U);
  const auto g = msg.no_md_entries();
  REQUIRE(g.count() == 2);
  CHECK(g.block_length() == 36);
  CHECK(g[1].rpt_seq() == 11U);
  CHECK(g[1].md_entry_type() == schema::MDEntryTypeBook::Offer);
  CHECK(g[1].tradeable_size() == 99);
  Price p1{};
  REQUIRE(g[1].md_entry_px().to_fixed(p1));
  CHECK(p1 == Price::from_int(2));
  std::size_t n = 0;
  for (const auto e : g) {
    CHECK(e.security_id() == 9);
    ++n;
  }
  CHECK(n == 2);
  CHECK(msg.no_order_id_entries().valid());
  CHECK(msg.no_order_id_entries().count() == 0);

  for (std::size_t len = 0; len < body.size(); ++len) {
    const schema::MDIncrementalRefreshBook46 cut(
        std::span<const std::byte>(body).first(len), 13, 13);
    CHECK_FALSE(cut.valid());
    CHECK(cut.size_bytes() == 0);
  }
  // Root block shorter than the fields every version has.
  CHECK_FALSE(schema::MDIncrementalRefreshBook46(body, 8, 13).valid());
  // Entries shorter than the required fields.
  const Bytes short_entries = extended_book46(20);
  CHECK_FALSE(schema::MDIncrementalRefreshBook46(short_entries, 13, 13).valid());
}

TEST_CASE("codecs.sbe: fields newer than the acting version or block read as null") {
  const Bytes body = extended_book46(36);
  using Entry = schema::MDIncrementalRefreshBook46::NoMDEntries;
  // TradeableSize is sinceVersion 10.
  CHECK(schema::MDIncrementalRefreshBook46(body, 13, 9).no_md_entries()[0].tradeable_size() ==
        Entry::kTradeableSizeNull);
  CHECK(schema::MDIncrementalRefreshBook46(body, 13, 10).no_md_entries()[0].tradeable_size() == 99);
  // A version-9 sized entry (27 bytes) cannot contain it even if the header claims version 13.
  const Bytes v9 = extended_book46(27);
  const schema::MDIncrementalRefreshBook46 old(v9, 13, 13);
  REQUIRE(old.valid());
  CHECK_FALSE(old.no_md_entries()[0].has_tradeable_size());
  CHECK(old.no_md_entries()[1].rpt_seq() == 11U);
}

TEST_CASE("codecs.sbe: generated writers round-trip through the readers") {
  Bytes buf(kMaxPacketBytes);
  PacketBuilder p(buf);
  REQUIRE(p.begin(5, 6));
  DefinitionSpec d;
  d.security_id = 77;
  d.symbol = "NQH7";
  d.security_group = "NQ";
  d.asset = "NQ";
  d.tick = px("0.25");
  d.display_factor_e9 = 10'000'000;
  d.unit_of_measure_qty = Qty::from_int(20);
  d.market_depth = 5;
  d.implied_depth = 2;
  d.appl_id = 312;
  REQUIRE(encode_definition54(p, d));
  REQUIRE(encode_security_status30(p, 9, 77, schema::SecurityTradingStatus::PreOpen));
  REQUIRE(encode_channel_reset4(p, 10, 312));
  REQUIRE(encode_heartbeat12(p));
  CHECK(p.messages() == 4);

  PacketCursor cur(p.packet());
  REQUIRE(cur.valid());
  CHECK(cur.header().msg_seq_num == 5U);
  MessageView m;
  REQUIRE(cur.next(m));
  CHECK(m.header.template_id == 54);
  CHECK(m.header.schema_id == schema::kSchemaId);
  CHECK(m.header.version == schema::kSchemaVersion);
  CHECK(m.header.block_length == 224);
  const schema::MDInstrumentDefinitionFuture54 def(m.body, m.header.block_length, m.header.version);
  REQUIRE(def.valid());
  CHECK(def.size_bytes() == m.body.size());
  CHECK(def.symbol() == "NQH7");
  CHECK(def.security_group() == "NQ");
  CHECK(def.security_id() == 77);
  CHECK(def.security_id_source() == '8');
  CHECK(def.security_update_action() == schema::SecurityUpdateAction::Add);
  Price tick{};
  REQUIRE(def.min_price_increment().to_fixed(tick));
  CHECK(tick == px("0.25"));
  CHECK(def.display_factor().mantissa == 10'000'000);
  Qty uom{};
  REQUIRE(def.unit_of_measure_qty().to_fixed(uom));
  CHECK(uom == Qty::from_int(20));
  CHECK_FALSE(def.has_contract_multiplier());
  CHECK_FALSE(def.has_tot_num_reports());
  CHECK(def.maturity_month_year().year == schema::MaturityMonthYear::kYearNull);
  const auto feeds = def.no_md_feed_types();
  REQUIRE(feeds.count() == 2);
  CHECK(feeds[0].md_feed_type() == "GBX");
  CHECK(feeds[0].market_depth() == 5);
  CHECK(feeds[1].md_feed_type() == "GBI");
  CHECK(feeds[1].market_depth() == 2);
  CHECK(def.no_events().count() == 0);
  CHECK(def.no_inst_attrib().count() == 0);
  CHECK(def.no_lot_type_rules().count() == 0);

  REQUIRE(cur.next(m));
  const schema::SecurityStatus30 status(m.body, m.header.block_length, m.header.version);
  REQUIRE(status.valid());
  CHECK(status.security_id() == 77);
  CHECK(status.security_trading_status() == schema::SecurityTradingStatus::PreOpen);
  CHECK(schema::is_valid(status.halt_reason()));
  CHECK(status.match_event_indicator().end_of_event());

  REQUIRE(cur.next(m));
  const schema::ChannelReset4 reset(m.body, m.header.block_length, m.header.version);
  REQUIRE(reset.valid());
  REQUIRE(reset.no_md_entries().count() == 1);
  CHECK(reset.no_md_entries()[0].appl_id() == 312);
  CHECK(reset.no_md_entries()[0].md_entry_type() == 'J');

  REQUIRE(cur.next(m));
  CHECK(m.header.template_id == 12);
  CHECK(m.body.empty());
  CHECK_FALSE(cur.next(m));
  CHECK_FALSE(cur.malformed());

  // A message that does not fit leaves the packet untouched.
  Bytes small(64);
  PacketBuilder tiny(small);
  REQUIRE(tiny.begin(1, 1));
  CHECK_FALSE(encode_definition54(tiny, d));
  CHECK(tiny.size() == kPacketHeaderSize);
  CHECK(tiny.messages() == 0);
}

TEST_CASE("codecs.sbe: enums, bitsets and template names") {
  CHECK(schema::is_valid(schema::MDUpdateAction::Overlay));
  CHECK_FALSE(schema::is_valid(static_cast<schema::MDUpdateAction>(9)));
  CHECK_FALSE(schema::is_valid(schema::AggressorSide::NullValue));
  CHECK(schema::to_string(schema::MDEntryTypeBook::ImpliedBid) == "ImpliedBid");
  CHECK(static_cast<char>(schema::MDEntryTypeBook::BookReset) == 'J');
  schema::MatchEventIndicator mei{};
  mei.set_end_of_event(true);
  mei.set_last_quote_msg(true);
  CHECK(mei.bits == 0x84);
  mei.set_end_of_event(false);
  CHECK(mei.bits == 0x04);
  CHECK_FALSE(mei.recovery_msg());
  CHECK(schema::template_name(46) == "MDIncrementalRefreshBook46");
  CHECK(schema::template_name(47) == "?");
}

TEST_CASE("codecs.mdp3: MessageFramer splits a datagram into SBE messages") {
  static_assert(Framer<MessageFramer>);
  const Bytes pkt = hex_fixture("cme/admin_mixed.hex");
  MessageFramer f;
  std::span<const std::byte> rest = std::span<const std::byte>(pkt).subspan(kPacketHeaderSize);
  std::vector<std::uint16_t> templates;
  while (!rest.empty()) {
    const FrameView v = f.next(rest);
    REQUIRE(v.complete());
    templates.push_back(sbe::MessageHeader::load(v.payload.data()).template_id);
    rest = rest.subspan(v.consumed);
  }
  CHECK(templates == std::vector<std::uint16_t>{12, 999, 30, 4});

  Bytes bad(12);
  bad[0] = std::byte{3};  // MsgSize smaller than MsgSize + SBE header
  CHECK_FALSE(f.next(bad).complete());
  bad[0] = std::byte{200};  // beyond the datagram
  CHECK_FALSE(f.next(bad).complete());
}
