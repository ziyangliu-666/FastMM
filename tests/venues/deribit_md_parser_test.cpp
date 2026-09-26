// DeribitMdParser and the REST decoders against recorded testnet frames
// (tests/fixtures/deribit, see fixtures.meta.json): book snapshot/change with contract conversion,
// ticker -> BookTicker + OptionTicker, trades, heartbeats, responses, reference data mapping.
#include "fastmm/venues/deribit/deribit_md_parser.hpp"

#include "fake_venue_util.hpp"

#include "fastmm/core/position.hpp"
#include "fastmm/venues/deribit/deribit_md_feed.hpp"
#include "fastmm/venues/deribit/deribit_rest_decoder.hpp"
#include "fastmm/venues/deribit/deribit_venue.hpp"
#include "fastmm/venues/registry.hpp"

#include <cmath>
#include <sstream>
#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::deribit;
using namespace fastmm::venues::test;

namespace {

constexpr VenueId kVenue{2};

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

// Ids: 0 BTC-15SEP26-77000-C (option), 1 BTC-PERPETUAL (10 USD contracts).
struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    Instrument call = make_instrument("BTC-15SEP26-77000-C", kVenue.value, "BTC", "BTC");
    call.asset_class = AssetClass::Option;
    call.option_type = OptionType::Call;
    call.tick = px("0.0001");
    call.lot = qt("0.1");
    REQUIRE(instruments.add(call));
    Instrument perp = make_instrument("BTC-PERPETUAL", kVenue.value, "BTC", "USD");
    perp.asset_class = AssetClass::Perpetual;
    perp.tick = px("0.5");
    perp.lot = qt("1");
    perp.contract_multiplier = Qty::from_int(10);
    REQUIRE(instruments.add(perp));
    REQUIRE(symbols.build(instruments));
  }
};

MdDecodeResult decode(DeribitMdParser& p, const std::string& fixture, Scratch& s) {
  const PaddedJson j = padded_fixture(fixture);
  return p.decode(j.view(), Timestamp{42}, Cycles{7}, s.span());
}

}  // namespace

TEST_CASE("deribit.md_parser: option book snapshot then change") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  MdDecodeResult r = decode(p, "deribit/book_option_snapshot.json", s);
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::BookSnapshot);
  CHECK(r.frame == FrameKind::Notification);
  const auto& snap = s.as<BookDeltaMsg>();
  CHECK(snap.hdr.type == EventType::BookSnapshot);
  CHECK(snap.is_snapshot());
  CHECK(snap.hdr.instrument == InstrumentId{0});
  CHECK(snap.hdr.venue == kVenue);
  CHECK(snap.hdr.len == BookDeltaMsg::size_for(1, 1));
  CHECK(snap.last_update_id == 118850965713ULL);
  CHECK(snap.prev_update_id == 0);
  CHECK(snap.hdr.exch_ts.ns == 1789344930088LL * 1'000'000);
  CHECK(snap.hdr.recv_ts.ns == 42);
  CHECK(snap.hdr.t0_cycles.v == 7);
  REQUIRE(snap.bid_count == 1);
  REQUIRE(snap.ask_count == 1);
  CHECK(snap.bids()[0] == Level{px("0.0065"), qt("10")});
  CHECK(snap.asks()[0] == Level{px("0.0075"), qt("10")});

  r = decode(p, "deribit/book_option_change.json", s);
  REQUIRE(r.ok());
  const auto& chg = s.as<BookDeltaMsg>();
  CHECK(chg.hdr.type == EventType::BookDelta);
  CHECK_FALSE(chg.is_snapshot());
  CHECK(chg.last_update_id == 118850980778ULL);
  CHECK(chg.prev_update_id == 118850965713ULL);
  CHECK(chg.bid_count == 1);
  CHECK(chg.ask_count == 0);
  CHECK(chg.bids()[0] == Level{px("0.0055"), qt("41")});
  CHECK(p.stats().book_snapshots == 1);
  CHECK(p.stats().book_changes == 1);
}

