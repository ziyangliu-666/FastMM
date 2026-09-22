// Binance Spot SBE market data (md_format = "sbe"): the generated stream_1_0 flyweights against
// hand-packed fixtures (tests/fixtures/binance/sbe/make_fixtures.py), the decoder's output against
// the JSON decoder's for the same event, exponent scaling, truncation and foreign frames, and the
// SBE feed driving BinanceDepthSync exactly like the JSON feed.
#include "venue_test_util.hpp"

#include "fastmm/venues/binance/binance_md_feed.hpp"
#include "fastmm/venues/binance/binance_md_parser.hpp"
#include "fastmm/venues/binance/binance_sbe_md_parser.hpp"
#include "fastmm/venues/binance/generated/binance_stream_sbe.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;
namespace sbe = fastmm::codecs::sbe;
namespace ss = fastmm::venues::binance::sbe_stream;

namespace {

using Bytes = std::vector<std::byte>;
const Timestamp kRecv{1'700'000'000'000'000'000LL};
const Cycles kT0{123456};

// Hex text: pairs of hex digits, '#' starts a comment to the end of the line.
Bytes hex_fixture(const std::string& rel) {
  const std::string text = fastmm::test::fixture(rel);
  Bytes out;
  int high = -1;
  bool comment = false;
  for (const char c : text) {
    if (c == '\n') {
      comment = false;
      continue;
    }
    if (comment) continue;
    if (c == '#') {
      comment = true;
      continue;
    }
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    if (v < 0) continue;
    if (high < 0) {
      high = v;
    } else {
      out.push_back(static_cast<std::byte>(high * 16 + v));
      high = -1;
    }
  }
  REQUIRE(high < 0);
  return out;
}

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}

// Decodes one frame and returns copies of every emitted message.
struct Decoded {
  ParseStatus status = ParseStatus::Ignored;
  std::vector<Bytes> msgs;
  std::vector<MdKind> kinds;
  template <class M>
  const M& as(std::size_t i = 0) const {
    return *reinterpret_cast<const M*>(msgs.at(i).data());
  }
};
Decoded decode(BinanceSbeMdParser& p, std::span<const std::byte> frame) {
  Scratch s;
  Decoded d;
  d.status = p.decode(frame, kRecv, kT0, s.span(), [&](EventHeader& h, MdKind k) {
    const auto* b = reinterpret_cast<const std::byte*>(&h);
    d.msgs.emplace_back(b, b + h.len);
    d.kinds.push_back(k);
  });
  return d;
}

// Frame writer over the generated writers: header + body.
struct FrameBuf {
  alignas(8) std::byte buf[4096] = {};
  std::size_t size = 0;
  [[nodiscard]] std::span<std::byte> body() {
    return {buf + sbe::MessageHeader::kSize, sizeof buf - sbe::MessageHeader::kSize};
  }
  template <class W>
  void finish(const W& w) {
    W::header().store(buf);
    size = sbe::MessageHeader::kSize + w.size_bytes();
  }
  [[nodiscard]] std::span<const std::byte> view() const { return {buf, size}; }
};

// DepthDiffStreamEvent with one bid level, exponents -2 / -8.
FrameBuf depth_frame(std::int64_t first,
                     std::int64_t last,
                     std::int64_t bid_cents,
                     std::int64_t qty8) {
  FrameBuf f;
  ss::DepthDiffStreamEventWriter w(f.body());
  w.set_event_time(1789295134334000);
  w.set_first_book_update_id(first);
  w.set_last_book_update_id(last);
  w.set_price_exponent(-2);
  w.set_qty_exponent(-8);
  auto bids = w.bids(1);
  bids[0].set_price(bid_cents);
  bids[0].set_qty(qty8);
  static_cast<void>(w.asks(0));
  REQUIRE(w.set_symbol("BTCUSDT"));
  REQUIRE(w.ok());
  f.finish(w);
  return f;
}

