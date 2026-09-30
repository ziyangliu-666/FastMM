// Deribit tickers that carry a PerpStateMsg (perpetuals, futures) do not allocate after warm-up:
// the market-data parser and the feed on the tickers recorded on production
// (tests/fixtures/deribit/*_prod.json, raw_perp_ticker_stream.jsonl). The first pass is the
// warm-up; the measured rounds must then decode and push everything without a single allocation.
#if defined(FASTMM_HOTPATH_VENUES)

#include "../venues/venue_test_util.hpp"
#include "alloc_counter.hpp"

#include "fastmm/venues/deribit/deribit_md_feed.hpp"
#include "fastmm/venues/deribit/deribit_md_parser.hpp"

#include <sstream>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::test::NoAllocScope;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::Scratch;

namespace {

constexpr int kPerpRounds = 200;

// 0 BTC-PERPETUAL, 1 ETH-PERPETUAL, 2 BTC-25DEC26 (future), 3 BTC-25DEC26-76000-C (option).
struct PerpUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  PerpUniverse() {
    Instrument btc = make_instrument("BTC-PERPETUAL", 2, "BTC", "USD");
    btc.asset_class = AssetClass::Perpetual;
    btc.contract_multiplier = Qty::from_int(10);
    REQUIRE(instruments.add(btc));
    Instrument eth = make_instrument("ETH-PERPETUAL", 2, "ETH", "USD");
    eth.asset_class = AssetClass::Perpetual;
    eth.contract_multiplier = Qty::from_int(1);
    REQUIRE(instruments.add(eth));
    Instrument fut = make_instrument("BTC-25DEC26", 2, "BTC", "USD");
    fut.asset_class = AssetClass::Future;
    fut.contract_multiplier = Qty::from_int(10);
    REQUIRE(instruments.add(fut));
    Instrument call = make_instrument("BTC-25DEC26-76000-C", 2, "BTC", "BTC");
    call.asset_class = AssetClass::Option;
    call.option_type = OptionType::Call;
    REQUIRE(instruments.add(call));
    REQUIRE(symbols.build(instruments));
  }
};

std::vector<PaddedJson> perp_frames() {
  std::vector<PaddedJson> out;
  for (const char* n : {"deribit/ticker_perp_btc_prod.json",
                        "deribit/ticker_perp_eth_prod.json",
                        "deribit/ticker_future_prod.json",
                        "deribit/ticker_option_prod.json"})
    out.emplace_back(fastmm::test::fixture(n));
  std::istringstream lines(fastmm::test::fixture("deribit/raw_perp_ticker_stream.jsonl"));
  std::string line;
  while (std::getline(lines, line)) out.emplace_back(line);
  return out;
}

}  // namespace

TEST_CASE("hotpath.noalloc: Deribit perpetual and future tickers with PerpState") {
  PerpUniverse u;
  const std::vector<PaddedJson> in = perp_frames();
  REQUIRE(in.size() > 60);
  {
    deribit::DeribitMdParser md(u.symbols, u.instruments, VenueId{2});
    Scratch s;
    for (const PaddedJson& f : in)
      REQUIRE(md.decode(f.view(), Timestamp{1}, Cycles{1}, s.span()).ok());
    const std::uint64_t warm = md.stats().perp_states;
    REQUIRE(warm > 50);
    int failed = 0;
    {
      NoAllocScope guard;
      for (int round = 0; round < kPerpRounds; ++round) {
        for (const PaddedJson& f : in) {
          if (!md.decode(f.view(), Timestamp{round}, Cycles{1}, s.span()).ok()) ++failed;
        }
      }
    }
    CHECK(failed == 0);
    CHECK(md.stats().perp_states == warm * (kPerpRounds + 1));
  }
  {
    fastmm::venues::test::RecordingSink sink(4U << 20);
    deribit::DeribitMdFeed feed(
        u.symbols,
        u.instruments,
        VenueId{2},
        sink.sink,
        deribit::ResubscribeRequester{[](void*, InstrumentId) noexcept {}, nullptr});
    for (std::uint32_t i = 0; i < 4; ++i) REQUIRE(feed.add_instrument(InstrumentId{i}));
    feed.on_connected();
    auto run = [&] {
      int failed = 0;
      for (const PaddedJson& f : in) {
        if (feed.on_message(f.view(), 1) != ParseStatus::Ok) ++failed;
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
