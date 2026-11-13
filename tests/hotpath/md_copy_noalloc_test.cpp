// A market-data copy's frame path (venues/md_copy.hpp): decode, filter, push, no allocation.
#include "alloc_counter.hpp"
#include "test_support.hpp"

#include "fastmm/core/instrument.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"
#include "fastmm/venues/md_copy.hpp"
#include "fastmm/venues/symbology.hpp"

#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::test::NoAllocScope;

TEST_CASE("hotpath.noalloc: market-data copy frames") {
  InstrumentTable instruments;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.venue = VenueId{0};
  i.base = "BTC";
  i.quote = "USDT";
  i.asset_class = AssetClass::Perpetual;
  i.flags = Instrument::kEnabled;
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  REQUIRE(instruments.add(i));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  MsgRing ring(1U << 20);
  EventSink sink(&ring, SinkPolicy::Drop);
  UpdateFilter filter;
  net::ConnectionConfig cfg;
  cfg.url = "wss://example.invalid/public/stream";
  WsMdCopy<binance_usdm::BinanceUsdmMdParser> copy(
      "noalloc", cfg, sink, filter, {}, symbols, VenueId{0}, kMdCopyParserBytes);
  std::vector<std::string> frames;
  for (int u = 1; u <= 64; ++u)
    frames.push_back(
        std::string(R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":)") +
        std::to_string(u) +
        R"(,"s":"BTCUSDT","b":"70000.00","B":"1.000","a":"70010.00","A":"2.000","T":1789469121938,"E":1789469121940}})");
  for (std::string& f : frames) f.reserve(f.size() + 64);  // simdjson padding
  static_cast<void>(copy.on_text(frames[0]));              // warm up
  {
    NoAllocScope guard(true);
    for (const std::string& f : frames) static_cast<void>(copy.on_text(f));  // 1 late, 63 first
    for (const std::string& f : frames) static_cast<void>(copy.on_text(f));  // all late
  }
  CHECK(copy.stats().first == 64);
  CHECK(copy.stats().late == 64 + 64 - 63);
}