void check_same_header(const EventHeader& a, const EventHeader& b) {
  CHECK(a.len == b.len);
  CHECK(a.type == b.type);
  CHECK(a.version == b.version);
  CHECK(a.venue == b.venue);
  CHECK(a.instrument == b.instrument);
  CHECK(a.flags == b.flags);
  CHECK(a.exch_ts == b.exch_ts);
  CHECK(a.recv_ts == b.recv_ts);
  CHECK(a.t0_cycles == b.t0_cycles);
  CHECK(a.venue_seq == b.venue_seq);
}

}  // namespace

TEST_CASE("binance.sbe: schema constants match stream_1_0.xml") {
  static_assert(ss::kSchemaId == 1 && ss::kSchemaVersion == 0);
  static_assert(ss::TradesStreamEvent::kTemplateId == 10000 &&
                ss::TradesStreamEvent::kBlockLength == 18 &&
                ss::TradesStreamEvent::Trades::kBlockLength == 25);
  static_assert(ss::BestBidAskStreamEvent::kTemplateId == 10001 &&
                ss::BestBidAskStreamEvent::kBlockLength == 50);
  static_assert(ss::DepthSnapshotStreamEvent::kTemplateId == 10002 &&
                ss::DepthSnapshotStreamEvent::kBlockLength == 18 &&
                ss::DepthSnapshotStreamEvent::Bids::kBlockLength == 16);
  static_assert(ss::DepthDiffStreamEvent::kTemplateId == 10003 &&
                ss::DepthDiffStreamEvent::kBlockLength == 26);
  static_assert(ss::GroupSizeEncoding::kSize == 6 && ss::GroupSize16Encoding::kSize == 4);
  CHECK(ss::template_name(10003) == "DepthDiffStreamEvent");
}

TEST_CASE("binance.sbe: flyweights read the hand-packed depth diff field by field") {
  const Bytes f = hex_fixture("binance/sbe/depth_diff.hex");
  const auto h = sbe::MessageHeader::load(f.data());
  CHECK(h.template_id == 10003);
  CHECK(h.schema_id == 1);
  CHECK(h.version == 0);
  const ss::DepthDiffStreamEvent m(
      std::span<const std::byte>(f).subspan(sbe::MessageHeader::kSize), h.block_length, h.version);
  REQUIRE(m.valid());
  CHECK(m.size_bytes() == f.size() - sbe::MessageHeader::kSize);
  CHECK(m.event_time() == 1789295134334000);
  CHECK(m.first_book_update_id() == 94127);
  CHECK(m.last_book_update_id() == 94129);
  CHECK(m.price_exponent() == -8);
  REQUIRE(m.bids().count() == 2);
  CHECK(m.bids()[1].qty() == 0);
  REQUIRE(m.asks().count() == 1);
  CHECK(m.asks()[0].price() == 7000010000000);
  CHECK(m.symbol() == "BTCUSDT");
}

TEST_CASE("binance.sbe: generated writer reproduces the hand-packed bytes") {
  const Bytes f = hex_fixture("binance/sbe/depth_diff.hex");
  FrameBuf b;
  ss::DepthDiffStreamEventWriter w(b.body());
  w.set_event_time(1789295134334000);
  w.set_first_book_update_id(94127);
  w.set_last_book_update_id(94129);
  w.set_price_exponent(-8);
  w.set_qty_exponent(-8);
  auto bids = w.bids(2);
  bids[0].set_price(7000000000000);
  bids[0].set_qty(150000000);
  bids[1].set_price(6999900000000);
  bids[1].set_qty(0);
  auto asks = w.asks(1);
  asks[0].set_price(7000010000000);
  asks[0].set_qty(200000000);
  REQUIRE(w.set_symbol("BTCUSDT"));
  REQUIRE(w.ok());
  b.finish(w);
  REQUIRE(b.size == f.size());
  CHECK(std::memcmp(b.buf, f.data(), f.size()) == 0);
}

