// TotalView-ITCH 5.0: layouts against the spec-derived fixtures, encoder bytes, decoder events.
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/itch/itch_decoder.hpp"
#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/codecs/itch/itch_messages.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::itch;
using fastmm::codecs::test::Bytes;
using fastmm::codecs::test::RecordingSink;

namespace {
constexpr std::uint64_t kTs = 34'200'000'000'123ULL;  // fixture timestamp
constexpr std::uint16_t kLocate = 7;
Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(std::int64_t n) {
  return Qty::from_int(n);
}
const std::map<std::string, Bytes>& fixtures() {
  static const std::map<std::string, Bytes> f =
      codecs::test::load_hex_fixture("itch50_messages.hex");
  return f;
}
const Bytes& fx(const char* name) {
  const auto it = fixtures().find(name);
  REQUIRE_MESSAGE(it != fixtures().end(), name);
  return it->second;
}
// A copy of the packed struct (the fixture bytes stay the only source of truth).
template <class M>
M view(const Bytes& b) {
  REQUIRE(b.size() == sizeof(M));
  M m;
  std::memcpy(&m, b.data(), sizeof m);
  return m;
}
}  // namespace

TEST_CASE("codecs.itch: fixtures cover all 23 message types at the spec lengths") {
  std::set<char> seen;
  for (const auto& [name, raw] : fixtures()) {
    REQUIRE_FALSE(raw.empty());
    const auto type = static_cast<char>(raw[0]);
    CHECK(type == name[0]);
    CHECK(message_length(type) == raw.size());
    seen.insert(type);
  }
  CHECK(seen.size() == kAllMessageTypes.size());
  for (char t : kAllMessageTypes) CHECK(seen.count(t) == 1);
  CHECK(message_length('Z') == 0);
  CHECK(message_length('a') == 0);
}

