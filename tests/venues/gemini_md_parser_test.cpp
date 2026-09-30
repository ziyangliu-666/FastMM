// Gemini WebSocket API public stream: the depthUpdate / bookTicker / trade decoder on frames
// recorded from production (tests/fixtures/gemini/raw_md_stream.jsonl, fastmm-live --dry-run
// --record-raw, 2026-09-30), the snapshot marking and the U/u chain of the feed.
#include "fastmm/venues/gemini/gemini_md_parser.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/gemini/gemini_md_feed.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::gemini;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kGemini{1};

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(make_instrument("BTCGUSDPERP", 1, "BTC", "GUSD")));  // id 0
    REQUIRE(instruments.add(make_instrument("ETHGUSDPERP", 1, "ETH", "GUSD")));  // id 1
    REQUIRE(instruments.add(make_instrument("BTCUSD", 1, "BTC", "USD")));        // id 2
    REQUIRE(symbols.build(instruments));
  }
};

Qty qty(const char* s) {
  return Qty::from_decimal(s).value();
}
Price px(const char* s) {
  return Price::from_decimal(s).value();
}

// The recorded frames, the receive stamp dropped.
std::vector<std::string> recorded() {
  const std::string raw = fastmm::test::fixture("gemini/raw_md_stream.jsonl");
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t end = raw.find('\n', pos);
    if (end == std::string::npos) end = raw.size();
    const std::string line = raw.substr(pos, end - pos);
    pos = end + 1;
    const std::size_t tab = line.find('\t');
    if (tab != std::string::npos) out.push_back(line.substr(tab + 1));
  }
  return out;
}

MdDecodeResult decode(GeminiMdParser& p, const std::string& frame, Scratch& s) {
  const PaddedJson j(frame);
  return p.decode(j.view(), Timestamp{1}, Cycles{}, s.span());
}

std::string depth(const char* sym, long long first, long long last, const char* b, const char* a) {
  return std::string(R"({"e":"depthUpdate","E":1790730770606322654,"s":")") + sym + R"(","U":)" +
         std::to_string(first) + R"(,"u":)" + std::to_string(last) + R"(,"b":[)" + b +
         R"(],"a":[)" + a + "]}";
}

struct Resubscribes {
  std::vector<InstrumentId> ids;
  static void fn(void* ctx, InstrumentId id) noexcept {
    static_cast<Resubscribes*>(ctx)->ids.push_back(id);
  }
};

}  // namespace

TEST_CASE("gemini.md_parser: every recorded production frame decodes") {
  Universe u;
  GeminiMdParser p(u.symbols, kGemini);
  Scratch s;
  std::size_t depth_frames = 0;
  std::size_t tickers = 0;
  std::size_t trades = 0;
  std::size_t replies = 0;
  const std::vector<std::string> frames = recorded();
  REQUIRE(frames.size() > 100);
  for (const std::string& f : frames) {
    const MdDecodeResult r = decode(p, f, s);
    INFO(f.substr(0, 200));
    if (r.control.present) {
      CHECK(r.control.status == 200);
      ++replies;
      continue;
    }
    REQUIRE(r.status == ParseStatus::Ok);
    switch (r.kind) {
      case MdKind::BookDelta: {
        const auto& m = s.as<BookDeltaMsg>();
        CHECK(m.first_update_id <= m.last_update_id);
        CHECK(m.prev_update_id == m.first_update_id);
        CHECK(m.hdr.exch_ts.ns > 1'790'000'000'000'000'000);
        ++depth_frames;
        break;
      }
      case MdKind::BookTicker: {
        const auto& m = s.as<BookTickerMsg>();
        CHECK(m.bid_px < m.ask_px);
        ++tickers;
        break;
      }
      case MdKind::Trade:
        ++trades;
        break;
      default:
        FAIL("unexpected kind");
    }
  }
  CHECK(p.stats().malformed == 0);
  CHECK(depth_frames > 50);
  CHECK(tickers > 20);
  CHECK(trades > 0);
  CHECK(replies >= 2);  // the subscribe and the time replies
  // The spot book's snapshot has more than 512 levels a side: kept nearest the touch.
  CHECK(p.stats().truncated >= 1);
  // It also lists asks at 1e10 and above, past the fixed point: left out, not a malformed frame.
  CHECK(p.stats().out_of_range >= 1);
}

