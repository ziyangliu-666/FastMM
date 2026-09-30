// Gemini perpetual mark and funding on the WebSocket API: `{symbol}@markPrice` and
// `{symbol}@fundingAmount` frames recorded on production (tests/fixtures/gemini/
// raw_perp_stream.jsonl, fastmm-live --dry-run --record-raw, see fixtures.meta.json) become
// PerpStateMsg; the feed subscribes them for perpetuals only, in a request of their own.
#include "venue_test_util.hpp"

#include "fastmm/core/perp_book.hpp"
#include "fastmm/venues/gemini/gemini_md_feed.hpp"
#include "fastmm/venues/gemini/gemini_md_parser.hpp"
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
    Instrument perp = make_instrument("BTCGUSDPERP", 1, "BTC", "GUSD");  // id 0
    perp.asset_class = AssetClass::Perpetual;
    REQUIRE(instruments.add(perp));
    REQUIRE(instruments.add(make_instrument("BTCUSD", 1, "BTC", "USD")));  // id 1
    REQUIRE(symbols.build(instruments));
  }
};

Price px(const char* s) {
  return Price::from_decimal(s).value_or(Price{});
}

// The recorded frames, the receive stamp dropped.
std::vector<std::string> recorded() {
  const std::string raw = fastmm::test::fixture("gemini/raw_perp_stream.jsonl");
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
std::string first_with(const std::string& needle) {
  for (const std::string& f : recorded())
    if (f.find(needle) != std::string::npos) return f;
  FAIL("no recorded frame with " << needle);
  return {};
}
std::string replaced(std::string text, const std::string& from, const std::string& to) {
  const std::size_t at = text.find(from);
  REQUIRE(at != std::string::npos);
  text.replace(at, from.size(), to);
  return text;
}

MdDecodeResult decode(GeminiMdParser& p, const std::string& frame, Scratch& s) {
  const PaddedJson j(frame);
  return p.decode(j.view(), Timestamp{42}, Cycles{7}, s.span());
}

}  // namespace

TEST_CASE("gemini.perp_state: a markPrice frame is the venue mark at E") {
  Universe u;
  GeminiMdParser p(u.symbols, kGemini);
  Scratch s;
  // {"e":"markPrice","E":1790757351404791988,"s":"btcgusdperp","p":"83060.862","i":"8301506.333"}
  const MdDecodeResult r = decode(p, first_with(R"("e":"markPrice")"), s);
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::PerpState);
  CHECK(r.count == 1);
  CHECK(r.len == sizeof(PerpStateMsg));
  const auto& m = s.as<PerpStateMsg>();
  CHECK(m.hdr.type == EventType::PerpState);
  CHECK(m.hdr.len == sizeof(PerpStateMsg));
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == kGemini);
  CHECK(m.hdr.exch_ts.ns == 1790757351404791988LL);
  CHECK(m.hdr.recv_ts.ns == 42);
  CHECK(m.hdr.t0_cycles.v == 7);
  CHECK(m.fields == PerpStateMsg::kMark);  // `i` (a scaled index) is not reported
  CHECK(m.mark_price == px("83060.862"));
  CHECK(p.stats().marks == 1);
}

TEST_CASE("gemini.perp_state: a fundingAmount estimate is the rate f / p per hour, due at T") {
  Universe u;
  GeminiMdParser p(u.symbols, kGemini);
  Scratch s;
  // {"e":"fundingAmount","E":1790758800000000000,"s":"btcgusdperp","T":1790758800000000000,
  //  "i":60,"f":"5.77329","r":"0.00600","p":"83067.436","R":false}
  const std::string frame = first_with(R"("e":"fundingAmount")");
  const MdDecodeResult r = decode(p, frame, s);
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::PerpState);
  const auto& m = s.as<PerpStateMsg>();
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.fields == PerpStateMsg::kFunding);
  CHECK(m.funding_rate == doctest::Approx(5.77329 / 83067.436).epsilon(1e-12));
  CHECK(m.funding_rate * 100 == doctest::Approx(0.006).epsilon(0.2));  // r, cut to 0.001 %
  CHECK(m.funding_interval == seconds(3600));
  CHECK(m.next_funding.ns == 1790758800000000000LL);
  CHECK_FALSE(m.hdr.exch_ts.valid());  // E is the funding time, not the estimate's
  CHECK(p.stats().fundings == 1);

  // Negative amounts: shorts pay longs.
  REQUIRE(decode(p, replaced(frame, R"("f":"5.77329")", R"("f":"-1.5")"), s).ok());
  CHECK(s.as<PerpStateMsg>().funding_rate == doctest::Approx(-1.5 / 83067.436).epsilon(1e-12));

  // A realized amount is the payment at T, not the next estimate.
  CHECK(decode(p, replaced(frame, R"("R":false)", R"("R":true)"), s).status ==
        ParseStatus::Ignored);
  CHECK(p.stats().funding_realized == 1);

  CHECK(decode(p, replaced(frame, R"("T":1790758800000000000,)", ""), s).status ==
        ParseStatus::Malformed);
  CHECK(decode(p, replaced(frame, R"("p":"83067.436")", R"("p":"0")"), s).status ==
        ParseStatus::Malformed);
  CHECK(decode(p, replaced(frame, R"("i":60)", R"("i":"60")"), s).status ==
        ParseStatus::Malformed);  // no interval
  CHECK(p.stats().fundings == 2);
}