TEST_CASE("codecs.itch: packed structs read every fixture field") {
  {
    const auto& m = view<SystemEvent>(fx("S_system_event"));
    CHECK(m.hdr.stock_locate.get() == 0);
    CHECK(m.hdr.tracking_number.get() == 3);
    CHECK(m.hdr.timestamp.get() == kTs);
    CHECK(m.event_code == 'O');
  }
  {
    const auto& m = view<StockDirectory>(fx("R_stock_directory"));
    CHECK(m.hdr.stock_locate.get() == kLocate);
    CHECK(nasdaq::get_alpha(m.stock, 8) == "AAPL");
    CHECK(m.market_category == 'Q');
    CHECK(m.financial_status_indicator == 'N');
    CHECK(m.round_lot_size.get() == 100);
    CHECK(m.issue_classification == 'C');
    CHECK(m.authenticity == 'P');
    CHECK(m.luld_reference_price_tier == '1');
    CHECK(m.etp_leverage_factor.get() == 0);
    CHECK(m.inverse_indicator == 'N');
  }
  {
    const auto& m = view<StockTradingAction>(fx("H_stock_trading_action"));
    CHECK(m.trading_state == 'T');
    CHECK(nasdaq::get_alpha(m.reason, 4).empty());
  }
  CHECK(view<RegShoRestriction>(fx("Y_reg_sho_restriction")).reg_sho_action == '0');
  {
    const auto& m = view<MarketParticipantPosition>(fx("L_market_participant_position"));
    CHECK(nasdaq::get_alpha(m.mpid, 4) == "NSDQ");
    CHECK(nasdaq::get_alpha(m.stock, 8) == "AAPL");
    CHECK(m.market_participant_state == 'A');
  }
  {
    const auto& m = view<MwcbDeclineLevel>(fx("V_mwcb_decline_level"));
    Price p;
    REQUIRE(nasdaq::price8_to_price(m.level1.get(), p));
    CHECK(p == px("3000.12345678"));  // Price(8) is exactly the engine scale
    REQUIRE(nasdaq::price8_to_price(m.level2.get(), p));
    CHECK(p == px("2800.5"));
    REQUIRE(nasdaq::price8_to_price(m.level3.get(), p));
    CHECK(p == px("2600.00000001"));
  }
  CHECK(view<MwcbStatus>(fx("W_mwcb_status")).breached_level == '1');
  {
    const auto& m = view<IpoQuotingPeriodUpdate>(fx("K_ipo_quoting_period_update"));
    CHECK(m.ipo_quotation_release_time.get() == 34'200);
    CHECK(m.ipo_quotation_release_qualifier == 'A');
    CHECK(nasdaq::price4_to_price(m.ipo_price.get()) == px("12.34"));
  }
  {
    const auto& m = view<LuldAuctionCollar>(fx("J_luld_auction_collar"));
    CHECK(nasdaq::price4_to_price(m.auction_collar_reference_price.get()) == px("150"));
    CHECK(nasdaq::price4_to_price(m.upper_auction_collar_price.get()) == px("165"));
    CHECK(nasdaq::price4_to_price(m.lower_auction_collar_price.get()) == px("135"));
    CHECK(m.auction_collar_extension.get() == 1);
  }
  {
    const auto& m = view<OperationalHalt>(fx("h_operational_halt"));
    CHECK(m.market_code == 'Q');
    CHECK(m.operational_halt_action == 'H');
  }
  {
    const auto& m = view<AddOrder>(fx("A_add_order"));
    CHECK(m.order_reference_number.get() == 1001);
    CHECK(m.buy_sell_indicator == 'B');
    CHECK(m.shares.get() == 300);
    CHECK(nasdaq::get_alpha(m.stock, 8) == "AAPL");
    CHECK(nasdaq::price4_to_price(m.price.get()) == px("189.1234"));
  }
  {
    const auto& m = view<AddOrderMpid>(fx("F_add_order_mpid"));
    CHECK(m.buy_sell_indicator == 'S');
    CHECK(nasdaq::get_alpha(m.attribution, 4) == "GSCO");
  }
  {
    const auto& m = view<OrderExecuted>(fx("E_order_executed"));
    CHECK(m.executed_shares.get() == 100);
    CHECK(m.match_number.get() == 5001);
  }
  {
    const auto& m = view<OrderExecutedWithPrice>(fx("C_order_executed_with_price"));
    CHECK(m.printable == 'Y');
    CHECK(nasdaq::price4_to_price(m.execution_price.get()) == px("189.24"));
  }
  CHECK(view<OrderCancel>(fx("X_order_cancel")).cancelled_shares.get() == 25);
  CHECK(view<OrderDelete>(fx("D_order_delete")).order_reference_number.get() == 1002);
  {
    const auto& m = view<OrderReplace>(fx("U_order_replace"));
    CHECK(m.original_order_reference_number.get() == 1001);
    CHECK(m.new_order_reference_number.get() == 1003);
    CHECK(m.shares.get() == 400);
    CHECK(nasdaq::price4_to_price(m.price.get()) == px("189.1"));
  }
  {
    const auto& m = view<Trade>(fx("P_trade"));
    CHECK(m.order_reference_number.get() == 0);
    CHECK(m.match_number.get() == 5003);
  }
  {
    const auto& m = view<CrossTrade>(fx("Q_cross_trade"));
    CHECK(m.shares.get() == 123'456);
    CHECK(m.match_number.get() == 5004);
    CHECK(m.cross_type == 'O');
  }
  CHECK(view<BrokenTrade>(fx("B_broken_trade")).match_number.get() == 5003);
  {
    const auto& m = view<Noii>(fx("I_noii"));
    CHECK(m.paired_shares.get() == 1000);
    CHECK(m.imbalance_shares.get() == 500);
    CHECK(m.imbalance_direction == 'B');
    CHECK(nasdaq::get_alpha(m.stock, 8) == "AAPL");
    CHECK(nasdaq::price4_to_price(m.current_reference_price.get()) == px("189.05"));
    CHECK(m.cross_type == 'O');
    CHECK(m.price_variation_indicator == 'L');
  }
  CHECK(view<RetailInterest>(fx("N_retail_interest")).interest_flag == 'A');
  {
    const auto& m = view<DlcrPriceDiscovery>(fx("O_dlcr_price_discovery"));
    CHECK(m.open_eligibility_status == 'Y');
    CHECK(m.near_execution_time.get() == 34'200'000'000'000ULL);
    CHECK(nasdaq::price4_to_price(m.upper_price_range_collar.get()) == px("16.5"));
  }
}

TEST_CASE("codecs.itch: the encoder reproduces the fixture bytes") {
  ItchEncoder enc;
  enc.set_tracking_number(3);
  std::array<std::byte, 64> buf{};
  auto same = [&](std::size_t n, const char* name) {
    const Bytes& want = fx(name);
    REQUIRE_MESSAGE(n == want.size(), name);
    CHECK_MESSAGE(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)) == want, name);
  };
  same(enc.system_event(buf, kTs, 'O'), "S_system_event");
  same(enc.stock_directory(buf, kLocate, kTs, "AAPL"), "R_stock_directory");
  same(enc.add_order(buf, kLocate, kTs, 1001, Side::Buy, qt(300), "AAPL", px("189.1234")),
       "A_add_order");
  same(enc.add_order_mpid(
           buf, kLocate, kTs, 1002, Side::Sell, qt(200), "AAPL", px("189.25"), "GSCO"),
       "F_add_order_mpid");
  same(enc.order_executed(buf, kLocate, kTs, 1001, qt(100), 5001), "E_order_executed");
  same(enc.order_executed_with_price(buf, kLocate, kTs, 1002, qt(50), 5002, px("189.24"), true),
       "C_order_executed_with_price");
  same(enc.order_cancel(buf, kLocate, kTs, 1001, qt(25)), "X_order_cancel");
  same(enc.order_delete(buf, kLocate, kTs, 1002), "D_order_delete");
  same(enc.order_replace(buf, kLocate, kTs, 1001, 1003, qt(400), px("189.10")), "U_order_replace");
  same(enc.trade(buf, kLocate, kTs, Side::Buy, qt(10), "AAPL", px("189.15"), 5003), "P_trade");
  same(enc.cross_trade(buf, kLocate, kTs, qt(123'456), "AAPL", px("189.20"), 5004, 'O'),
       "Q_cross_trade");
  same(enc.broken_trade(buf, kLocate, kTs, 5003), "B_broken_trade");
  {
    RetailInterest m{};
    REQUIRE(enc.header(m.hdr, 'N', kLocate, kTs));
    nasdaq::put_alpha(m.stock, sizeof m.stock, "AAPL");
    m.interest_flag = 'A';
    same(ItchEncoder::write(buf, m), "N_retail_interest");
  }
}

TEST_CASE("codecs.itch: the encoder refuses values it cannot represent exactly") {
  ItchEncoder enc;
  std::array<std::byte, 64> buf{};
  CHECK(enc.add_order(buf, 1, nasdaq::kMaxBe48 + 1, 1, Side::Buy, qt(1), "X", px("1")) == 0);
  CHECK(enc.add_order(buf, 1, 1, 1, Side::Buy, qt(1), "X", px("1.00001")) == 0);  // 5 decimals
  CHECK(enc.add_order(buf, 1, 1, 1, Side::Buy, qt(1), "X", px("-1")) == 0);
  CHECK(enc.add_order(buf, 1, 1, 1, Side::Buy, Qty::from_decimal("1.5").value(), "X", px("1")) ==
        0);
  CHECK(enc.add_order(
            std::span<std::byte>(buf).first(35), 1, 1, 1, Side::Buy, qt(1), "X", px("1")) == 0);
  CHECK(enc.add_order(buf, 1, nasdaq::kMaxBe48, 1, Side::Buy, qt(1), "X", px("214748.3647")) == 36);
}

TEST_CASE("codecs.itch: field helpers convert exactly") {
  CHECK(nasdaq::price4_to_price(std::uint32_t{1}) == px("0.0001"));
  CHECK(nasdaq::price4_to_price(std::uint32_t{0x7FFFFFFF}) == px("214748.3647"));
  CHECK(nasdaq::price4_to_price(std::uint32_t{0xFFFFFFFF}) == px("429496.7295"));
  std::uint32_t p4 = 0;
  CHECK(nasdaq::price_to_price4(px("189.1234"), p4));
  CHECK(p4 == 1'891'234);
  CHECK_FALSE(nasdaq::price_to_price4(px("0.00005"), p4));
  CHECK_FALSE(nasdaq::price_to_price4(px("429496.7296"), p4));
  std::uint64_t p8 = 0;
  CHECK(nasdaq::price_to_price8(px("0.00000001"), p8));
  CHECK(p8 == 1);
  Price p;
  CHECK_FALSE(nasdaq::price8_to_price(0x8000'0000'0000'0000ULL, p));
  Qty q;
  CHECK(nasdaq::shares_to_qty(std::uint64_t{92'233'720'368}, q));
  CHECK_FALSE(nasdaq::shares_to_qty(std::uint64_t{92'233'720'369}, q));
  nasdaq::be48_t t{};
  t.set(nasdaq::kMaxBe48);
  CHECK(t.get() == nasdaq::kMaxBe48);
  t.set(kTs);
  CHECK(t.get() == kTs);
  char num[20];
  CHECK(nasdaq::put_numeric(num, sizeof num, 42));
  CHECK(std::string_view(num, 20) == "                  42");
  std::uint64_t v = 0;
  CHECK(nasdaq::get_numeric(num, sizeof num, v));
  CHECK(v == 42);
  CHECK(nasdaq::get_numeric("42  ", 4, v));
  CHECK(v == 42);
  CHECK(nasdaq::get_numeric("    ", 4, v));
  CHECK(v == 0);
  CHECK_FALSE(nasdaq::get_numeric("4 2 ", 4, v));
  CHECK_FALSE(nasdaq::put_numeric(num, 2, 123));
  CHECK(nasdaq::symbol_key("AAPL") == nasdaq::symbol_key8("AAPL    "));
  CHECK(nasdaq::symbol_key("AAPL") == nasdaq::symbol_key("AAPL    "));
  CHECK(nasdaq::symbol_key("AAPL") != nasdaq::symbol_key("AAPLX"));
}

TEST_CASE("codecs.itch: the decoder maps order messages to L3 events") {
  RecordingSink rec;
  ItchDecoder dec(VenueId{3});
  REQUIRE(dec.add_symbol("AAPL", InstrumentId{4}));
  const Timestamp midnight{1'700'000'000'000'000'000};
  dec.set_midnight(midnight);
  dec.set_venue_seq(77);
  const std::int64_t rx = 42;

  CHECK(dec.decode(codecs::test::frame_of(fx("R_stock_directory")), rx, rec.sink) ==
        ParseStatus::Ignored);
  CHECK(dec.instrument(kLocate) == InstrumentId{4});
  CHECK(dec.stats().directory_mapped == 1);

  const char* events[] = {"A_add_order",
                          "F_add_order_mpid",
                          "E_order_executed",
                          "C_order_executed_with_price",
                          "X_order_cancel",
                          "D_order_delete",
                          "U_order_replace",
                          "P_trade",
                          "Q_cross_trade"};
  for (const char* name : events)
    CHECK_MESSAGE(dec.decode(codecs::test::frame_of(fx(name)), rx, rec.sink) == ParseStatus::Ok,
                  name);
  const char* ignored[] = {"S_system_event",
                           "H_stock_trading_action",
                           "Y_reg_sho_restriction",
                           "L_market_participant_position",
                           "V_mwcb_decline_level",
                           "W_mwcb_status",
                           "K_ipo_quoting_period_update",
                           "J_luld_auction_collar",
                           "h_operational_halt",
                           "B_broken_trade",
                           "I_noii",
                           "N_retail_interest",
                           "O_dlcr_price_discovery"};
  for (const char* name : ignored)
    CHECK_MESSAGE(
        dec.decode(codecs::test::frame_of(fx(name)), rx, rec.sink) == ParseStatus::Ignored, name);

  const std::vector<Bytes> out = rec.drain();
  REQUIRE(out.size() == 9);
  for (const Bytes& raw : out) {
    const auto h = RecordingSink::as<EventHeader>(raw);
    CHECK(h.instrument == InstrumentId{4});
    CHECK(h.venue == VenueId{3});
    CHECK(h.venue_seq == 77);
    CHECK(h.exch_ts.ns == midnight.ns + static_cast<std::int64_t>(kTs));
    CHECK(h.recv_ts.ns == rx);
    CHECK(h.len % 64 == 0);
  }
  {
    const auto m = RecordingSink::as<OrderAddL3Msg>(out[0]);
    CHECK(m.hdr.type == EventType::OrderAddL3);
    CHECK(m.order_ref == 1001);
    CHECK(m.side == Side::Buy);
    CHECK(m.qty == qt(300));
    CHECK(m.price == px("189.1234"));
  }
  {
    const auto m = RecordingSink::as<OrderAddL3Msg>(out[1]);
    CHECK(m.order_ref == 1002);
    CHECK(m.side == Side::Sell);
    CHECK(m.price == px("189.25"));
  }
  {
    const auto m = RecordingSink::as<OrderExecL3Msg>(out[2]);
    CHECK(m.hdr.type == EventType::OrderExecL3);
    CHECK(m.order_ref == 1001);
    CHECK(m.exec_qty == qt(100));
    CHECK(m.exec_price.is_zero());  // E executes at the order's own price
    CHECK(m.match_id == 5001);
    CHECK(m.printable());
  }
  {
    const auto m = RecordingSink::as<OrderExecL3Msg>(out[3]);
    CHECK(m.exec_price == px("189.24"));
    CHECK(m.match_id == 5002);
    CHECK(m.printable());  // Printable 'Y'
    CHECK(m.exec_flags == 0);
  }
  {
    const auto m = RecordingSink::as<OrderCancelL3Msg>(out[4]);
    CHECK(m.hdr.type == EventType::OrderCancelL3);
    CHECK(m.canceled_qty == qt(25));
  }
  {
    const auto m = RecordingSink::as<OrderCancelL3Msg>(out[5]);
    CHECK(m.order_ref == 1002);
    CHECK(m.canceled_qty.is_zero());  // delete
  }
  {
    const auto m = RecordingSink::as<OrderReplaceL3Msg>(out[6]);
    CHECK(m.old_order_ref == 1001);
    CHECK(m.new_order_ref == 1003);
    CHECK(m.qty == qt(400));
    CHECK(m.price == px("189.1"));
  }
  {
    const auto m = RecordingSink::as<TradeMsg>(out[7]);
    CHECK(m.hdr.type == EventType::Trade);
    CHECK(m.qty == qt(10));
    CHECK(m.price == px("189.15"));
    CHECK(m.trade_id == 5003);
  }
  {
    const auto m = RecordingSink::as<TradeMsg>(out[8]);
    CHECK(m.qty == qt(123'456));
    CHECK(m.price == px("189.2"));
    CHECK(m.trade_id == 5004);
  }
  CHECK(dec.stats().events == 9);
  CHECK(dec.stats().messages == 23);

  // C with Printable 'N' keeps the flag.
  ItchEncoder enc;
  std::array<std::byte, 64> buf{};
  const std::size_t n =
      enc.order_executed_with_price(buf, kLocate, kTs, 1003, qt(5), 5005, px("189.3"), false);
  REQUIRE(n != 0);
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(buf.data(), n)),
                   rx,
                   rec.sink) == ParseStatus::Ok);
  const std::vector<Bytes> c = rec.drain();
  REQUIRE(c.size() == 1);
  const auto m = RecordingSink::as<OrderExecL3Msg>(c[0]);
  CHECK(m.exec_flags == OrderExecL3Msg::kNonPrintable);
  CHECK_FALSE(m.printable());
  CHECK(m.exec_price == px("189.3"));
}

TEST_CASE("codecs.itch: unknown locates, unknown types and malformed messages") {
  RecordingSink rec;
  ItchDecoder dec;
  const Bytes& add = fx("A_add_order");
  CHECK(dec.decode(codecs::test::frame_of(add), 0, rec.sink) == ParseStatus::Ignored);
  CHECK(dec.stats().unknown_locate == 1);

  dec.map_locate(kLocate, InstrumentId{1});
  CHECK(dec.decode(codecs::test::frame_of(add), 0, rec.sink) == ParseStatus::Ok);
  dec.clear_locates();
  CHECK_FALSE(dec.instrument(kLocate).valid());
  dec.map_locate(kLocate, InstrumentId{1});

  const Bytes unknown = {std::byte{'z'}, std::byte{0}};
  CHECK(dec.decode(codecs::test::frame_of(unknown), 0, rec.sink) == ParseStatus::Ignored);
  CHECK(dec.stats().unknown_type == 1);

  CHECK(dec.decode(FrameView{}, 0, rec.sink) == ParseStatus::Malformed);
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(add).first(35)),
                   0,
                   rec.sink) == ParseStatus::Malformed);
  Bytes bad_side = add;
  bad_side[19] = std::byte{'X'};
  CHECK(dec.decode(codecs::test::frame_of(bad_side), 0, rec.sink) == ParseStatus::Malformed);
  Bytes zero_cancel = fx("X_order_cancel");
  std::fill(zero_cancel.begin() + 19, zero_cancel.end(), std::byte{0});
  CHECK(dec.decode(codecs::test::frame_of(zero_cancel), 0, rec.sink) == ParseStatus::Ignored);
  Bytes zero_cross = fx("Q_cross_trade");
  std::fill(zero_cross.begin() + 11, zero_cross.begin() + 19, std::byte{0});
  CHECK(dec.decode(codecs::test::frame_of(zero_cross), 0, rec.sink) == ParseStatus::Ignored);
  CHECK(dec.stats().malformed == 3);
  CHECK_FALSE(dec.add_symbol("TOOLONGSYM", InstrumentId{2}));
  CHECK_FALSE(dec.add_symbol("", InstrumentId{2}));
}

TEST_CASE("codecs.itch: a full sink reports overflow") {
  RecordingSink rec(512);
  ItchDecoder dec;
  dec.map_locate(kLocate, InstrumentId{0});
  std::size_t ok = 0;
  ParseStatus last = ParseStatus::Ok;
  for (int i = 0; i < 16 && last == ParseStatus::Ok; ++i) {
    last = dec.decode(codecs::test::frame_of(fx("A_add_order")), 0, rec.sink);
    if (last == ParseStatus::Ok) ++ok;
  }
  CHECK(last == ParseStatus::Overflow);
  CHECK(dec.stats().overflow == 1);
  CHECK(rec.drain().size() == ok);
}
