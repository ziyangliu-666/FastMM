// Gemini's perpetual mark and funding frames (`@markPrice`, `@fundingAmount`) do not allocate
// after warm-up: the market-data parser and the feed on the frames recorded on production
// (tests/fixtures/gemini/raw_perp_stream.jsonl).
#if defined(FASTMM_HOTPATH_VENUES)

#include "../venues/venue_test_util.hpp"
#include "alloc_counter.hpp"

#include "fastmm/venues/gemini/gemini_md_feed.hpp"
#include "fastmm/venues/gemini/gemini_md_parser.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::test::NoAllocScope;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::Scratch;

namespace {

constexpr int kGeminiPerpRounds = 200;

std::vector<PaddedJson> perp_stream() {
  const std::string raw = fastmm::test::fixture("gemini/raw_perp_stream.jsonl");
  std::vector<PaddedJson> out;
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t end = raw.find('\n', pos);
    if (end == std::string::npos) end = raw.size();
    const std::string line = raw.substr(pos, end - pos);
    pos = end + 1;
    const std::size_t tab = line.find('\t');
    if (tab != std::string::npos) out.emplace_back(line.substr(tab + 1));
  }
  return out;
}

}  // namespace

TEST_CASE("hotpath.noalloc: Gemini perpetual mark and funding frames") {
  InstrumentTable instruments;
  Instrument perp = make_instrument("BTCGUSDPERP", 1, "BTC", "GUSD");
  perp.asset_class = AssetClass::Perpetual;
  REQUIRE(instruments.add(perp));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  const std::vector<PaddedJson> in = perp_stream();
  REQUIRE(in.size() > 20);
  {
    gemini::GeminiMdParser md(symbols, VenueId{1});
    Scratch s;
    for (const PaddedJson& f : in)
      static_cast<void>(md.decode(f.view(), Timestamp{1}, Cycles{1}, s.span()));
    const std::uint64_t warm = md.stats().marks + md.stats().fundings;
    REQUIRE(warm > 20);
    int failed = 0;
    {
      NoAllocScope guard;
      for (int round = 0; round < kGeminiPerpRounds; ++round) {
        for (const PaddedJson& f : in) {
          const auto r = md.decode(f.view(), Timestamp{round}, Cycles{1}, s.span());
          if (r.status == ParseStatus::Malformed) ++failed;
        }
      }
    }
    CHECK(failed == 0);
    CHECK(md.stats().marks + md.stats().fundings == warm * (kGeminiPerpRounds + 1));
  }
  {
    fastmm::venues::test::RecordingSink sink(1U << 20);
    gemini::GeminiMdFeed feed(symbols, VenueId{1}, sink.sink, InstrumentCallback{});
    REQUIRE(feed.add_instrument(InstrumentId{0}, true));
    feed.on_connected();
    auto run = [&] {
      int failed = 0;
      for (const PaddedJson& f : in) {
        if (feed.on_message(f.view(), 1) == ParseStatus::Malformed) ++failed;
        while (sink.ring.try_peek() != nullptr) sink.ring.release();
      }
      return failed;
    };
    REQUIRE(run() == 0);
    int failed = 0;
    {
      NoAllocScope guard;
      for (int round = 0; round < 20; ++round) failed += run();
    }
    CHECK(failed == 0);
    CHECK(feed.stats().dropped == 0);
  }
}

#endif  // FASTMM_HOTPATH_VENUES
