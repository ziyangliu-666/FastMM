// The Bybit tickers path does not allocate after warm-up: the market-data parser and feed on the
// recorded production tickers frames (tests/fixtures/bybit/raw_tickers_stream.jsonl), including
// the reconnect that clears the per-symbol cache. The first pass is the warm-up; the measured
// rounds must then decode everything without a single allocation.
#if defined(FASTMM_HOTPATH_VENUES)

#include "../venues/venue_test_util.hpp"
#include "alloc_counter.hpp"

#include "fastmm/venues/bybit/bybit_md_feed.hpp"
#include "fastmm/venues/bybit/bybit_md_parser.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::test::NoAllocScope;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::Scratch;

namespace {

constexpr int kRounds = 200;

// Every recorded frame (`<rx_ns>\t<json>` per line).
std::vector<PaddedJson> recorded_frames() {
  const std::string raw = fastmm::test::fixture("bybit/raw_tickers_stream.jsonl");
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

// BTCUSDT (id 0) and ETHUSDT (id 1), linear perpetuals on venue 1.
struct PerpUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  PerpUniverse() {
    Instrument btc = make_instrument("BTCUSDT", 1, "BTC", "USDT");
    btc.asset_class = AssetClass::Perpetual;
    Instrument eth = make_instrument("ETHUSDT", 1, "ETH", "USDT");
    eth.asset_class = AssetClass::Perpetual;
    REQUIRE(instruments.add(btc));
    REQUIRE(instruments.add(eth));
    REQUIRE(symbols.build(instruments));
  }
};

}  // namespace

TEST_CASE("hotpath.noalloc: Bybit tickers parser and feed") {
  PerpUniverse u;
  const std::vector<PaddedJson> in = recorded_frames();
  REQUIRE(in.size() > 100);
  {
    bybit::BybitMdParser md(u.symbols, VenueId{1});
    Scratch s;
    for (const PaddedJson& f : in)
      static_cast<void>(md.decode(f.view(), Timestamp{1}, Cycles{1}, s.span()));
    int failed = 0;
    std::uint64_t states = 0;
    {
      NoAllocScope guard;
      for (int round = 0; round < kRounds; ++round) {
        md.reset_tickers();  // a reconnect: the next snapshot refills the cache
        for (const PaddedJson& f : in) {
          const auto r = md.decode(f.view(), Timestamp{round}, Cycles{1}, s.span());
          if (r.status == ParseStatus::Malformed) ++failed;
          if (r.kind == MdKind::PerpState) ++states;
        }
      }
    }
    CHECK(failed == 0);
    CHECK(states > static_cast<std::uint64_t>(kRounds) * 100);
  }
  {
    fastmm::venues::test::RecordingSink rs(8U << 20);
    bybit::BybitMdFeed feed(u.symbols, VenueId{1}, rs.sink, {}, 50);
    REQUIRE(feed.add_instrument(InstrumentId{0}, true));
    REQUIRE(feed.add_instrument(InstrumentId{1}, true));
    feed.on_connected();
    for (const PaddedJson& f : in) static_cast<void>(feed.on_message(f.view(), 1));
    static_cast<void>(rs.drain());
    int failed = 0;
    {
      NoAllocScope guard;
      for (int round = 0; round < 20; ++round) {
        feed.on_connected();
        for (const PaddedJson& f : in) {
          if (feed.on_message(f.view(), 1) == ParseStatus::Malformed) ++failed;
        }
      }
    }
    CHECK(failed == 0);
    CHECK(feed.stats().dropped == 0);
  }
}

#endif
