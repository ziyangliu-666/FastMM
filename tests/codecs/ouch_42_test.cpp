// OUCH 4.2: layouts against the spec-derived fixtures, encoder, decoder and host builders.
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/ouch/ouch42.hpp"

#include <array>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::ouch42;
using fastmm::codecs::test::Bytes;
using fastmm::codecs::test::RecordingSink;

namespace {
constexpr std::uint64_t kTs = 34'200'000'000'456ULL;
constexpr InstrumentId kAapl{4};
const ClientOrderId kId1{0x0001'0000'0001ULL};  // "fm000100000001"
const ClientOrderId kId2{0x0001'0000'0002ULL};
const ClientOrderId kId3{0x0001'0000'0003ULL};

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
const std::map<std::string, Bytes>& fixtures() {
  static const std::map<std::string, Bytes> f =
      codecs::test::load_hex_fixture("ouch42_messages.hex");
  return f;
}
const Bytes& fx(const char* name) {
  const auto it = fixtures().find(name);
  REQUIRE_MESSAGE(it != fixtures().end(), name);
  return it->second;
}
template <class M>
M view(const Bytes& b) {
  REQUIRE(b.size() == sizeof(M));
  M m;
  std::memcpy(&m, b.data(), sizeof m);
  return m;
}
venues::OrderCommand new_order(ClientOrderId id, Side side, const char* price, std::int64_t qty) {
  venues::OrderCommand c;
  c.kind = venues::OrderCommandKind::New;
  c.instrument = kAapl;
  c.cl_ord_id = id;
  c.side = side;
  c.price = px(price);
  c.qty = Qty::from_int(qty);
  return c;
}
Bytes out_of(const std::array<std::byte, 128>& buf, std::size_t n) {
  return Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
}
}  // namespace

TEST_CASE("codecs.ouch: 4.2 fixture lengths match the message tables") {
  for (const auto& [name, raw] : fixtures()) {
    const auto type = static_cast<char>(raw[0]);
    const bool inbound = name == "O_enter_order" || name == "O_enter_order_fok_post_only" ||
                         name == "U_replace_order" || name == "X_cancel_order" ||
                         name == "M_modify_order";
    CHECK_MESSAGE((inbound ? inbound_length(type) : outbound_length(type)) == raw.size(), name);
  }
  CHECK(outbound_length('Z') == 0);
  CHECK(inbound_length('A') == 0);
}

TEST_CASE("codecs.ouch: 4.2 order tokens are the 14-character client order ids") {
  char tok[14];
  put_token(tok, kId1);
  CHECK(std::string_view(tok, 14) == "fm000100000001");
  CHECK(token_to_cl_ord_id(tok) == kId1);
  CHECK_FALSE(token_to_cl_ord_id("ABCDEFGHIJKLMN").has_value());
  CHECK_FALSE(token_to_cl_ord_id("fm00000000000G").has_value());
}

TEST_CASE("codecs.ouch: 4.2 encoder reproduces the fixture bytes") {
  OuchEncoder enc;
  REQUIRE(enc.add_symbol("AAPL", kAapl));
  std::array<std::byte, 128> buf{};

  venues::OrderCommand n1 = new_order(kId1, Side::Buy, "189.1234", 100);
  CHECK(out_of(buf, enc.encode(n1, buf)) == fx("O_enter_order"));

  venues::OrderCommand n2 = new_order(kId2, Side::Sell, "189.25", 50);
  n2.tif = TimeInForce::Fok;
  n2.type = OrderType::PostOnly;
  CHECK(out_of(buf, enc.encode(n2, buf)) == fx("O_enter_order_fok_post_only"));

  venues::OrderCommand r;
  r.kind = venues::OrderCommandKind::Replace;
  r.cl_ord_id = kId3;
  r.orig_cl_ord_id = kId1;
  r.price = px("189.20");
  r.qty = Qty::from_int(150);
  CHECK(out_of(buf, enc.encode(r, buf)) == fx("U_replace_order"));

  venues::OrderCommand c;
  c.kind = venues::OrderCommandKind::Cancel;
  c.cl_ord_id = kId3;
  CHECK(out_of(buf, enc.encode(c, buf)) == fx("X_cancel_order"));
  CHECK(enc.stats().encoded == 4);

  CHECK(OuchEncoder::time_in_force(TimeInForce::Gtc) == kTifSystemHours);
  CHECK(OuchEncoder::time_in_force(TimeInForce::Day) == kTifMarketHours);
  CHECK(OuchEncoder::time_in_force(TimeInForce::Ioc) == kTifImmediateOrCancel);
}