TEST_CASE("binance.sbe: depth diff -> BookDeltaMsg (U/u, levels, us timestamps)") {
  TestUniverse u;
  BinanceSbeMdParser p(u.symbols, VenueId{0});
  const Bytes f = hex_fixture("binance/sbe/depth_diff.hex");
  const Decoded d = decode(p, f);
  REQUIRE(d.status == ParseStatus::Ok);
  REQUIRE(d.msgs.size() == 1);
  CHECK(d.kinds[0] == MdKind::BookDelta);
  const auto& m = d.as<BookDeltaMsg>();
  CHECK(m.hdr.type == EventType::BookDelta);
  CHECK(m.hdr.len == BookDeltaMsg::size_for(2, 1));
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == VenueId{0});
  CHECK(m.hdr.flags == 0);
  CHECK(m.hdr.recv_ts == kRecv);
  CHECK(m.hdr.t0_cycles == kT0);
  CHECK(m.hdr.exch_ts.ns == 1789295134334000LL * 1000);
  CHECK(m.hdr.venue_seq == 94129);
  CHECK(m.first_update_id == 94127);
  CHECK(m.last_update_id == 94129);
  CHECK(m.prev_update_id == 0);
  REQUIRE(m.bid_count == 2);
  REQUIRE(m.ask_count == 1);
  CHECK(m.bids()[0].price == px("70000"));
  CHECK(m.bids()[0].qty == qty("1.5"));
  CHECK(m.bids()[1].price == px("69999"));
  CHECK(m.bids()[1].qty == Qty{});
  CHECK(m.asks()[0].price == px("70000.1"));
  CHECK(m.asks()[0].qty == qty("2"));
  CHECK(p.stats().book_deltas == 1);
}

TEST_CASE("binance.sbe: bestBidAsk scales coarse exponents") {
  TestUniverse u;
  BinanceSbeMdParser p(u.symbols, VenueId{0});
  const Decoded d = decode(p, hex_fixture("binance/sbe/best_bid_ask.hex"));
  REQUIRE(d.status == ParseStatus::Ok);
  REQUIRE(d.msgs.size() == 1);
  const auto& m = d.as<BookTickerMsg>();
  CHECK(m.hdr.type == EventType::BookTicker);
  CHECK(m.bid_px == px("70000"));
  CHECK(m.bid_qty == qty("1.5"));
  CHECK(m.ask_px == px("70000.1"));
  CHECK(m.ask_qty == qty("2"));
  CHECK(m.hdr.venue_seq == 94130);
  CHECK(m.hdr.exch_ts.ns == 1789295134226000LL * 1000);
}

TEST_CASE("binance.sbe: a trades event yields one TradeMsg per trade") {
  TestUniverse u;
  BinanceSbeMdParser p(u.symbols, VenueId{0});
  const Decoded d = decode(p, hex_fixture("binance/sbe/trades.hex"));
  REQUIRE(d.status == ParseStatus::Ok);
  REQUIRE(d.msgs.size() == 2);
  const auto& a = d.as<TradeMsg>(0);
  const auto& b = d.as<TradeMsg>(1);
  CHECK(a.trade_id == 388510);
  CHECK(a.price == px("70000.1"));
  CHECK(a.qty == qty("0.00065"));
  CHECK(a.aggressor == Side::Sell);  // buyer was the maker
  CHECK(a.hdr.venue_seq == 388510);
  CHECK(a.hdr.exch_ts.ns == 1789295134225000LL * 1000);  // transactTime
  CHECK(b.trade_id == 388511);
  CHECK(b.price == px("70000.2"));
  CHECK(b.qty == qty("0.001"));
  CHECK(b.aggressor == Side::Buy);
  CHECK(p.stats().trades == 2);
}