TEST_CASE("deribit.md_parser: perpetual amounts become contracts, deletes become zero") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  REQUIRE(decode(p, "deribit/book_perp_snapshot.json", s).ok());
  const auto& snap = s.as<BookDeltaMsg>();
  CHECK(snap.hdr.instrument == InstrumentId{1});
  REQUIRE(snap.bid_count == 12);
  REQUIRE(snap.ask_count == 12);
  CHECK(snap.bids()[0] == Level{px("76914"), qt("100020")});  // 1000200 USD / 10 USD contracts
  CHECK(snap.asks()[0] == Level{px("76914.5"), qt("99999")});
  CHECK(snap.bids()[11].price == px("76867"));
  REQUIRE(decode(p, "deribit/book_perp_change_1.json", s).ok());
  const auto& chg = s.as<BookDeltaMsg>();
  CHECK(chg.prev_update_id == 118850968930ULL);
  CHECK(chg.last_update_id == 118850969743ULL);
  bool saw_delete = false;
  for (const Level& l : chg.bids())
    saw_delete = saw_delete || (l.price == px("76893") && l.qty.is_zero());
  CHECK(saw_delete);
}

TEST_CASE("deribit.md_parser: option ticker yields BookTicker and OptionTicker") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const MdDecodeResult r = decode(p, "deribit/ticker_option.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);
  CHECK(r.len == sizeof(BookTickerMsg) + sizeof(OptionTickerMsg));
  const auto& bt = s.as<BookTickerMsg>();
  CHECK(bt.hdr.type == EventType::BookTicker);
  CHECK(bt.bid_px == px("0.0065"));
  CHECK(bt.bid_qty == qt("10"));
  CHECK(bt.ask_px == px("0.0075"));
  CHECK(bt.ask_qty == qt("10"));
  CHECK(bt.hdr.exch_ts.ns == 1789344931096LL * 1'000'000);
  const auto& ot = *reinterpret_cast<const OptionTickerMsg*>(s.buf + sizeof(BookTickerMsg));
  CHECK(ot.hdr.type == EventType::OptionTicker);
  CHECK(ot.hdr.len == sizeof(OptionTickerMsg));
  CHECK(ot.hdr.instrument == InstrumentId{0});
  CHECK(ot.mark_price == px("0.0069"));
  CHECK(ot.underlying_price == px("76904.4"));
  CHECK(ot.index_price == px("76900.24"));
  CHECK(ot.mark_iv == doctest::Approx(0.312));
  CHECK(ot.bid_iv == doctest::Approx(0.2957));
  CHECK(ot.ask_iv == doctest::Approx(0.3374));
  CHECK(ot.delta == doctest::Approx(0.47736));
  CHECK(ot.gamma == doctest::Approx(0.00028));
  CHECK(ot.vega == doctest::Approx(18.43602));
  CHECK(ot.theta == doctest::Approx(-217.49347));
  CHECK(ot.rho == doctest::Approx(1.31068));
  CHECK(ot.interest_rate == doctest::Approx(0.0));
  CHECK(p.stats().option_tickers == 1);
}

TEST_CASE("deribit.md_parser: perpetual ticker has no greeks and keeps exponent amounts exact") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const MdDecodeResult r = decode(p, "deribit/ticker_perp.json", s);
  REQUIRE(r.ok());
  CHECK(r.count == 1);
  const auto& bt = s.as<BookTickerMsg>();
  CHECK(bt.bid_px == px("76914"));
  CHECK(bt.bid_qty == qt("100020"));  // "best_bid_amount":1.0002e6 on the wire
  CHECK(bt.ask_px == px("76914.5"));
  CHECK(bt.ask_qty == qt("99999"));
}

