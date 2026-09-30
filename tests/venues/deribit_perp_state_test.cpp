// Deribit perpetual and future tickers -> PerpStateMsg, on frames recorded on production
// (tests/fixtures/deribit/*_prod.json and raw_perp_ticker_stream.jsonl, see fixtures.meta.json):
// every field, the units (USD open interest to contracts, current_funding per 8 h), the venue
// time; options keep their OptionTickerMsg and carry no PerpStateMsg; the feed pushes the messages
// and a PerpBook fed from them reports the last values.
#include "fake_venue_util.hpp"

#include "fastmm/core/perp_book.hpp"
#include "fastmm/venues/deribit/deribit_md_feed.hpp"
#include "fastmm/venues/deribit/deribit_md_parser.hpp"

#include <cmath>
#include <sstream>
#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::deribit;
using namespace fastmm::venues::test;

namespace {

constexpr VenueId kVenue{2};
constexpr Duration kEightHours = seconds(std::int64_t{8} * 3600);

Price px(const char* s) {
  return Price::from_decimal(s).value_or(Price{});
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value_or(Qty{});
}

// Ids: 0 BTC-PERPETUAL (10 USD contracts), 1 ETH-PERPETUAL (1 USD), 2 BTC-25DEC26 (future, 10 USD),
// 3 BTC-25DEC26-76000-C (option, 1 BTC).
struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    Instrument btc = make_instrument("BTC-PERPETUAL", kVenue.value, "BTC", "USD");
    btc.asset_class = AssetClass::Perpetual;
    btc.tick = px("0.5");
    btc.lot = qt("1");
    btc.contract_multiplier = Qty::from_int(10);
    REQUIRE(instruments.add(btc));
    Instrument eth = make_instrument("ETH-PERPETUAL", kVenue.value, "ETH", "USD");
    eth.asset_class = AssetClass::Perpetual;
    eth.tick = px("0.05");
    eth.lot = qt("1");
    eth.contract_multiplier = Qty::from_int(1);
    REQUIRE(instruments.add(eth));
    Instrument fut = make_instrument("BTC-25DEC26", kVenue.value, "BTC", "USD");
    fut.asset_class = AssetClass::Future;
    fut.tick = px("2.5");
    fut.lot = qt("1");
    fut.contract_multiplier = Qty::from_int(10);
    REQUIRE(instruments.add(fut));
    Instrument call = make_instrument("BTC-25DEC26-76000-C", kVenue.value, "BTC", "BTC");
    call.asset_class = AssetClass::Option;
    call.option_type = OptionType::Call;
    call.tick = px("0.0001");
    call.lot = qt("0.1");
    REQUIRE(instruments.add(call));
    REQUIRE(symbols.build(instruments));
  }
};

MdDecodeResult decode_text(DeribitMdParser& p, const std::string& text, Scratch& s) {
  const PaddedJson j(text);
  return p.decode(j.view(), Timestamp{42}, Cycles{7}, s.span());
}
MdDecodeResult decode(DeribitMdParser& p, const std::string& fixture, Scratch& s) {
  return decode_text(p, fastmm::test::fixture(fixture), s);
}
const PerpStateMsg& perp_after_ticker(const Scratch& s) {
  return *reinterpret_cast<const PerpStateMsg*>(s.buf + sizeof(BookTickerMsg));
}
std::string replaced(std::string text, const std::string& from, const std::string& to) {
  const std::size_t at = text.find(from);
  REQUIRE(at != std::string::npos);
  text.replace(at, from.size(), to);
  return text;
}

}  // namespace