TEST_CASE("binance.sbe: depth20 snapshot decodes as a kSnapshot book") {
  TestUniverse u;
  BinanceSbeMdParser p(u.symbols, VenueId{0});
  const Decoded d = decode(p, hex_fixture("binance/sbe/depth_snapshot20.hex"));
  REQUIRE(d.status == ParseStatus::Ok);
  CHECK(d.kinds[0] == MdKind::BookSnapshot);
  const auto& m = d.as<BookDeltaMsg>();
  CHECK(m.hdr.type == EventType::BookSnapshot);
  CHECK(m.is_snapshot());
  CHECK(m.first_update_id == 94131);
  CHECK(m.last_update_id == 94131);
  CHECK(m.bid_count == 1);
  CHECK(m.ask_count == 2);
  CHECK(m.asks()[1].qty == qty("0.5"));
}

TEST_CASE("binance.sbe: output equals the JSON decoder's for the same events") {
  TestUniverse u;
  BinanceSbeMdParser sp(u.symbols, VenueId{0});
  BinanceMdParser jp(u.symbols, VenueId{0});
  Scratch js;

  // depth diff
  const PaddedJson depth_json(
      R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1789295134334,"s":"BTCUSDT","U":94127,"u":94129,"b":[["70000.00000000","1.50000000"],["69999.00000000","0.00000000"]],"a":[["70000.10000000","2.00000000"]]}})");
  REQUIRE(jp.decode(depth_json.view(), kRecv, kT0, js.span()).ok());
  const Decoded sd = decode(sp, hex_fixture("binance/sbe/depth_diff.hex"));
  REQUIRE(sd.status == ParseStatus::Ok);
  const auto& jd = js.as<BookDeltaMsg>();
  const auto& sdm = sd.as<BookDeltaMsg>();
  check_same_header(jd.hdr, sdm.hdr);
  CHECK(jd.first_update_id == sdm.first_update_id);
  CHECK(jd.last_update_id == sdm.last_update_id);
  CHECK(jd.prev_update_id == sdm.prev_update_id);
  REQUIRE(jd.bid_count == sdm.bid_count);
  REQUIRE(jd.ask_count == sdm.ask_count);
  for (std::uint32_t i = 0; i < jd.bid_count + jd.ask_count; ++i) {
    CHECK(jd.levels()[i].price == sdm.levels()[i].price);
    CHECK(jd.levels()[i].qty == sdm.levels()[i].qty);
  }

  // trade
  const PaddedJson trade_json(
      R"({"stream":"btcusdt@trade","data":{"e":"trade","E":1789295134226,"s":"BTCUSDT","t":388510,"p":"70000.10000000","q":"0.00065000","T":1789295134225,"m":true,"M":true}})");
  REQUIRE(jp.decode(trade_json.view(), kRecv, kT0, js.span()).ok());
  const Decoded st = decode(sp, hex_fixture("binance/sbe/trades.hex"));
  REQUIRE(st.msgs.size() == 2);
  const auto& jt = js.as<TradeMsg>();
  const auto& stm = st.as<TradeMsg>(0);
  check_same_header(jt.hdr, stm.hdr);
  CHECK(jt.price == stm.price);
  CHECK(jt.qty == stm.qty);
  CHECK(jt.trade_id == stm.trade_id);
  CHECK(jt.aggressor == stm.aggressor);

  // best bid/ask: the JSON bookTicker has no event time; everything else matches.
  const PaddedJson bbo_json(
      R"({"stream":"btcusdt@bookTicker","data":{"u":94130,"s":"BTCUSDT","b":"70000.00000000","B":"1.50000000","a":"70000.10000000","A":"2.00000000"}})");
  REQUIRE(jp.decode(bbo_json.view(), kRecv, kT0, js.span()).ok());
  const Decoded sb = decode(sp, hex_fixture("binance/sbe/best_bid_ask.hex"));
  REQUIRE(sb.msgs.size() == 1);
  BookTickerMsg sbe_bbo = sb.as<BookTickerMsg>();
  const auto& jb = js.as<BookTickerMsg>();
  sbe_bbo.hdr.exch_ts = jb.hdr.exch_ts;
  check_same_header(jb.hdr, sbe_bbo.hdr);
  CHECK(jb.bid_px == sbe_bbo.bid_px);
  CHECK(jb.bid_qty == sbe_bbo.bid_qty);
  CHECK(jb.ask_px == sbe_bbo.ask_px);
  CHECK(jb.ask_qty == sbe_bbo.ask_qty);
}