TEST_CASE("deribit.md_parser: trades, heartbeats, responses, ignored and bad frames") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  MdDecodeResult r = decode(p, "deribit/trades_perp.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 3);
  const auto& t = s.as<TradeMsg>();
  CHECK(t.hdr.type == EventType::Trade);
  CHECK(t.price == px("76917.5"));
  CHECK(t.qty == qt("845"));
  CHECK(t.trade_id == 267258393ULL);
  CHECK(t.aggressor == Side::Buy);
  CHECK(t.hdr.venue_seq == 140284854ULL);
  CHECK(reinterpret_cast<const TradeMsg*>(s.buf + 2 * sizeof(TradeMsg))->price == px("76919.5"));

  r = decode(p, "deribit/heartbeat_test_request.json", s);
  CHECK(r.status == ParseStatus::Ignored);
  CHECK(r.frame == FrameKind::TestRequest);

  r = decode(p, "deribit/subscribe_ok.json", s);
  CHECK(r.frame == FrameKind::Response);
  CHECK(r.rpc.id == 4);
  CHECK(r.result_items == 7);
  r = decode(p, "deribit/subscribe_partial.json", s);
  CHECK(r.rpc.id == 5);
  CHECK(r.result_items == 1);  // bogus.channel silently dropped
  r = decode(p, "deribit/rpc_unauthorized.json", s);
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.rpc.id == 6);
  CHECK(r.rpc.error_code == 13009);
  CHECK(r.rpc.error_message == "unauthorized");
  CHECK(r.rpc.error_reason == "invalid_token");
  r = decode(p, "deribit/heartbeat_interval_too_small.json", s);
  CHECK(r.rpc.error_code == -32602);
  CHECK(r.rpc.error_reason == "value must be >= 10");
  r = decode(p, "deribit/string_id_echo.json", s);
  CHECK(r.rpc.id == -1);
  CHECK(r.rpc.id_text == "nfm000100000001");

  CHECK(decode(p, "deribit/book_grouped_ignored.json", s).status == ParseStatus::Ignored);
  const PaddedJson unknown(
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"trades.ETH-PERPETUAL.100ms","data":[]}})");
  CHECK(p.decode(unknown.view(), Timestamp{}, Cycles{}, s.span()).status ==
        ParseStatus::UnknownSymbol);
  const PaddedJson truncated(
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"book.BTC-PERPETUAL.100ms","data":{"timestamp":1,"type":"change","change_id":5,"bids":[["new",1.0]],"asks":[],"prev_change_id":4}}})");
  CHECK(p.decode(truncated.view(), Timestamp{}, Cycles{}, s.span()).status ==
        ParseStatus::Malformed);
  const PaddedJson no_prev(
      R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"book.BTC-PERPETUAL.100ms","data":{"timestamp":1,"type":"change","change_id":5,"bids":[],"asks":[]}}})");
  CHECK(p.decode(no_prev.view(), Timestamp{}, Cycles{}, s.span()).status == ParseStatus::Malformed);
  const PaddedJson garbage("{nope");
  CHECK(p.decode(garbage.view(), Timestamp{}, Cycles{}, s.span()).status == ParseStatus::Malformed);
}

TEST_CASE("deribit.md_feed: the recorded option session syncs without a gap") {
  Universe u;
  RecordingSink md(8U << 20);
  int resubscribes = 0;
  struct Ctx {
    int* n;
  } ctx{&resubscribes};
  DeribitMdFeed feed(u.symbols,
                     u.instruments,
                     kVenue,
                     md.sink,
                     ResubscribeRequester{
                         [](void* c, InstrumentId) noexcept { ++*static_cast<Ctx*>(c)->n; }, &ctx});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  REQUIRE(feed.subscription_payloads().size() == 1);
  CHECK(
      feed.subscription_payloads()[0] ==
      R"({"jsonrpc":"2.0","id":1000,"method":"public/subscribe","params":{"channels":["book.BTC-15SEP26-77000-C.100ms","ticker.BTC-15SEP26-77000-C.100ms","trades.BTC-15SEP26-77000-C.100ms"]}})");
  CHECK(
      feed.resubscribe_payloads(InstrumentId{0})[0] ==
      R"({"jsonrpc":"2.0","id":20,"method":"public/unsubscribe","params":{"channels":["book.BTC-15SEP26-77000-C.100ms"]}})");
  feed.on_connected();
  std::istringstream lines(fastmm::test::fixture("deribit/raw_option_stream.jsonl"));
  std::string line;
  std::size_t frames = 0;
  while (std::getline(lines, line)) {
    const PaddedJson j(line);
    CHECK(feed.on_message(j.view(), 1) == ParseStatus::Ok);
    ++frames;
  }
  CHECK(frames > 50);
  CHECK(feed.synced_count() == 1);
  CHECK(feed.resync_count() == 0);
  CHECK(resubscribes == 0);
  Collected c;
  c.take(md);
  CHECK(c.count(EventType::BookSnapshot) == 1);
  CHECK(c.count(EventType::OptionTicker) == c.count(EventType::BookTicker));
  CHECK(c.count(EventType::OptionTicker) > 10);
  const PaddedJson partial = padded_fixture("deribit/subscribe_partial.json");
  static_cast<void>(feed.on_message(partial.view(), 1));  // id 5 is not a feed subscription
  CHECK(feed.stats().subscribe_errors == 0);
  const PaddedJson ok(
      R"({"jsonrpc":"2.0","id":1000,"result":["book.BTC-15SEP26-77000-C.100ms"],"usIn":1,"usOut":2,"usDiff":1,"testnet":true})");
  static_cast<void>(feed.on_message(ok.view(), 1));
  CHECK(feed.stats().subscribe_errors == 1);  // 1 of 3 channels subscribed
}

