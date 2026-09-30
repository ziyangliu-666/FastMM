// OKX mark-price, index-tickers, funding-rate and open-interest do not allocate after warm-up: the
// parser and the feed (parser, sink) on the recorded production frames
// (tests/fixtures/okx/raw_perp_stream.jsonl). The first pass is the warm-up; the measured rounds
// must then decode everything without a single allocation.
#if defined(FASTMM_HOTPATH_VENUES)

#include "../venues/venue_test_util.hpp"
#include "alloc_counter.hpp"

#include "fastmm/venues/okx/okx_md_feed.hpp"
#include "fastmm/venues/okx/okx_md_parser.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::test::NoAllocScope;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::Scratch;

namespace {

constexpr int kRounds = 200;

// The data pushes of the recorded stream (the subscribe acks left out).
std::vector<PaddedJson> perp_frames() {
  const std::string raw = fastmm::test::fixture("okx/raw_perp_stream.jsonl");
  std::vector<PaddedJson> out;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t end = raw.find('\n', pos);
    if (end == std::string::npos) end = raw.size();
    const std::string line = raw.substr(pos, end - pos);
    pos = end + 1;
    const std::size_t tab = line.find('\t');
    if (tab != std::string::npos && line.find("\"data\"") != std::string::npos)
      out.emplace_back(line.substr(tab + 1));
  }
  return out;
}

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(make_instrument("BTC-USDT-SWAP", 1, "BTC", "USDT")));
    REQUIRE(instruments.add(make_instrument("ETH-USDT-SWAP", 1, "ETH", "USDT")));
    REQUIRE(symbols.build(instruments));
  }
};

}  // namespace

TEST_CASE("hotpath.noalloc: OKX mark, index, funding and open interest in the market-data parser") {
  Universe u;
  const std::vector<PaddedJson> in = perp_frames();
  REQUIRE(in.size() == 22);
  okx::OkxMdParser parser(u.symbols, VenueId{1});
  parser.add_index("BTC-USDT", InstrumentId{0});
  parser.add_index("ETH-USDT", InstrumentId{1});
  Scratch s;
  for (const PaddedJson& f : in) {
    const auto r = parser.decode(f.view(), Timestamp{1}, Cycles{1}, s.span());
    REQUIRE(r.ok());
    REQUIRE(r.kind == MdKind::PerpState);
  }
  int failed = 0;
  {
    NoAllocScope guard;
    for (int round = 0; round < kRounds; ++round) {
      for (const PaddedJson& f : in) {
        if (!parser.decode(f.view(), Timestamp{round}, Cycles{1}, s.span()).ok()) ++failed;
      }
    }
  }
  CHECK(failed == 0);
  CHECK(parser.stats().perp_states == 22U * (kRounds + 1));
}

TEST_CASE("hotpath.noalloc: OKX mark, index, funding and open interest through the feed") {
  Universe u;
  const std::vector<PaddedJson> in = perp_frames();
  REQUIRE(in.size() == 22);
  fastmm::venues::test::RecordingSink sink(8U << 20);
  okx::OkxMdFeed feed(u.symbols, VenueId{1}, sink.sink, okx::ResubscribeRequester{});
  REQUIRE(feed.add_instrument(InstrumentId{0}));
  REQUIRE(feed.add_instrument(InstrumentId{1}));
  feed.on_connected();
  for (const PaddedJson& f : in) REQUIRE(feed.on_message(f.view(), 1) == ParseStatus::Ok);
  static_cast<void>(sink.drain());
  int failed = 0;
  {
    NoAllocScope guard;
    for (int round = 0; round < kRounds; ++round) {
      for (const PaddedJson& f : in) {
        if (feed.on_message(f.view(), 1) != ParseStatus::Ok) ++failed;
      }
      while (sink.ring.try_peek() != nullptr) sink.ring.release();
    }
  }
  CHECK(failed == 0);
  CHECK(feed.stats().dropped == 0);
  CHECK(feed.stats().pushed == 22U * (kRounds + 1));
}

#endif  // FASTMM_HOTPATH_VENUES