TEST_CASE("binance.sbe: truncated frames are malformed at every length") {
  TestUniverse u;
  BinanceSbeMdParser p(u.symbols, VenueId{0});
  for (const char* name : {"binance/sbe/depth_diff.hex",
                           "binance/sbe/best_bid_ask.hex",
                           "binance/sbe/trades.hex",
                           "binance/sbe/depth_snapshot20.hex"}) {
    const Bytes f = hex_fixture(name);
    for (std::size_t n = 0; n < f.size(); ++n) {
      INFO(name << " truncated to " << n);
      const Decoded d = decode(p, std::span<const std::byte>(f.data(), n));
      CHECK(d.status == ParseStatus::Malformed);
      CHECK(d.msgs.empty());
    }
    CHECK(decode(p, f).status == ParseStatus::Ok);
  }
}

TEST_CASE("binance.sbe: foreign schema, unknown template and unknown symbol") {
  TestUniverse u;
  BinanceSbeMdParser p(u.symbols, VenueId{0});
  Bytes f = hex_fixture("binance/sbe/best_bid_ask.hex");
  Bytes other_schema = f;
  sbe::store_le<std::uint16_t>(other_schema.data() + 4, 3);  // the spot REST/WS API schema
  CHECK(decode(p, other_schema).status == ParseStatus::Ignored);
  Bytes other_template = f;
  sbe::store_le<std::uint16_t>(other_template.data() + 2, 10099);
  CHECK(decode(p, other_template).status == ParseStatus::Ignored);
  Bytes other_symbol = f;
  other_symbol[other_symbol.size() - 1] = std::byte{'X'};  // BTCUSDX
  CHECK(decode(p, other_symbol).status == ParseStatus::UnknownSymbol);
  CHECK(p.stats().unknown_symbol == 1);
}

TEST_CASE("binance.sbe: a longer block from a newer schema version is skipped by blockLength") {
  TestUniverse u;
  BinanceSbeMdParser p(u.symbols, VenueId{0});
  const Bytes f = hex_fixture("binance/sbe/best_bid_ask.hex");
  // Insert 6 unknown bytes at the end of the root block (blockLength 50 -> 56, version 1).
  Bytes g(f.begin(), f.begin() + 8 + 50);
  g.insert(g.end(), 6, std::byte{0xEE});
  g.insert(g.end(), f.begin() + 8 + 50, f.end());
  sbe::store_le<std::uint16_t>(g.data(), 56);
  sbe::store_le<std::uint16_t>(g.data() + 6, 1);
  const Decoded d = decode(p, g);
  REQUIRE(d.status == ParseStatus::Ok);
  CHECK(d.as<BookTickerMsg>().ask_qty == qty("2"));
}

TEST_CASE("binance.sbe: inexact exponents are rejected, not rounded") {
  TestUniverse u;
  BinanceSbeMdParser p(u.symbols, VenueId{0});
  FrameBuf f;
  ss::BestBidAskStreamEventWriter w(f.body());
  w.set_price_exponent(-10);  // 1e-10 price steps: finer than the engine's 1e-8
  w.set_qty_exponent(-8);
  w.set_bid_price(123);
  w.set_bid_qty(1);
  w.set_ask_price(200);
  w.set_ask_qty(1);
  REQUIRE(w.set_symbol("BTCUSDT"));
  f.finish(w);
  CHECK(decode(p, f.view()).status == ParseStatus::Malformed);
  // The same exponent with an exactly representable mantissa is fine.
  FrameBuf g;
  ss::BestBidAskStreamEventWriter w2(g.body());
  w2.set_price_exponent(-10);
  w2.set_qty_exponent(0);
  w2.set_bid_price(700000000000000);  // 70000.0000000000
  w2.set_bid_qty(3);
  w2.set_ask_price(700001000000000);
  w2.set_ask_qty(4);
  REQUIRE(w2.set_symbol("BTCUSDT"));
  g.finish(w2);
  const Decoded d = decode(p, g.view());
  REQUIRE(d.status == ParseStatus::Ok);
  CHECK(d.as<BookTickerMsg>().bid_px == px("70000"));
  CHECK(d.as<BookTickerMsg>().ask_px == px("70000.1"));
  CHECK(d.as<BookTickerMsg>().ask_qty == Qty::from_int(4));
}

