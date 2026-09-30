// The Binance USDⓈ-M mark price path does not allocate after warm-up: recorded markPriceUpdate
// frames through the parser (PerpStateMsg, funding interval and rollover tracking) and through the
// market-data feed onto the sink.
#if defined(FASTMM_HOTPATH_VENUES)

#include "../venues/venue_test_util.hpp"
#include "alloc_counter.hpp"

#include "fastmm/venues/binance_usdm/binance_usdm_md_feed.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"

#include <sstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::test::NoAllocScope;
using fastmm::venues::test::padded_fixture;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;
using fastmm::venues::test::TestUniverse;

namespace {

Duration hours(std::int64_t h) {
  return seconds(h * 3600);
}

constexpr int kRounds = 200;

// The recorded frames of BTCUSDT and ETHUSDT (the rollover recording has LPTUSDT too, which the
// test universe does not list).
std::vector<PaddedJson> mark_frames() {
  std::vector<PaddedJson> out;
  out.push_back(padded_fixture("binance_usdm/mark_price.json"));
  std::istringstream in(fastmm::test::fixture("binance_usdm/mark_price_rollover.jsonl"));
  for (std::string l; std::getline(in, l);) {
    if (l.find("lptusdt") == std::string::npos && !l.empty()) out.emplace_back(l);
  }
  return out;
}

struct NoRequests {
  static void on_request(void*, InstrumentId) noexcept {}
};

}  // namespace

TEST_CASE("binance_usdm.perp: markPrice decoding does not allocate") {
  TestUniverse u;
  binance_usdm::BinanceUsdmMdParser p(u.symbols, VenueId{0});
  p.set_funding_interval(InstrumentId{0}, hours(4));
  const std::vector<PaddedJson> in = mark_frames();
  REQUIRE(in.size() > 2);
  Scratch s;
  for (const PaddedJson& f : in) {
    const DecodeResult r = p.decode(f.view(), Timestamp{1}, Cycles{1}, s.span());
    REQUIRE(r.ok());
    REQUIRE(r.kind == MdKind::PerpState);
  }
  int failed = 0;
  {
    NoAllocScope guard;
    for (int round = 0; round < kRounds; ++round) {
      for (const PaddedJson& f : in) {
        const DecodeResult r = p.decode(f.view(), Timestamp{round}, Cycles{1}, s.span());
        if (!r.ok() || r.kind != MdKind::PerpState) ++failed;
      }
    }
  }
  CHECK(failed == 0);
}

TEST_CASE("binance_usdm.perp: markPrice through the feed onto the sink does not allocate") {
  TestUniverse u;
  RecordingSink rs(64U << 20);
  binance_usdm::BinanceUsdmMdFeed feed(
      u.symbols, VenueId{0}, rs.sink, {&NoRequests::on_request, nullptr}, 0);
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  REQUIRE(feed.add_instrument(InstrumentId{1}));
  REQUIRE(feed.add_perpetual(InstrumentId{0}, hours(8)));
  REQUIRE(feed.add_perpetual(InstrumentId{1}, hours(8)));
  const std::vector<PaddedJson> in = mark_frames();
  for (const PaddedJson& f : in) REQUIRE(feed.on_message(f.view(), 1) == ParseStatus::Ok);
  int failed = 0;
  {
    NoAllocScope guard;
    for (int round = 0; round < kRounds; ++round) {
      for (const PaddedJson& f : in) {
        if (feed.on_message(f.view(), round) != ParseStatus::Ok) ++failed;
      }
    }
  }
  CHECK(failed == 0);
  CHECK(feed.stats().pushed == in.size() * (kRounds + 1));
  CHECK(feed.stats().dropped == 0);
}

#endif  // FASTMM_HOTPATH_VENUES