TEST_CASE("codecs.ouch: 4.2 encoder refuses what OUCH cannot express") {
  OuchEncoder enc;
  REQUIRE(enc.add_symbol("AAPL", kAapl));
  std::array<std::byte, 128> buf{};
  venues::OrderCommand m = new_order(kId1, Side::Buy, "189", 100);
  m.type = OrderType::Market;
  CHECK(enc.encode(m, buf) == 0);
  CHECK(enc.stats().unsupported == 1);
  venues::OrderCommand u = new_order(kId1, Side::Buy, "189", 100);
  u.instrument = InstrumentId{99};
  CHECK(enc.encode(u, buf) == 0);
  CHECK(enc.stats().unknown_instrument == 1);
  CHECK(enc.encode(new_order(kId1, Side::Buy, "189.00001", 100), buf) == 0);
  CHECK(enc.encode(new_order(kId1, Side::Buy, "199999.9901", 100), buf) == 0);
  CHECK(enc.encode(new_order(kId1, Side::Buy, "189", 1'000'000), buf) == 0);
  CHECK(enc.encode(new_order(kId1, Side::Buy, "189", 0), buf) == 0);
  CHECK(enc.stats().bad_value == 4);
  CHECK(enc.encode(new_order(kId1, Side::Buy, "199999.99", 999'999), buf) == sizeof(EnterOrder));
  CHECK(enc.encode(new_order(kId1, Side::Buy, "189", 1), std::span<std::byte>(buf).first(48)) == 0);
  CHECK(enc.stats().buffer_too_small == 1);
}

TEST_CASE("codecs.ouch: 4.2 host builders reproduce the fixture bytes") {
  std::array<std::byte, 128> buf{};
  const auto enter = view<EnterOrder>(fx("O_enter_order"));
  const auto replace = view<ReplaceOrder>(fx("U_replace_order"));
  CHECK(out_of(buf, host::system_event(buf, kTs, 'S')) == fx("S_system_event"));
  CHECK(out_of(buf, host::accepted(buf, kTs, enter, 100, 7777, 'L')) == fx("A_accepted"));
  CHECK(out_of(buf, host::replaced(buf, kTs, replace, enter, 150, 7778, 'L')) == fx("U_replaced"));
  CHECK(out_of(buf, host::executed(buf, kTs, "fm000100000003", 40, 1'892'000, 'A', 9001)) ==
        fx("E_executed"));
  CHECK(out_of(buf, host::canceled(buf, kTs, "fm000100000003", 110, 'U')) == fx("C_canceled"));
  CHECK(out_of(buf, host::rejected(buf, kTs, "fm000100000002", 'X')) == fx("J_rejected"));
  CHECK(out_of(buf, host::cancel_reject(buf, kTs, "fm000100000003")) == fx("I_cancel_reject"));
  CHECK(host::accepted(std::span<std::byte>(buf).first(65), kTs, enter, 100, 1, 'L') == 0);
}

TEST_CASE("codecs.ouch: 4.2 decoder turns an order's life into engine events") {
  RecordingSink rec;
  OuchDecoder dec(VenueId{2});
  REQUIRE(dec.add_symbol("AAPL", kAapl));
  const Timestamp midnight{1'700'000'000'000'000'000};
  dec.set_midnight(midnight);
  dec.set_venue_seq(11);
  auto feed = [&](const char* name) {
    return dec.decode(codecs::test::frame_of(fx(name)), 5, rec.sink);
  };

  CHECK(feed("S_system_event") == ParseStatus::Ignored);
  CHECK(dec.stats().last_system_event == 'S');
  CHECK(feed("A_accepted") == ParseStatus::Ok);
  REQUIRE(dec.order(kId1) != nullptr);
  CHECK(feed("U_replaced") == ParseStatus::Ok);  // token1 -> token3
  CHECK(dec.order(kId1) == nullptr);
  CHECK(feed("E_executed") == ParseStatus::Ok);                       // 40 of 150
  CHECK(feed("G_executed_with_reference_price") == ParseStatus::Ok);  // 5 more
  CHECK(feed("D_aiq_canceled") == ParseStatus::Ignored);              // -10, still open
  CHECK(dec.stats().partial_cancels == 1);
  REQUIRE(dec.order(kId3) != nullptr);
  CHECK(dec.order(kId3)->leaves == Qty::from_int(95));
  CHECK(feed("M_order_modified") == ParseStatus::Ignored);  // "shares outstanding" 100
  CHECK(dec.order(kId3)->leaves == Qty::from_int(100));
  CHECK(feed("I_cancel_reject") == ParseStatus::Ok);
  CHECK(feed("P_cancel_pending") == ParseStatus::Ignored);
  CHECK(feed("T_order_priority_update") == ParseStatus::Ignored);
  CHECK(feed("B_broken_trade") == ParseStatus::Ignored);
  CHECK(feed("C_canceled") == ParseStatus::Ok);  // 110 >= leaves: done
  CHECK(dec.order(kId3) == nullptr);
  CHECK(feed("J_rejected") == ParseStatus::Ok);
  CHECK(dec.open_orders() == 0);

  const std::vector<Bytes> out = rec.drain();
  REQUIRE(out.size() == 7);
  {
    const auto m = RecordingSink::as<OrderAckMsg>(out[0]);
    CHECK(m.hdr.type == EventType::OrderAck);
    CHECK(m.hdr.instrument == kAapl);
    CHECK(m.hdr.venue == VenueId{2});
    CHECK(m.hdr.venue_seq == 11);
    CHECK(m.hdr.exch_ts.ns == midnight.ns + static_cast<std::int64_t>(kTs));
    CHECK(m.hdr.recv_ts.ns == 5);
    CHECK(m.cl_ord_id == kId1);
    CHECK(m.venue_order_id.view() == "7777");
  }
  {
    const auto m = RecordingSink::as<OrderAckMsg>(out[1]);
    CHECK(m.cl_ord_id == kId3);  // the OMS completes the replace on the pending id
    CHECK(m.venue_order_id.view() == "7778");
  }
  {
    const auto m = RecordingSink::as<OrderFillMsg>(out[2]);
    CHECK(m.hdr.type == EventType::OrderFill);
    CHECK(m.cl_ord_id == kId3);
    CHECK(m.qty == Qty::from_int(40));
    CHECK(m.price == px("189.20"));
    CHECK(m.cum_qty == Qty::from_int(40));
    CHECK(m.leaves_qty == Qty::from_int(110));
    CHECK(m.side == Side::Buy);
    CHECK(m.liquidity == Liquidity::Maker);  // 'A'
    CHECK(m.exec_id.view() == "9001");
    CHECK(m.venue_order_id.view() == "7778");
  }
  {
    const auto m = RecordingSink::as<OrderFillMsg>(out[3]);
    CHECK(m.cum_qty == Qty::from_int(45));
    CHECK(m.liquidity == Liquidity::Taker);  // 'R'
  }
  {
    const auto m = RecordingSink::as<OrderCancelRejectMsg>(out[4]);
    CHECK(m.hdr.type == EventType::OrderCancelReject);
    CHECK(m.cl_ord_id == kId3);
  }
  {
    const auto m = RecordingSink::as<OrderCancelAckMsg>(out[5]);
    CHECK(m.hdr.type == EventType::OrderCancelAck);
    CHECK(m.cl_ord_id == kId3);
    CHECK(m.cum_qty == Qty::from_int(45));
  }
  {
    const auto m = RecordingSink::as<OrderRejectMsg>(out[6]);
    CHECK(m.hdr.type == EventType::OrderReject);
    CHECK(m.cl_ord_id == kId2);
    CHECK(m.reason == RejectReason::VenueReject);
    CHECK(m.venue_code == 'X');
    CHECK(m.text.view() == "Invalid price");
  }
}

TEST_CASE("codecs.ouch: 4.2 dead orders, IOC remainders and malformed input") {
  RecordingSink rec;
  OuchDecoder dec;
  std::array<std::byte, 128> buf{};
  auto enter = view<EnterOrder>(fx("O_enter_order"));

  std::size_t n = host::accepted(buf, kTs, enter, 100, 1, 'D');  // accepted and auto-canceled
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(buf.data(), n)),
                   0,
                   rec.sink) == ParseStatus::Ok);
  n = host::accepted(buf, kTs, enter, 100, 2, 'L');
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(buf.data(), n)),
                   0,
                   rec.sink) == ParseStatus::Ok);
  n = host::canceled(buf, kTs, "fm000100000001", 100, 'I');
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(buf.data(), n)),
                   0,
                   rec.sink) == ParseStatus::Ok);
  n = host::canceled(buf, kTs, "fm000100000001", 100, 'U');  // already gone
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(buf.data(), n)),
                   0,
                   rec.sink) == ParseStatus::Ignored);
  CHECK(dec.stats().unknown_order == 1);
  n = host::executed(buf, kTs, "NOTOURTOKEN123", 1, 1, 'A', 1);
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(buf.data(), n)),
                   0,
                   rec.sink) == ParseStatus::Ignored);
  CHECK(dec.stats().foreign_id == 1);

  const std::vector<Bytes> out = rec.drain();
  REQUIRE(out.size() == 4);
  CHECK(RecordingSink::type_of(out[0]) == EventType::OrderAck);
  CHECK(RecordingSink::type_of(out[1]) == EventType::OrderExpired);
  CHECK(RecordingSink::type_of(out[2]) == EventType::OrderAck);
  CHECK(RecordingSink::type_of(out[3]) == EventType::OrderExpired);  // IOC reason
  CHECK_FALSE(
      RecordingSink::as<OrderAckMsg>(out[0]).hdr.instrument.valid());  // no symbol registered

  const Bytes& acc = fx("A_accepted");
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(acc).first(65)),
                   0,
                   rec.sink) == ParseStatus::Malformed);
  CHECK(dec.decode(FrameView{}, 0, rec.sink) == ParseStatus::Malformed);
  const Bytes unknown = {std::byte{'Z'}};
  CHECK(dec.decode(codecs::test::frame_of(unknown), 0, rec.sink) == ParseStatus::Ignored);
  CHECK(dec.stats().unknown_type == 1);
  Bytes bad_side = acc;
  bad_side[23] = std::byte{'Q'};
  CHECK(dec.decode(codecs::test::frame_of(bad_side), 0, rec.sink) == ParseStatus::Malformed);

  RecordingSink tiny(128);
  OuchDecoder dec2;
  n = host::accepted(buf, kTs, enter, 100, 3, 'L');
  const FrameView f = codecs::test::frame_of(std::span<const std::byte>(buf.data(), n));
  CHECK(dec2.decode(f, 0, tiny.sink) == ParseStatus::Ok);
  CHECK(dec2.decode(f, 0, tiny.sink) == ParseStatus::Overflow);
  CHECK(dec2.stats().overflow == 1);
}

TEST_CASE("codecs.ouch: liquidity flags") {
  CHECK(ouch::liquidity_from_flag('A') == Liquidity::Maker);
  CHECK(ouch::liquidity_from_flag('k') == Liquidity::Maker);
  CHECK(ouch::liquidity_from_flag('R') == Liquidity::Taker);
  CHECK(ouch::liquidity_from_flag('m') == Liquidity::Taker);
  CHECK(ouch::liquidity_from_flag('O') == Liquidity::Unknown);
  CHECK(ouch::liquidity_from_flag('C') == Liquidity::Unknown);
}
