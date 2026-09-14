// FixDecoder against fixtures (ExecutionReport, OrderCancelReject, W, X) and FixEncoder golden
// messages for NewOrderSingle, OrderCancelRequest and OrderCancelReplaceRequest.
#include "fix_test_util.hpp"

#include <memory>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::fix;
using namespace fastmm::codecs::fix::test;
using venues::OrderCommand;
using venues::ParseStatus;

namespace {

static_assert(Decoder<FixDecoder>);
static_assert(Encoder<FixEncoder>);

constexpr std::int64_t kRx = 1'789'344'000'500'000'000;
// 2026-09-14T08:00:00 == 1789372800 s since the epoch.
constexpr std::int64_t kFixtureTransactTime = 1'789'372'800'120'000'000;

ClientOrderId id_of(std::string_view s) {
  return decode_cl_ord_id(s).value();
}

struct DecodeHarness {
  std::unique_ptr<FixSymbolTable> symbols = std::make_unique<FixSymbolTable>(test_symbols());
  std::unique_ptr<FixDecoder> dec = std::make_unique<FixDecoder>(*symbols, VenueId{3});
  std::unique_ptr<RecordingSink> rec = std::make_unique<RecordingSink>();

  std::vector<std::vector<std::byte>> run(const std::string& fixture,
                                          ParseStatus expected = ParseStatus::Ok) {
    for (const std::string& m : fixture_messages(fixture)) {
      CHECK(dec->decode(frame_of(m), kRx, rec->sink) == expected);
    }
    return rec->drain();
  }
};

}  // namespace

TEST_CASE("codecs.fix.decoder: ExecutionReport New and Replaced become OrderAck") {
  DecodeHarness h;
  auto ev = h.run("exec_report_new.fix");
  REQUIRE(ev.size() == 1);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::OrderAck);
  const auto& ack = RecordingSink::as<OrderAckMsg>(ev[0]);
  CHECK(ack.hdr.len == sizeof(OrderAckMsg));
  CHECK(ack.cl_ord_id == id_of("fm000100000001"));
  CHECK(ack.venue_order_id == "1001");
  CHECK(ack.hdr.instrument == InstrumentId{0});
  CHECK(ack.hdr.venue == VenueId{3});
  CHECK(ack.hdr.venue_seq == 2);
  CHECK(ack.hdr.exch_ts.ns == kFixtureTransactTime);
  CHECK(ack.hdr.recv_ts.ns == kRx);

  ev = h.run("exec_report_replaced.fix");
  REQUIRE(ev.size() == 1);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::OrderAck);
  CHECK(RecordingSink::as<OrderAckMsg>(ev[0]).cl_ord_id == id_of("fm000100000004"));
  CHECK(RecordingSink::as<OrderAckMsg>(ev[0]).venue_order_id == "1003");
  CHECK(h.dec->stats().acks == 2);
}

TEST_CASE("codecs.fix.decoder: ExecutionReport Trade becomes OrderFill with ExecID dedup") {
  DecodeHarness h;
  auto ev = h.run("exec_report_fill.fix");
  REQUIRE(ev.size() == 2);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::OrderFill);
  const auto& f1 = RecordingSink::as<OrderFillMsg>(ev[0]);
  CHECK(f1.hdr.len == sizeof(OrderFillMsg));
  CHECK(f1.cl_ord_id == id_of("fm000100000001"));
  CHECK(f1.venue_order_id == "1001");
  CHECK(f1.exec_id == "E2");
  CHECK(f1.qty == Qty::from_decimal("0.2").value());
  CHECK(f1.price == Price::from_decimal("70000.5").value());
  CHECK(f1.cum_qty == Qty::from_decimal("0.2").value());
  CHECK(f1.leaves_qty == Qty::from_decimal("0.3").value());
  CHECK(f1.side == Side::Buy);
  CHECK(f1.liquidity == Liquidity::Maker);
  CHECK(f1.fee.raw == 0);
  const auto& f2 = RecordingSink::as<OrderFillMsg>(ev[1]);
  CHECK(f2.price.raw == 6'999'912'345'678);
  CHECK(f2.qty == Qty::from_decimal("0.3").value());
  CHECK(f2.leaves_qty.raw == 0);
  CHECK(f2.liquidity == Liquidity::Taker);
  CHECK(h.dec->stats().fills == 2);

  // The same executions again (e.g. an application-level PossResend): dropped by ExecID.
  ev = h.run("exec_report_fill.fix", ParseStatus::Ignored);
  CHECK(ev.empty());
  CHECK(h.dec->stats().duplicate_fills == 2);
}