TEST_CASE("gemini.md_feed: perpetuals subscribe markPrice and fundingAmount on their own") {
  Universe u;
  RecordingSink sink(1U << 20);
  GeminiMdFeed feed(u.symbols, kGemini, sink.sink, InstrumentCallback{});
  REQUIRE(feed.add_instrument(InstrumentId{0}, true));
  REQUIRE(feed.add_instrument(InstrumentId{1}));
  REQUIRE(feed.subscription_payloads().size() == 2);
  CHECK(
      feed.subscription_payloads()[0] ==
      R"({"id":"md","method":"subscribe","params":["btcgusdperp@depth@100ms","btcgusdperp@bookTicker","btcgusdperp@trade","btcusd@depth@100ms","btcusd@bookTicker","btcusd@trade"]})");
  CHECK(
      feed.subscription_payloads()[1] ==
      R"({"id":"perp","method":"subscribe","params":["btcgusdperp@markPrice","btcgusdperp@fundingAmount"]})");

  GeminiMdFeed spot(u.symbols, kGemini, sink.sink, InstrumentCallback{});
  REQUIRE(spot.add_instrument(InstrumentId{1}));
  CHECK(spot.subscription_payloads().size() == 1);
}

TEST_CASE("gemini.md_feed: the recorded mark and funding frames reach the sink and a PerpBook") {
  Universe u;
  RecordingSink sink(1U << 20);
  GeminiMdFeed feed(u.symbols, kGemini, sink.sink, InstrumentCallback{});
  REQUIRE(feed.add_instrument(InstrumentId{0}, true));
  feed.on_connected();
  std::size_t marks = 0;
  std::size_t fundings = 0;
  for (const std::string& f : recorded()) {
    const PaddedJson j(f);
    const ParseStatus st = feed.on_message(j.view(), 1);
    if (f.find(R"("e":"markPrice")") != std::string::npos) {
      CHECK(st == ParseStatus::Ok);
      ++marks;
    } else if (f.find(R"("e":"fundingAmount")") != std::string::npos) {
      CHECK(st == ParseStatus::Ok);
      ++fundings;
    } else {
      CHECK(st == ParseStatus::Ignored);  // the subscribe replies
    }
  }
  CHECK(marks > 20);
  CHECK(fundings >= 2);
  CHECK(feed.stats().malformed == 0);
  CHECK(feed.stats().request_errors == 0);

  PerpBook book;
  const Timestamp now{1'000'000'000};
  std::size_t reports = 0;
  const PerpStateMsg* last_mark = nullptr;
  const PerpStateMsg* last_funding = nullptr;
  const auto all = sink.drain();
  for (const auto& raw : all) {
    REQUIRE(RecordingSink::type_of(raw) == EventType::PerpState);
    const auto& m = RecordingSink::as<PerpStateMsg>(raw);
    static_cast<void>(book.on_report(m, now));
    ++reports;
    if ((m.fields & PerpStateMsg::kMark) != 0) last_mark = &m;
    if ((m.fields & PerpStateMsg::kFunding) != 0) last_funding = &m;
  }
  CHECK(reports == marks + fundings);
  REQUIRE(last_mark != nullptr);
  REQUIRE(last_funding != nullptr);
  const RefPrice mark = book.mark(InstrumentId{0}, now);
  CHECK(mark.usable());
  CHECK(mark.price == last_mark->mark_price);
  CHECK_FALSE(book.index(InstrumentId{0}, now).at.valid());  // no index on this API
  const FundingView fv = book.funding(InstrumentId{0}, now);
  CHECK(fv.usable());
  CHECK(fv.rate == last_funding->funding_rate);
  CHECK(fv.interval == seconds(3600));
  CHECK(fv.next.ns == 1790758800000000000LL);
  CHECK(fv.over(seconds(std::int64_t{8} * 3600)) == doctest::Approx(fv.rate * 8));
}