TEST_CASE("deribit.perp_state: BTC-PERPETUAL ticker carries mark, index, funding, open interest") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const MdDecodeResult r = decode(p, "deribit/ticker_perp_btc_prod.json", s);
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::BookTicker);
  REQUIRE(r.count == 2);
  CHECK(r.len == sizeof(BookTickerMsg) + sizeof(PerpStateMsg));
  const auto& bt = s.as<BookTickerMsg>();
  CHECK(bt.hdr.type == EventType::BookTicker);
  CHECK(bt.bid_px == px("83212"));
  CHECK(bt.bid_qty == qt("7175"));  // 71750 USD / 10 USD contracts
  const PerpStateMsg& m = perp_after_ticker(s);
  CHECK(m.hdr.type == EventType::PerpState);
  CHECK(m.hdr.len == sizeof(PerpStateMsg));
  CHECK(m.hdr.instrument == InstrumentId{0});
  CHECK(m.hdr.venue == kVenue);
  CHECK(m.hdr.exch_ts.ns == 1790755218559LL * 1'000'000);
  CHECK(m.hdr.recv_ts.ns == 42);
  CHECK(m.hdr.t0_cycles.v == 7);
  CHECK(m.fields == (PerpStateMsg::kMark | PerpStateMsg::kIndex | PerpStateMsg::kFunding |
                     PerpStateMsg::kOpenInterest));
  CHECK(m.mark_price == px("83212.14"));
  CHECK(m.index_price == px("83251.03"));
  CHECK(m.funding_rate == doctest::Approx(-2.1714e-4).epsilon(1e-12));  // current_funding
  CHECK(m.funding_interval == kEightHours);
  CHECK_FALSE(m.next_funding.valid());       // continuous funding
  CHECK(m.open_interest == qt("79757813"));  // 797578130 USD / 10 USD contracts
  CHECK(p.stats().perp_states == 1);
  CHECK(p.stats().book_tickers == 1);
  CHECK(p.stats().option_tickers == 0);
}

TEST_CASE("deribit.perp_state: a zero funding rate is reported and 1 USD contracts stay USD") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const MdDecodeResult r = decode(p, "deribit/ticker_perp_eth_prod.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);
  const PerpStateMsg& m = perp_after_ticker(s);
  CHECK(m.hdr.instrument == InstrumentId{1});
  CHECK(m.hdr.exch_ts.ns == 1790755218539LL * 1'000'000);
  CHECK((m.fields & PerpStateMsg::kFunding) != 0);
  CHECK(m.funding_rate == 0.0);
  CHECK(m.funding_interval == kEightHours);
  CHECK(m.mark_price == px("2672.5"));
  CHECK(m.index_price == px("2672.32"));
  CHECK(m.open_interest == qt("234968698"));
}

TEST_CASE("deribit.perp_state: a future carries mark, index and open interest, no funding") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const MdDecodeResult r = decode(p, "deribit/ticker_future_prod.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);
  const PerpStateMsg& m = perp_after_ticker(s);
  CHECK(m.hdr.type == EventType::PerpState);
  CHECK(m.hdr.instrument == InstrumentId{2});
  CHECK(m.hdr.exch_ts.ns == 1790755218589LL * 1'000'000);
  CHECK(m.fields == (PerpStateMsg::kMark | PerpStateMsg::kIndex | PerpStateMsg::kOpenInterest));
  CHECK(m.mark_price == px("84222.65"));
  CHECK(m.index_price == px("83251.03"));
  CHECK(m.open_interest == qt("31177984"));
  CHECK(m.funding_interval.ns == 0);
}

TEST_CASE("deribit.perp_state: an option ticker yields OptionTicker and no PerpState") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const MdDecodeResult r = decode(p, "deribit/ticker_option_prod.json", s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);
  CHECK(r.len == sizeof(BookTickerMsg) + sizeof(OptionTickerMsg));
  const auto& ot = *reinterpret_cast<const OptionTickerMsg*>(s.buf + sizeof(BookTickerMsg));
  CHECK(ot.hdr.type == EventType::OptionTicker);
  CHECK(ot.hdr.instrument == InstrumentId{3});
  CHECK(ot.mark_price == px("0.1296"));
  CHECK(ot.index_price == px("83250.78"));
  CHECK(ot.underlying_price == px("84222.23"));
  CHECK(ot.mark_iv == doctest::Approx(0.3825));
  CHECK(ot.delta == doctest::Approx(0.74092));
  CHECK(ot.interest_rate == doctest::Approx(0.0));
  CHECK(p.stats().option_tickers == 1);
  CHECK(p.stats().perp_states == 0);
}