TEST_CASE("codecs.fix.decoder: Canceled, Rejected, Expired and pending ExecutionReports") {
  DecodeHarness h;
  auto ev = h.run("exec_report_canceled.fix");
  REQUIRE(ev.size() == 1);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::OrderCancelAck);
  const auto& c = RecordingSink::as<OrderCancelAckMsg>(ev[0]);
  CHECK(c.cl_ord_id == id_of("fm000100000002"));  // OrigClOrdID, not the cancel request id
  CHECK(c.cum_qty == Qty::from_decimal("1.25").value());
  CHECK(c.hdr.instrument == InstrumentId{1});

  ev = h.run("exec_report_rejected.fix");
  REQUIRE(ev.size() == 1);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::OrderReject);
  const auto& r = RecordingSink::as<OrderRejectMsg>(ev[0]);
  CHECK(r.cl_ord_id == id_of("fm000100000005"));
  CHECK(r.reason == RejectReason::DuplicateId);
  CHECK(r.venue_code == 6);
  CHECK(r.text == "Duplicate ClOrdID");

  ev = h.run("exec_report_expired.fix");
  REQUIRE(ev.size() == 1);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::OrderExpired);
  CHECK(RecordingSink::as<OrderExpiredMsg>(ev[0]).cl_ord_id == id_of("fm000100000006"));
  CHECK(RecordingSink::as<OrderExpiredMsg>(ev[0]).cum_qty == Qty::from_decimal("0.4").value());

  CHECK(h.run("exec_report_pending_new.fix", ParseStatus::Ignored).empty());
}

TEST_CASE("codecs.fix.decoder: OrderCancelReject for cancel and for cancel/replace") {
  DecodeHarness h;
  auto ev = h.run("cancel_reject.fix");
  REQUIRE(ev.size() == 2);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::OrderCancelReject);
  const auto& cr = RecordingSink::as<OrderCancelRejectMsg>(ev[0]);
  CHECK(cr.cl_ord_id == id_of("fm000100000001"));
  CHECK(cr.reason == RejectReason::VenueReject);
  CHECK(cr.venue_code == 0);
  CHECK(cr.text == "Too late to cancel");
  REQUIRE(RecordingSink::type_of(ev[1]) == EventType::OrderReject);
  const auto& rr = RecordingSink::as<OrderRejectMsg>(ev[1]);
  CHECK(rr.cl_ord_id == id_of("fm000100000009"));  // the replacement id
  CHECK(rr.reason == RejectReason::VenueUnknownOrder);
  CHECK(rr.venue_code == 1);
}

TEST_CASE("codecs.fix.decoder: MarketDataSnapshotFullRefresh becomes a BookSnapshot") {
  DecodeHarness h;
  const auto ev = h.run("md_snapshot.fix");
  REQUIRE(ev.size() == 1);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::BookSnapshot);
  const auto& b = RecordingSink::as<BookDeltaMsg>(ev[0]);
  CHECK(b.hdr.len == BookDeltaMsg::size_for(2, 1));
  CHECK(ev[0].size() == b.hdr.len);
  CHECK(b.is_snapshot());
  CHECK(b.hdr.instrument == InstrumentId{0});
  CHECK(b.last_update_id == 12);
  REQUIRE(b.bids().size() == 2);
  REQUIRE(b.asks().size() == 1);
  CHECK(b.bids()[0] == Level{Price::from_int(70000), Qty::from_decimal("1.5").value()});
  CHECK(b.bids()[1] == Level{Price::from_decimal("69999.5").value(), Qty::from_int(2)});
  CHECK(b.asks()[0] ==
        Level{Price::from_decimal("70000.5").value(), Qty::from_decimal("0.75").value()});
}

TEST_CASE("codecs.fix.decoder: MarketDataIncrementalRefresh becomes BookDelta and Trade") {
  DecodeHarness h;
  const auto ev = h.run("md_incremental.fix");
  REQUIRE(ev.size() == 3);
  REQUIRE(RecordingSink::type_of(ev[0]) == EventType::BookDelta);
  const auto& btc = RecordingSink::as<BookDeltaMsg>(ev[0]);
  CHECK(btc.hdr.instrument == InstrumentId{0});
  CHECK_FALSE(btc.is_snapshot());
  CHECK(btc.hdr.len == BookDeltaMsg::size_for(1, 1));
  REQUIRE(btc.bids().size() == 1);
  REQUIRE(btc.asks().size() == 1);
  CHECK(btc.bids()[0] == Level{Price::from_int(70000), Qty::from_int(1)});
  CHECK(btc.asks()[0] == Level{Price::from_int(70001), Qty{}});  // delete
  CHECK(btc.last_update_id == 13);

  REQUIRE(RecordingSink::type_of(ev[1]) == EventType::Trade);
  const auto& t = RecordingSink::as<TradeMsg>(ev[1]);
  CHECK(t.hdr.instrument == InstrumentId{1});
  CHECK(t.price == Price::from_decimal("3500.25").value());
  CHECK(t.qty == Qty::from_decimal("0.4").value());
  CHECK(t.trade_id == 987654);

  REQUIRE(RecordingSink::type_of(ev[2]) == EventType::BookDelta);
  const auto& eth = RecordingSink::as<BookDeltaMsg>(ev[2]);
  CHECK(eth.hdr.instrument == InstrumentId{1});
  REQUIRE(eth.bids().empty());
  REQUIRE(eth.asks().size() == 1);
  CHECK(eth.asks()[0] == Level{Price::from_int(3501), Qty::from_int(2)});
  CHECK(h.dec->stats().book_updates == 2);
  CHECK(h.dec->stats().trades == 1);
}