TEST_CASE("gemini.md_parser: the recorded perpetual snapshot, bookTicker and time reply") {
  Universe u;
  GeminiMdParser p(u.symbols, kGemini);
  Scratch s;
  const std::vector<std::string> frames = recorded();
  // The first frame is the btcgusdperp snapshot: U == u, absolute levels, bids best first.
  REQUIRE(decode(p, frames[0], s).status == ParseStatus::Ok);
  const auto& snap = s.as<BookDeltaMsg>();
  CHECK(snap.hdr.instrument == InstrumentId{0});
  CHECK(snap.first_update_id == snap.last_update_id);
  CHECK(snap.bid_count > 5);
  CHECK(snap.ask_count > 5);
  CHECK(snap.bids()[0].price == px("83395"));
  CHECK(snap.bids()[0].qty == qty("0.144"));
  CHECK(snap.bids()[0].price > snap.bids()[1].price);
  CHECK(snap.asks()[0].price > snap.bids()[0].price);
  // A bookTicker: bid/ask as strings, `u` the update id.
  const std::string ticker =
      R"({"u":1764559652999672,"E":1790732602948771651,"s":"btcusd","b":"83361.25000","B":"0.0043000000","a":"83361.26000","A":"0.0599811200","c":"83355.92000","C":"0.00001"})";
  REQUIRE(decode(p, ticker, s).status == ParseStatus::Ok);
  const auto& t = s.as<BookTickerMsg>();
  CHECK(t.hdr.instrument == InstrumentId{2});
  CHECK(t.bid_px == px("83361.25"));
  CHECK(t.bid_qty == qty("0.0043"));
  CHECK(t.ask_px == px("83361.26"));
  CHECK(t.ask_qty == qty("0.05998112"));
  CHECK(t.hdr.venue_seq == 1764559652999672);
  CHECK(t.hdr.exch_ts == Timestamp{1790732602948771651});
  // A trade: `m` true means the buyer made, so the seller took.
  const std::string trade =
      R"({"E":1790732616897030070,"s":"btcusd","t":2840141030659933,"p":"83371.08000","q":"0.0000593800","m":true})";
  REQUIRE(decode(p, trade, s).status == ParseStatus::Ok);
  const auto& tr = s.as<TradeMsg>();
  CHECK(tr.price == px("83371.08"));
  CHECK(tr.qty == qty("0.00005938"));
  CHECK(tr.trade_id == 2840141030659933);
  CHECK(tr.aggressor == Side::Sell);
  // Replies.
  MdDecodeResult r =
      decode(p, R"({"id":"time","status":200,"result":{"serverTime":1790732602153}})", s);
  CHECK(r.control.present);
  CHECK(r.control.id == "time");
  CHECK(r.control.server_time_ms == 1790732602153);
  r = decode(
      p, R"({"id":7,"status":401,"error":{"code":-1002,"msg":"Authentication required"}})", s);
  CHECK(r.status == ParseStatus::Error);
  CHECK(r.control.id_number == 7);
  CHECK(r.control.error_code == -1002);
  CHECK(r.control.msg == "Authentication required");
  // Unknown symbols and unreadable frames.
  CHECK(decode(p, depth("solgusdperp", 1, 2, "", ""), s).status == ParseStatus::UnknownSymbol);
  CHECK(decode(p, R"({"e":"depthUpdate","s":"btcgusdperp","U":1,"u":2,"b":[["x","1"]],"a":[]})", s)
            .status == ParseStatus::Malformed);
}