TEST_CASE("deribit.perp_state: missing funding is left out, a bad open interest is malformed") {
  Universe u;
  DeribitMdParser p(u.symbols, u.instruments, kVenue);
  Scratch s;
  const std::string btc = fastmm::test::fixture("deribit/ticker_perp_btc_prod.json");
  MdDecodeResult r = decode_text(p, replaced(btc, R"("current_funding":-2.1714e-4,)", ""), s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);
  const PerpStateMsg& m = perp_after_ticker(s);
  CHECK(m.fields == (PerpStateMsg::kMark | PerpStateMsg::kIndex | PerpStateMsg::kOpenInterest));
  CHECK(m.mark_price == px("83212.14"));

  r = decode_text(p, replaced(btc, R"("open_interest":797578130,)", ""), s);
  REQUIRE(r.ok());
  CHECK(perp_after_ticker(s).fields ==
        (PerpStateMsg::kMark | PerpStateMsg::kIndex | PerpStateMsg::kFunding));

  r = decode_text(p, replaced(btc, R"("open_interest":797578130)", R"("open_interest":"x")"), s);
  CHECK(r.status == ParseStatus::Malformed);
  CHECK(r.count == 0);
  CHECK(p.stats().perp_states == 2);
  CHECK(p.stats().malformed == 1);
}

TEST_CASE("deribit.md_feed: the recorded production tickers reach the sink and a PerpBook") {
  Universe u;
  RecordingSink md(4U << 20);
  DeribitMdFeed feed(u.symbols,
                     u.instruments,
                     kVenue,
                     md.sink,
                     ResubscribeRequester{[](void*, InstrumentId) noexcept {}, nullptr});
  for (std::uint32_t i = 0; i < 4; ++i) REQUIRE(feed.add_instrument(InstrumentId{i}));
  REQUIRE(feed.subscription_payloads().size() == 1);
  CHECK(feed.subscription_payloads()[0].find("ticker.BTC-PERPETUAL.100ms") != std::string::npos);
  feed.on_connected();
  std::istringstream lines(fastmm::test::fixture("deribit/raw_perp_ticker_stream.jsonl"));
  std::string line;
  std::size_t frames = 0;
  while (std::getline(lines, line)) {
    const PaddedJson j(line);
    CHECK(feed.on_message(j.view(), 1) == ParseStatus::Ok);
    ++frames;
  }
  CHECK(frames == 63);
  CHECK(feed.stats().dropped == 0);
  Collected c;
  c.take(md);
  // 18 BTC-PERPETUAL, 13 ETH-PERPETUAL, 22 BTC-25DEC26 and 10 option tickers.
  CHECK(c.count(EventType::BookTicker) == 63);
  CHECK(c.count(EventType::PerpState) == 53);
  CHECK(c.count(EventType::OptionTicker) == 10);
  CHECK(feed.stats().pushed == 63 + 53 + 10);
  CHECK(c.first_if<PerpStateMsg>(EventType::PerpState, [](const PerpStateMsg& m) {
    return m.hdr.instrument == InstrumentId{3};
  }) == nullptr);

  PerpBook book;
  const Timestamp now{1'000'000'000};
  for (const auto& raw : c.all) {
    if (RecordingSink::type_of(raw) == EventType::PerpState)
      static_cast<void>(book.on_report(RecordingSink::as<PerpStateMsg>(raw), now));
  }
  CHECK(book.instruments() == 3);
  const RefPrice btc = book.mark(InstrumentId{0}, now);
  CHECK(btc.usable());
  CHECK(btc.price == px("83211.85"));  // the last BTC-PERPETUAL ticker of the recording
  CHECK(book.index(InstrumentId{0}, now).price == px("83250.78"));
  const FundingView f = book.funding(InstrumentId{0}, now);
  CHECK(f.usable());
  CHECK(f.rate == doctest::Approx(-0.00021762).epsilon(1e-12));
  CHECK(f.over(seconds(3600)) == doctest::Approx(-0.00021762 / 8).epsilon(1e-12));
  CHECK(book.row(InstrumentId{0}).venue_ts.ns == 1790755226284LL * 1'000'000);
  CHECK(book.row(InstrumentId{0}).open_interest == qt("79757813"));
  CHECK(book.mark(InstrumentId{1}, now).price == px("2672.63"));
  CHECK(book.mark(InstrumentId{2}, now).price == px("84222.88"));
  CHECK_FALSE(book.funding(InstrumentId{2}, now).at.valid());  // futures carry no funding
  CHECK_FALSE(book.mark(InstrumentId{3}, now).at.valid());
  CHECK(book.mark(InstrumentId{0}, now + seconds(16)).stale);
}