TEST_CASE("codecs.fix.decoder: malformed, foreign, unknown symbol and overflow") {
  DecodeHarness h;
  const std::string head = "49=VENUE|56=CLIENT|34=5|52=20260914-08:00:00.123|";
  auto decode = [&](const std::string& fields) {
    const std::string m = wrap(fields);
    return h.dec->decode(frame_of(m), kRx, h.rec->sink);
  };
  CHECK(decode("35=8|" + head + "37=1|11=fm000100000001|17=E1|39=0|") == ParseStatus::Malformed);
  CHECK(decode("35=8|" + head + "37=1|11=fm000100000001|17=E1|150=F|39=1|31=1|14=1|151=0|") ==
        ParseStatus::Malformed);  // LastQty missing
  CHECK(decode("35=8|" + head + "37=1|11=fm000100000001|150=F|39=1|32=1|31=1|14=1|151=0|") ==
        ParseStatus::Malformed);  // ExecID missing
  CHECK(decode("35=8|" + head + "37=1|11=ABC|17=E1|150=0|39=0|") == ParseStatus::Ignored);
  CHECK(h.dec->stats().foreign_ids == 1);
  CHECK(decode("35=W|" + head + "55=XRPUSDT|268=1|269=0|270=1|271=1|") ==
        ParseStatus::UnknownSymbol);
  CHECK(decode("35=X|" + head + "268=2|279=0|269=0|55=BTCUSDT|270=1|271=1|") ==
        ParseStatus::Malformed);  // NumInGroup says 2
  CHECK(decode("35=X|" + head + "268=1|279=1|55=BTCUSDT|270=1|271=1|") ==
        ParseStatus::Ignored);  // no MDEntryType: not a book entry
  CHECK(decode("35=0|" + head) == ParseStatus::Ignored);
  std::string bad = wrap("35=8|" + head + "37=1|11=fm000100000001|17=E1|150=0|39=0|");
  bad[bad.size() - 2] = bad[bad.size() - 2] == '1' ? '2' : '1';
  CHECK(h.dec->decode(frame_of(bad), kRx, h.rec->sink) == ParseStatus::Malformed);
  // Order events survive an unknown symbol (instrument left invalid).
  CHECK(decode("35=8|" + head + "37=1|11=fm000100000001|17=E1|150=0|39=0|55=XRPUSDT|") ==
        ParseStatus::Ok);
  auto ev = h.rec->drain();
  REQUIRE(ev.size() == 1);
  CHECK_FALSE(RecordingSink::as<OrderAckMsg>(ev[0]).hdr.instrument.valid());

  RecordingSink tiny(256);
  const std::string fill = fixture_messages("exec_report_fill.fix")[0];
  const std::string fill2 = fixture_messages("exec_report_fill.fix")[1];
  CHECK(h.dec->decode(frame_of(fill), kRx, tiny.sink) == ParseStatus::Ok);
  CHECK(h.dec->decode(frame_of(fill2), kRx, tiny.sink) == ParseStatus::Overflow);
  CHECK(h.dec->stats().overflow == 1);
}