TEST_CASE("gemini.md_feed: the first frame is the snapshot, U/u chain, gap resubscribes") {
  Universe u;
  RecordingSink sink(8U << 20);
  Resubscribes rs;
  GeminiMdFeed feed(u.symbols, kGemini, sink.sink, InstrumentCallback{&Resubscribes::fn, &rs}, 0);
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  REQUIRE(feed.subscription_payloads().size() == 1);
  CHECK(
      feed.subscription_payloads()[0] ==
      R"({"id":"md","method":"subscribe","params":["btcgusdperp@depth@100ms","btcgusdperp@bookTicker","btcgusdperp@trade"]})");
  feed.on_connected();
  auto feed_text = [&](const std::string& t) {
    const PaddedJson j(t);
    return feed.on_message(j.view(), 1);
  };
  // Snapshot (U == u), then diffs overlapping at U == previous u.
  CHECK(feed_text(depth("btcgusdperp", 100, 100, R"(["60000","1"])", R"(["60001","2"])")) ==
        ParseStatus::Ok);
  CHECK(feed.synced_count() == 1);
  CHECK(feed_text(depth("btcgusdperp", 100, 140, R"(["60000","0"])", "")) == ParseStatus::Ok);
  CHECK(feed_text(depth("btcgusdperp", 140, 141, "", R"(["60002","1"])")) == ParseStatus::Ok);
  // A stale repeat is dropped without a resync.
  CHECK(feed_text(depth("btcgusdperp", 120, 140, "", "")) == ParseStatus::Ok);
  CHECK(feed.resync_count() == 0);
  auto msgs = sink.drain();
  REQUIRE(msgs.size() == 3);
  CHECK(RecordingSink::type_of(msgs[0]) == EventType::BookSnapshot);
  CHECK(RecordingSink::as<BookDeltaMsg>(msgs[0]).is_snapshot());
  CHECK(RecordingSink::type_of(msgs[1]) == EventType::BookDelta);
  CHECK(RecordingSink::type_of(msgs[2]) == EventType::BookDelta);
  // U skips ahead of the last u: a gap, Resyncing, and a resubscription.
  CHECK(feed_text(depth("btcgusdperp", 150, 160, "", "")) == ParseStatus::Ok);
  CHECK(feed.resync_count() == 1);
  CHECK(feed.synced_count() == 0);
  REQUIRE(rs.ids.size() == 1);
  const auto payloads = feed.resubscribe_payloads(InstrumentId{0});
  REQUIRE(payloads.size() == 2);
  CHECK(payloads[0] ==
        R"({"id":"unsub-0","method":"unsubscribe","params":["btcgusdperp@depth@100ms"]})");
  CHECK(payloads[1] ==
        R"({"id":"resub-0","method":"subscribe","params":["btcgusdperp@depth@100ms"]})");
  // Frames of the old subscription until the unsubscribe's reply are dropped...
  CHECK(feed_text(depth("btcgusdperp", 160, 170, "", "")) == ParseStatus::Ignored);
  CHECK(feed.stats().superseded == 1);
  CHECK(feed_text(R"({"id":"unsub-0","status":200})") == ParseStatus::Ignored);
  CHECK(feed_text(R"({"id":"resub-0","status":200})") == ParseStatus::Ignored);
  // ...and the next depth frame is the new snapshot.
  CHECK(feed_text(depth("btcgusdperp", 175, 175, R"(["60000","3"])", R"(["60001","1"])")) ==
        ParseStatus::Ok);
  CHECK(feed.synced_count() == 1);
  CHECK(feed_text(depth("btcgusdperp", 175, 180, "", "")) == ParseStatus::Ok);
  msgs = sink.drain();
  std::size_t snapshots = 0;
  bool resyncing = false;
  for (const auto& m : msgs) {
    snapshots += RecordingSink::type_of(m) == EventType::BookSnapshot ? 1U : 0U;
    resyncing =
        resyncing || (RecordingSink::type_of(m) == EventType::ConnectionState &&
                      RecordingSink::as<ConnectionStateMsg>(m).state == ConnState::Resyncing);
  }
  CHECK(snapshots == 1);
  CHECK(resyncing);
  CHECK(RecordingSink::type_of(msgs.back()) == EventType::BookDelta);
}

TEST_CASE("gemini.md_feed: the recorded stream syncs every book with no resync") {
  Universe u;
  RecordingSink sink(32U << 20);
  Resubscribes rs;
  GeminiMdFeed feed(u.symbols, kGemini, sink.sink, InstrumentCallback{&Resubscribes::fn, &rs});
  for (std::uint16_t i = 0; i < 3; ++i) REQUIRE(feed.add_instrument(InstrumentId{i}));
  feed.on_connected();
  for (const std::string& f : recorded()) {
    const PaddedJson j(f);
    static_cast<void>(feed.on_message(j.view(), 1));
    static_cast<void>(sink.drain());
  }
  CHECK(feed.synced_count() == 3);
  CHECK(feed.resync_count() == 0);
  CHECK(feed.stats().snapshots == 3);
  CHECK(feed.stats().malformed == 0);
  CHECK(rs.ids.empty());
}