TEST_CASE("deribit.rest: get_instruments decoding and the reference data mapping") {
  std::vector<InstrumentInfo> infos;
  REQUIRE(decode_instruments(fastmm::test::fixture("deribit/get_instruments_option.json"), infos)
              .empty());
  REQUIRE(decode_instruments(fastmm::test::fixture("deribit/get_instruments_future.json"), infos)
              .empty());
  REQUIRE(infos.size() == 5);
  const InstrumentInfo& c = infos[0];
  CHECK(c.name == "BTC-15SEP26-77000-C");
  CHECK(c.kind == "option");
  CHECK(c.option_type == "call");
  CHECK(c.strike == px("77000"));
  CHECK(c.expiration_ms == 1789459200000LL);
  CHECK(c.tick == px("0.0001"));
  REQUIRE(c.tick_steps.size() == 1);
  CHECK(c.tick_steps[0].above_price == px("0.005"));
  CHECK(c.tick_steps[0].tick == px("0.0005"));
  CHECK(c.contract_size == qt("1"));
  CHECK(c.min_trade_amount == qt("0.1"));
  CHECK_FALSE(c.inverse());             // reversed, but priced in the BTC it settles in
  CHECK(c.settlement_period == "day");  // not in the OpenAPI enum
  CHECK(c.is_active);

  Instrument inst = make_instrument("BTC-15SEP26-77000-C", kVenue.value, "", "");
  TickSchedule ticks;
  REQUIRE(apply_instrument_info(c, inst, ticks).empty());
  CHECK(inst.asset_class == AssetClass::Option);
  CHECK(inst.option_type == OptionType::Call);
  CHECK(inst.strike == px("77000"));
  CHECK(inst.expiry_ns == 1789459200000LL * 1'000'000);
  CHECK(inst.tick == px("0.0001"));
  CHECK(inst.lot == qt("0.1"));
  CHECK(inst.min_qty == qt("0.1"));
  CHECK(inst.contract_multiplier == qt("1"));
  CHECK_FALSE(inst.inverse());
  CHECK(inst.coin_quoted());  // the model value is the USD price over the forward
  // Linear in the premium, in BTC: one contract at 0.0065 is 0.0065 BTC, and 0.0065 -> 0.0075 on
  // two contracts gains 0.002 BTC.
  CHECK(inst.settlement_ccy() == "BTC");
  CHECK(inst.notional(px("0.0065"), qt("1")) == Notional::from_decimal("0.0065").value());
  {
    Instrument opt = inst;
    opt.id = InstrumentId{0};
    PositionTracker book;
    book.on_fill(opt.id, Side::Buy, px("0.0065"), qt("2"), Notional{}, opt);
    book.mark(opt.id, px("0.0075"), opt);
    CHECK(book.get(opt.id).unrealized == Notional::from_decimal("0.002").value());
  }
  CHECK(inst.enabled());
  CHECK(inst.price_decimals == 4);
  CHECK(inst.base.view() == "BTC");
  CHECK(inst.quote.view() == "BTC");
  CHECK(ticks.base == px("0.0001"));
  CHECK(ticks.tick_for(px("0.01")) == px("0.0005"));

  const InstrumentInfo& perp = infos[3];
  CHECK(perp.name == "BTC-PERPETUAL");
  Instrument pi = make_instrument("BTC-PERPETUAL", kVenue.value, "", "");
  TickSchedule pt;
  REQUIRE(apply_instrument_info(perp, pi, pt).empty());
  CHECK(pi.asset_class == AssetClass::Perpetual);
  CHECK(pi.expiry_ns == 0);
  CHECK(pi.tick == px("0.5"));
  CHECK(pi.contract_multiplier == qt("10"));
  CHECK(pi.lot == qt("1"));  // min_trade_amount 10 USD / 10 USD contracts
  CHECK(pi.inverse());
  CHECK_FALSE(pi.coin_quoted());
  CHECK(pi.quote.view() == "USD");
  CHECK(pt.steps.empty());

  Instrument fi = make_instrument("BTC-25DEC26", kVenue.value, "", "");
  REQUIRE(apply_instrument_info(infos[4], fi, pt).empty());
  CHECK(fi.asset_class == AssetClass::Future);
  CHECK(fi.expiry_ns == 1798185600000LL * 1'000'000);
  CHECK(fi.tick == px("2.5"));

  InstrumentInfo closed = c;
  closed.state = "settlement";
  Instrument ci = make_instrument("BTC-15SEP26-77000-C", kVenue.value, "", "");
  REQUIRE(apply_instrument_info(closed, ci, ticks).empty());
  CHECK_FALSE(ci.enabled());
  InstrumentInfo combo = c;
  combo.kind = "option_combo";
  CHECK_FALSE(apply_instrument_info(combo, ci, ticks).empty());

  std::int64_t ms = 0;
  CHECK(decode_server_time(fastmm::test::fixture("deribit/get_time.json"), ms).empty());
  CHECK(ms > 1789340000000LL);
  RpcEnvelope env;
  REQUIRE(decode_envelope(fastmm::test::fixture("deribit/rpc_invalid_credentials.json"), env));
  CHECK(env.error_code == 13004);
  CHECK(env.message == "invalid_credentials");
  REQUIRE(decode_envelope(fastmm::test::fixture("deribit/cancel_all_by_instrument_ok.json"), env));
  CHECK(env.has_result);
  CHECK_FALSE(
      decode_instruments(fastmm::test::fixture("deribit/rpc_invalid_params.json"), infos).empty());
}

