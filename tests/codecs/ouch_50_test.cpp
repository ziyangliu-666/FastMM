// OUCH 5.0: layouts and appendages against the spec-derived fixtures, UserRefNum mapping,
// encoder, decoder and host builders.
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/ouch/ouch50.hpp"

#include <array>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::ouch50;
using fastmm::codecs::test::Bytes;
using fastmm::codecs::test::RecordingSink;

namespace {
constexpr std::uint64_t kTs = 34'200'000'000'456ULL;
constexpr InstrumentId kAapl{4};
const ClientOrderId kId1{0x0001'0000'0001ULL};
const ClientOrderId kId2{0x0001'0000'0002ULL};
const ClientOrderId kId3{0x0001'0000'0003ULL};
const ClientOrderId kId4{0x0001'0000'0004ULL};

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
const std::map<std::string, Bytes>& fixtures() {
  static const std::map<std::string, Bytes> f =
      codecs::test::load_hex_fixture("ouch50_messages.hex");
  return f;
}
const Bytes& fx(const char* name) {
  const auto it = fixtures().find(name);
  REQUIRE_MESSAGE(it != fixtures().end(), name);
  return it->second;
}
template <class M>
M view(const Bytes& b) {
  REQUIRE(b.size() >= sizeof(M));
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

TEST_CASE("codecs.ouch: 5.0 fixtures split into fixed part and appendage") {
  for (const auto& [name, raw] : fixtures()) {
    const auto type = static_cast<char>(raw[0]);
    const bool inbound =
        name.rfind("O_", 0) == 0 || name == "U_replace_order" || name == "X_cancel_order";
    std::span<const std::byte> app;
    if (inbound) {
      CHECK_MESSAGE(split_message(codecs::test::span_of(raw), inbound_base(type), false, app),
                    name);
    } else {
      const OutboundLayout l = outbound_layout(type);
      REQUIRE_MESSAGE(l.base != 0, name);
      if (l.has_appendage) {
        CHECK_MESSAGE(split_message(codecs::test::span_of(raw), l.base, l.appendage_optional, app),
                      name);
      } else {
        CHECK(raw.size() == l.base);
      }
    }
  }
  std::span<const std::byte> app;
  REQUIRE(split_message(codecs::test::span_of(fx("O_enter_order_fok_post_only")), 45, false, app));
  CHECK(app.size() == 9);
  std::span<const std::byte> v;
  REQUIRE(find_option(app, OptionTag::MinQty, v));
  REQUIRE(v.size() == 4);
  CHECK(nasdaq::load_be64(std::array<std::byte, 8>{
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, v[0], v[1], v[2], v[3]}
                              .data()) == 50);
  REQUIRE(find_option(app, OptionTag::PostOnly, v));
  CHECK(static_cast<char>(v[0]) == 'P');
  CHECK_FALSE(find_option(app, OptionTag::Firm, v));

  CHECK(split_message(codecs::test::span_of(fx("C_canceled_no_appendage")), 18, true, app));
  CHECK_FALSE(split_message(codecs::test::span_of(fx("C_canceled_no_appendage")), 18, false, app));
  Bytes too_long = fx("A_accepted");
  too_long[63] = std::byte{1};  // Appendage Length 1 but nothing follows
  CHECK_FALSE(split_message(codecs::test::span_of(too_long), 62, false, app));
  Bytes zero_tag = fx("A_accepted_with_appendage");
  zero_tag[64] = std::byte{0};  // TagValue Length 0
  CHECK_FALSE(split_message(codecs::test::span_of(zero_tag), 62, false, app));
  CHECK(outbound_layout('Z').base == 0);
  CHECK(inbound_base('A') == 0);
}

TEST_CASE("codecs.ouch: 5.0 UserRefNums are assigned strictly increasing") {
  UserRefMap ids(7);
  CHECK(ids.assign(kId1) == 7);
  CHECK(ids.assign(kId2) == 8);
  CHECK(ids.assign(kId1) == 7);  // benign resend keeps its number
  CHECK(ids.find(kId2) == 8);
  CHECK(ids.find(std::uint32_t{8}) == kId2);
  ids.erase(8);
  CHECK(ids.find(kId2) == 0);
  CHECK_FALSE(ids.find(std::uint32_t{8}).valid());
  ids.set_next(100);
  CHECK(ids.assign(kId3) == 100);
  ids.set_next(5);  // never backwards
  CHECK(ids.assign(kId4) == 101);
  CHECK(ids.size() == 3);
}

TEST_CASE("codecs.ouch: 5.0 encoder reproduces the fixture bytes") {
  UserRefMap ids;
  OuchEncoder enc(ids);
  REQUIRE(enc.add_symbol("AAPL", kAapl));
  std::array<std::byte, 128> buf{};

  CHECK(out_of(buf, enc.encode(new_order(kId1, Side::Buy, "189.1234", 100), buf)) ==
        fx("O_enter_order"));
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

  CHECK(OuchEncoder::time_in_force(TimeInForce::Gtc) == kTifDay);
  CHECK(OuchEncoder::time_in_force(TimeInForce::Ioc) == kTifIoc);

  // Firm option, unknown orders, market orders.
  EncoderConfig cfg;
  std::memcpy(cfg.firm, "FMMM", 4);
  UserRefMap ids2;
  OuchEncoder firm(ids2, cfg);
  REQUIRE(firm.add_symbol("AAPL", kAapl));
  const std::size_t n = firm.encode(new_order(kId1, Side::Buy, "1", 1), buf);
  REQUIRE(n == sizeof(EnterOrder) + 6);
  std::span<const std::byte> app;
  REQUIRE(split_message(std::span<const std::byte>(buf.data(), n), 45, false, app));
  std::span<const std::byte> v;
  REQUIRE(find_option(app, OptionTag::Firm, v));
  CHECK(codecs::test::to_string(v) == "FMMM");
  c.cl_ord_id = kId4;
  CHECK(firm.encode(c, buf) == 0);
  CHECK(firm.stats().unknown_order == 1);
  r.orig_cl_ord_id = kId4;
  CHECK(firm.encode(r, buf) == 0);
  venues::OrderCommand mkt = new_order(kId2, Side::Buy, "1", 1);
  mkt.type = OrderType::Market;
  CHECK(firm.encode(mkt, buf) == 0);
  CHECK(firm.encode(new_order(kId2, Side::Buy, "1.00001", 1), buf) == 0);
  CHECK(ids2.size() == 1);  // refused commands do not consume UserRefNums
}

TEST_CASE("codecs.ouch: 5.0 host builders reproduce the fixture bytes") {
  std::array<std::byte, 128> buf{};
  const auto enter = view<EnterOrder>(fx("O_enter_order"));
  const auto replace = view<ReplaceOrder>(fx("U_replace_order"));
  CHECK(out_of(buf, host::system_event(buf, kTs, 'S')) == fx("S_system_event"));
  CHECK(out_of(buf, host::accepted(buf, kTs, enter, 100, 7777, 'L')) == fx("A_accepted"));
  CHECK(out_of(buf, host::replaced(buf, kTs, replace, enter, 150, 7778, 'L')) == fx("U_replaced"));
  CHECK(out_of(buf, host::executed(buf, kTs, 3, 40, 1'892'000, 'R', 9001)) == fx("E_executed"));
  CHECK(out_of(buf, host::canceled(buf, kTs, 3, 100, 'U')) == fx("C_canceled"));
  CHECK(out_of(buf, host::rejected(buf, kTs, 4, 0x001D, "fm000100000004")) == fx("J_rejected"));
  CHECK(out_of(buf, host::cancel_reject(buf, kTs, 3)) == fx("I_cancel_reject"));
  CHECK(out_of(buf, host::account_query_response(buf, kTs, 42)) == fx("Q_account_query_response"));
}

TEST_CASE("codecs.ouch: 5.0 host parsers read what the encoder writes") {
  const Bytes& e = fx("O_enter_order");
  host::EnterView ev;
  REQUIRE(host::parse_enter(codecs::test::span_of(e), ev));
  CHECK(ev.user_ref_num == 1);
  CHECK(ev.side == Side::Buy);
  CHECK(ev.qty == Qty::from_int(100));
  CHECK(ev.price == px("189.1234"));
  CHECK(ev.symbol == "AAPL");
  CHECK(ev.tif == TimeInForce::Gtc);
  CHECK_FALSE(ev.post_only);
  CHECK(ev.seq_token == 0);  // "fm" + hex: not a sequence token

  const Bytes& fok = fx("O_enter_order_fok_post_only");
  REQUIRE(host::parse_enter(codecs::test::span_of(fok), ev));
  CHECK(ev.side == Side::Sell);
  CHECK(ev.tif == TimeInForce::Fok);
  CHECK(ev.min_qty == 50);
  CHECK(ev.post_only);

  host::ReplaceView rv;
  REQUIRE(host::parse_replace(codecs::test::span_of(fx("U_replace_order")), rv));
  CHECK(rv.orig_user_ref_num == 1);
  CHECK(rv.user_ref_num == 3);
  CHECK(rv.qty == Qty::from_int(150));
  CHECK(rv.price == px("189.20"));
  host::CancelView cv;
  REQUIRE(host::parse_cancel(codecs::test::span_of(fx("X_cancel_order")), cv));
  CHECK(cv.user_ref_num == 3);
  CHECK(cv.quantity == 0);

  // Wrong type, truncated, zero quantity.
  CHECK_FALSE(host::parse_replace(codecs::test::span_of(e), rv));
  CHECK_FALSE(host::parse_enter(std::span<const std::byte>(e.data(), 20), ev));
  Bytes zero = e;
  auto m = view<EnterOrder>(zero);
  m.quantity.set(0);
  std::memcpy(zero.data(), &m, sizeof m);
  CHECK_FALSE(host::parse_enter(codecs::test::span_of(zero), ev));
}

TEST_CASE("codecs.ouch: 5.0 venue_seq token in ClOrdID") {
  char f[14];
  REQUIRE(put_seq_token(f, 12345));
  CHECK(std::string_view(f, 14) == "T0000000012345");
  CHECK(parse_seq_token(f) == 12345);
  REQUIRE(put_seq_token(f, kMaxSeqToken));
  CHECK(parse_seq_token(f) == kMaxSeqToken);
  CHECK_FALSE(put_seq_token(f, 0));
  CHECK_FALSE(put_seq_token(f, kMaxSeqToken + 1));
  CHECK(parse_seq_token("fm000100000004") == 0);
  CHECK(parse_seq_token("T00000000123 4") == 0);

  Bytes e = fx("O_enter_order");
  auto m = view<EnterOrder>(e);
  REQUIRE(put_seq_token(m.cl_ord_id, 987));
  std::memcpy(e.data(), &m, sizeof m);
  host::EnterView ev;
  REQUIRE(host::parse_enter(codecs::test::span_of(e), ev));
  CHECK(ev.seq_token == 987);
}

TEST_CASE("codecs.ouch: 5.0 decoder with the shared UserRefNum map") {
  UserRefMap ids;
  OuchEncoder enc(ids);
  REQUIRE(enc.add_symbol("AAPL", kAapl));
  std::array<std::byte, 128> buf{};
  REQUIRE(enc.encode(new_order(kId1, Side::Buy, "189.1234", 100), buf) != 0);  // urn 1
  REQUIRE(enc.encode(new_order(kId2, Side::Sell, "189.25", 50), buf) != 0);    // urn 2
  venues::OrderCommand r;
  r.kind = venues::OrderCommandKind::Replace;
  r.cl_ord_id = kId3;
  r.orig_cl_ord_id = kId1;
  r.price = px("189.20");
  r.qty = Qty::from_int(150);
  REQUIRE(enc.encode(r, buf) != 0);  // urn 3

  RecordingSink rec;
  OuchDecoder dec(&ids, VenueId{5});
  REQUIRE(dec.add_symbol("AAPL", kAapl));
  auto feed = [&](const char* name) {
    return dec.decode(codecs::test::frame_of(fx(name)), 9, rec.sink);
  };
  CHECK(feed("S_system_event") == ParseStatus::Ignored);
  CHECK(feed("A_accepted") == ParseStatus::Ok);
  CHECK(feed("A_accepted_with_appendage") == ParseStatus::Ok);
  CHECK(feed("U_replaced") == ParseStatus::Ok);
  CHECK(ids.find(std::uint32_t{1}) == ClientOrderId{});  // the replaced order is forgotten
  CHECK(feed("E_executed") == ParseStatus::Ok);
  CHECK(feed("C_canceled_no_appendage") == ParseStatus::Ignored);  // partial: 110 -> 100
  REQUIRE(dec.order(3) != nullptr);
  CHECK(dec.order(3)->leaves == Qty::from_int(100));
  CHECK(feed("M_order_modified") == ParseStatus::Ignored);
  CHECK(feed("T_order_priority_update") == ParseStatus::Ignored);
  CHECK(feed("R_order_restated") == ParseStatus::Ignored);
  CHECK(feed("B_broken_trade") == ParseStatus::Ignored);
  CHECK(feed("D_aiq_canceled") == ParseStatus::Ignored);  // partial again
  CHECK(feed("C_canceled") == ParseStatus::Ok);           // done
  CHECK(ids.find(kId3) == 0);
  CHECK(feed("I_cancel_reject") == ParseStatus::Ignored);  // urn 3 is gone
  CHECK(feed("J_rejected") == ParseStatus::Ok);            // urn 4 via the echoed ClOrdID
  CHECK(feed("Q_account_query_response") == ParseStatus::Ignored);
  CHECK(ids.next() == 42);
  CHECK(dec.open_orders() == 1);  // urn 2

  const std::vector<Bytes> out = rec.drain();
  REQUIRE(out.size() == 6);
  CHECK(RecordingSink::as<OrderAckMsg>(out[0]).cl_ord_id == kId1);
  CHECK(RecordingSink::as<OrderAckMsg>(out[0]).hdr.instrument == kAapl);
  CHECK(RecordingSink::as<OrderAckMsg>(out[1]).cl_ord_id == kId2);
  CHECK(RecordingSink::as<OrderAckMsg>(out[2]).cl_ord_id == kId3);
  CHECK(RecordingSink::as<OrderAckMsg>(out[2]).venue_order_id.view() == "7778");
  {
    const auto m = RecordingSink::as<OrderFillMsg>(out[3]);
    CHECK(m.cl_ord_id == kId3);
    CHECK(m.qty == Qty::from_int(40));
    CHECK(m.price == px("189.20"));
    CHECK(m.leaves_qty == Qty::from_int(110));
    CHECK(m.liquidity == Liquidity::Taker);
    CHECK(m.hdr.venue == VenueId{5});
  }
  {
    const auto m = RecordingSink::as<OrderCancelAckMsg>(out[4]);
    CHECK(m.cl_ord_id == kId3);
    CHECK(m.cum_qty == Qty::from_int(40));
  }
  {
    const auto m = RecordingSink::as<OrderRejectMsg>(out[5]);
    CHECK(m.cl_ord_id == kId4);
    CHECK(m.venue_code == 0x001D);
    CHECK(m.text.view() == "Invalid Price");
  }
}

TEST_CASE("codecs.ouch: 5.0 decoder without a map and malformed messages") {
  RecordingSink rec;
  OuchDecoder dec(nullptr);
  CHECK(dec.decode(codecs::test::frame_of(fx("A_accepted")), 0, rec.sink) == ParseStatus::Ok);
  CHECK(dec.decode(codecs::test::frame_of(fx("E_executed")), 0, rec.sink) == ParseStatus::Ignored);
  CHECK(dec.stats().unknown_order == 1);  // urn 3 was never accepted here
  const std::vector<Bytes> out = rec.drain();
  REQUIRE(out.size() == 1);
  CHECK(RecordingSink::as<OrderAckMsg>(out[0]).cl_ord_id == kId1);

  const Bytes& acc = fx("A_accepted");
  CHECK(
      dec.decode(codecs::test::frame_of(std::span<const std::byte>(acc).first(62)), 0, rec.sink) ==
      ParseStatus::Malformed);  // Appendage Length is mandatory on Accepted
  const Bytes& ex = fx("E_executed");
  CHECK(dec.decode(codecs::test::frame_of(std::span<const std::byte>(ex).first(34)), 0, rec.sink) ==
        ParseStatus::Malformed);
  const Bytes unknown = {std::byte{'Z'}, std::byte{0}};
  CHECK(dec.decode(codecs::test::frame_of(unknown), 0, rec.sink) == ParseStatus::Ignored);
  CHECK(dec.decode(FrameView{}, 0, rec.sink) == ParseStatus::Malformed);
  CHECK(dec.stats().malformed == 3);

  Bytes foreign = fx("J_rejected");
  std::memcpy(foreign.data() + 15, "NOT-OUR-ID-000", 14);
  CHECK(dec.decode(codecs::test::frame_of(foreign), 0, rec.sink) == ParseStatus::Ignored);
  CHECK(dec.stats().foreign_id == 1);
  CHECK(reject_reason_text(0x0040) == "Invalid AIQ");
  CHECK(reject_reason_text(0x7777) == "Unknown reject reason");
}