TEST_CASE("codecs.fix.encoder: NewOrderSingle, OrderCancelRequest and OrderCancelReplaceRequest") {
  ManualClock clock;
  clock.ns = 1'789'374'615'250'000'000;  // 20260914-08:30:15.250
  const auto symbols = std::make_unique<FixSymbolTable>(test_symbols());
  Wire wire;
  FixSession session(initiator_config(), &Wire::send, &wire);
  session.set_clock(&ManualClock::read, &clock);
  FixEncoder enc(session, *symbols);
  std::vector<std::byte> buf(1024);
  auto encode = [&](const EventHeader& h) {
    const std::size_t n = enc.encode(command_of(h), buf);
    return std::string(reinterpret_cast<const char*>(buf.data()), n);
  };
  const std::string ts = "20260914-08:30:15.250";
  const std::string head = "49=CLIENT|56=VENUE|34=1|52=" + ts + "|";

  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{3});
  n.cl_ord_id = id_of("fm000100000001");
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.tif = TimeInForce::Gtc;
  n.price = Price::from_decimal("70000.5").value();
  n.qty = Qty::from_decimal("0.001").value();
  std::string m = encode(n.hdr);
  CHECK(m == wrap("35=D|" + head + "11=fm000100000001|18=6|55=BTCUSDT|54=1|60=" + ts +
                  "|38=0.001|40=2|44=70000.5|59=1|"));
  FixView v;
  CHECK(v.parse(m) == FixError::None);

  OutNewOrderMsg mkt{};
  init_header(mkt, EventType::OutNewOrder, InstrumentId{1}, VenueId{3});
  mkt.cl_ord_id = id_of("fm0001000000aa");
  mkt.side = Side::Sell;
  mkt.type = OrderType::Market;
  mkt.tif = TimeInForce::Ioc;
  mkt.qty = Qty::from_int(2);
  CHECK(encode(mkt.hdr) ==
        wrap("35=D|" + head + "11=fm0001000000aa|55=ETHUSDT|54=2|60=" + ts + "|38=2|40=1|59=3|"));

  OutNewOrderMsg fok = n;
  fok.cl_ord_id = id_of("fm0001000000ab");
  fok.type = OrderType::Limit;
  fok.tif = TimeInForce::Fok;
  CHECK(field_of(encode(fok.hdr), tag::kTimeInForce) == "4");
  fok.tif = TimeInForce::Day;
  CHECK(field_of(encode(fok.hdr), tag::kTimeInForce) == "0");
  CHECK(field_of(encode(fok.hdr), tag::kExecInst).empty());

  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{3});
  c.cl_ord_id = id_of("fm000100000001");
  c.venue_order_id = "1001";
  CHECK(encode(c.hdr) == wrap("35=F|" + head + "41=fm000100000001|37=1001|11=fm000100000001c1|" +
                              "55=BTCUSDT|54=1|60=" + ts + "|38=0.001|"));
  c.venue_order_id.clear();
  CHECK(field_of(encode(c.hdr), tag::kClOrdID) == "fm000100000001c2");

  OutReplaceMsg r{};
  init_header(r, EventType::OutReplace, InstrumentId{0}, VenueId{3});
  r.cl_ord_id = id_of("fm000100000002");
  r.orig_cl_ord_id = id_of("fm000100000001");
  r.price = Price::from_int(70001);
  r.qty = Qty::from_decimal("0.002").value();
  CHECK(encode(r.hdr) == wrap("35=G|" + head + "41=fm000100000001|11=fm000100000002|18=6|" +
                              "55=BTCUSDT|54=1|60=" + ts + "|38=0.002|40=2|44=70001|59=1|"));
  // The replacement is remembered: cancel it and replace it again.
  c.cl_ord_id = r.cl_ord_id;
  CHECK(field_of(encode(c.hdr), tag::kOrderQty) == "0.002");
  OutReplaceMsg r2 = r;
  r2.orig_cl_ord_id = r.cl_ord_id;
  r2.cl_ord_id = id_of("fm000100000003");
  r2.venue_order_id = "1005";
  const std::string m2 = encode(r2.hdr);
  CHECK(field_of(m2, tag::kOrderID) == "1005");
  CHECK(field_of(m2, tag::kSide) == "1");
  CHECK(enc.stats().new_orders == 5);  // post-only, market, FOK, Day twice
  CHECK(enc.stats().cancels == 3);
  CHECK(enc.stats().replaces == 2);

  // Failures: unknown order, unknown instrument, buffer too small.
  OutCancelMsg unknown{};
  init_header(unknown, EventType::OutCancel, InstrumentId{0}, VenueId{3});
  unknown.cl_ord_id = id_of("fm0001000000ff");
  CHECK(encode(unknown.hdr).empty());
  CHECK(enc.stats().unknown_orders == 1);
  OutNewOrderMsg bad = n;
  bad.hdr.instrument = InstrumentId{7};
  CHECK(encode(bad.hdr).empty());
  CHECK(enc.stats().unknown_symbols == 1);
  std::vector<std::byte> small(64);
  CHECK(enc.encode(command_of(n.hdr), small) == 0);
  // remember() seeds orders the encoder did not send.
  enc.remember(id_of("fm0001000000ff"),
               InstrumentId{1},
               Side::Sell,
               OrderType::Limit,
               TimeInForce::Gtc,
               Price::from_int(1),
               Qty::from_int(3));
  CHECK(field_of(encode(unknown.hdr), tag::kSymbol) == "ETHUSDT");

  // send() goes through the session, which must be logged on.
  CHECK_FALSE(enc.send(session, command_of(n.hdr)));
}