TEST_CASE("binance.sbe: feed selects SBE streams and syncs the book like the JSON feed") {
  struct Requests {
    std::vector<InstrumentId> ids;
    static void on_request(void* ctx, InstrumentId id) noexcept {
      static_cast<Requests*>(ctx)->ids.push_back(id);
    }
  };
  TestUniverse u;
  RecordingSink rs(4U << 20);
  Requests req;
  BinanceMdFeed feed(u.symbols,
                     VenueId{0},
                     rs.sink,
                     {&Requests::on_request, &req},
                     BinanceDepthSync::kDefaultMinInterval,
                     MdFormat::Sbe);
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  CHECK(feed.format() == MdFormat::Sbe);
  CHECK(feed.stream_target() == "/stream?streams=btcusdt@depth/btcusdt@bestBidAsk/btcusdt@trade");
  feed.on_connected();
  REQUIRE(req.ids.size() == 1);

  // Deltas before the snapshot are buffered; U <= L+1 <= u bridges the REST snapshot (L = 100).
  CHECK(feed.on_binary(depth_frame(95, 100, 6999900, 100000000).view(), 1) == ParseStatus::Ok);
  CHECK(feed.on_binary(depth_frame(101, 102, 7000000, 150000000).view(), 2) == ParseStatus::Ok);
  CHECK(rs.drain().empty());
  feed.on_snapshot_body(
      InstrumentId{0},
      PaddedJson(
          R"({"lastUpdateId":100,"bids":[["70000.00000000","1.00000000"]],"asks":[["70000.10000000","2.00000000"]]})")
          .view(),
      3);
  CHECK(feed.synced_count() == 1);
  auto out = rs.drain();
  REQUIRE(out.size() == 2);
  CHECK(RecordingSink::type_of(out[0]) == EventType::BookSnapshot);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[1]).first_update_id == 101);
  CHECK(RecordingSink::as<BookDeltaMsg>(out[1]).bids()[0].qty == qty("1.5"));

  // Trades and best bid/ask go straight to the sink; a trade frame can carry several trades.
  CHECK(feed.on_binary(hex_fixture("binance/sbe/trades.hex"), 4) == ParseStatus::Ok);
  CHECK(feed.on_binary(hex_fixture("binance/sbe/best_bid_ask.hex"), 5) == ParseStatus::Ok);
  out = rs.drain();
  REQUIRE(out.size() == 3);
  CHECK(RecordingSink::type_of(out[0]) == EventType::Trade);
  CHECK(RecordingSink::type_of(out[1]) == EventType::Trade);
  CHECK(RecordingSink::type_of(out[2]) == EventType::BookTicker);

  // A gap (U != last u + 1) resyncs through the rate-limited snapshot request.
  CHECK(feed.on_binary(depth_frame(150, 151, 7000000, 1).view(), 6) == ParseStatus::Ok);
  CHECK_FALSE(feed.synced_count() == 1);
  out = rs.drain();
  bool resyncing = false;
  for (const auto& m : out) {
    if (RecordingSink::type_of(m) == EventType::ConnectionState &&
        RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Resyncing)
      resyncing = true;
  }
  CHECK(resyncing);
  CHECK(feed.resync_count() >= 1);

  // Garbage counts as malformed and emits nothing.
  const std::byte junk[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
  CHECK(feed.on_binary(junk, 7) == ParseStatus::Malformed);
  CHECK(feed.stats().malformed == 1);
}