TEST_CASE("deribit.config: section mapping and factory registration") {
  VenueSection s;
  s.name = "deribit";
  s.kind = "deribit";
  s.ws_url = "wss://test.deribit.com/ws/api/v2";
  s.rest_url = "https://test.deribit.com/api/v2";
  s.api_key = "cid";
  s.api_secret = "secret";
  s.supports_replace = true;
  s.extra["currencies"] = R"(["btc", "ETH"])";
  s.extra["heartbeat_interval_s"] = "5";
  s.extra["book_interval"] = "agg2";
  s.extra["reject_post_only"] = "false";
  s.extra["matching_engine_rate"] = "20";
  s.extra["matching_engine_burst"] = "50";
  const DeribitVenueConfig c = make_deribit_config(s, false);
  REQUIRE(c.currencies.size() == 2);
  CHECK(c.currencies[0] == "BTC");
  CHECK(c.currencies[1] == "ETH");
  CHECK(c.ws_private_url == s.ws_url);
  CHECK(c.heartbeat_interval_s == 10);  // public/set_heartbeat minimum
  CHECK(c.intervals.book == "agg2");
  CHECK(c.intervals.ticker == "100ms");
  CHECK_FALSE(c.reject_post_only);
  CHECK(c.matching_engine_rate == 20);
  CHECK(c.matching_engine_burst == 50);
  CHECK(c.credentials.usable());
  register_builtin_venues();
  REQUIRE(VenueRegistry::instance().find("deribit") != nullptr);
  const auto venue = make_venue(VenueId{0}, s, VenueFactoryOptions{true, {}});
  REQUIRE(venue != nullptr);
  CHECK(venue->name() == "deribit");
  CHECK(venue->caps().supports_replace);
  CHECK_FALSE(venue->caps().user_stream);  // dry run
}
