// UpdateFilter and WsMdCopy (venues/md_copy.hpp): the first copy of each update passes, across
// connections and threads; a copy pushes tops and trades only.
#include "fastmm/venues/md_copy.hpp"

#include "venue_test_util.hpp"

#include "fastmm/venues/binance_usdm/binance_usdm_md_parser.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::test;

namespace {

std::string usdm_ticker(std::uint64_t u, const char* bid) {
  return std::string(R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":)") +
         std::to_string(u) + R"(,"s":"BTCUSDT","b":")" + bid +
         R"(","B":"1.000","a":"70010.00","A":"2.000","T":1789469121938,"E":1789469121940}})";
}

}  // namespace

TEST_CASE("md_copy: UpdateFilter passes each update id once, per kind and instrument") {
  UpdateFilter f;
  const InstrumentId a{3};
  const InstrumentId b{4};
  CHECK(f.first(MdKind::BookTicker, a, 10));
  CHECK_FALSE(f.first(MdKind::BookTicker, a, 10));  // the second copy
  CHECK_FALSE(f.first(MdKind::BookTicker, a, 9));   // older
  CHECK(f.first(MdKind::BookTicker, a, 12));        // a gap is fine: newer passes
  CHECK(f.first(MdKind::BookTicker, b, 10));        // another instrument
  CHECK(f.first(MdKind::Trade, a, 10));             // another kind
  CHECK_FALSE(f.first(MdKind::Trade, a, 10));
  // Kinds the filter does not keep, and instruments out of range, always pass.
  CHECK(f.first(MdKind::BookDelta, a, 1));
  CHECK(f.first(MdKind::BookDelta, a, 1));
  CHECK(f.first(MdKind::BookTicker, InstrumentId{kMaxInstruments}, 1));
  CHECK(UpdateFilter::keeps(MdKind::BookTicker));
  CHECK_FALSE(UpdateFilter::keeps(MdKind::PerpState));
}

TEST_CASE("md_copy: two threads racing on the same updates pass each one exactly once") {
  UpdateFilter f;
  constexpr std::uint64_t kUpdates = 200'000;
  std::atomic<bool> go{false};
  std::uint64_t won[2] = {0, 0};
  const auto run = [&](int k) {
    while (!go.load()) {
    }
    for (std::uint64_t u = 1; u <= kUpdates; ++u) {
      if (f.first(MdKind::BookTicker, InstrumentId{static_cast<std::uint16_t>(u % 4)}, u)) ++won[k];
    }
  };
  std::thread t0(run, 0);
  std::thread t1(run, 1);
  go = true;
  t0.join();
  t1.join();
  CHECK(won[0] + won[1] == kUpdates);
}

TEST_CASE("md_copy: WsMdCopy pushes the first copy of a bookTicker and counts the rest") {
  TestUniverse u;
  RecordingSink md;
  UpdateFilter filter;
  net::ConnectionConfig cfg;
  cfg.url = "wss://example.invalid/public/stream?streams=btcusdt@bookTicker";
  WsMdCopy<binance_usdm::BinanceUsdmMdParser> copy(
      "test: md copy 1", cfg, md.sink, filter, {}, u.symbols, VenueId{0}, kMdCopyParserBytes);
  CHECK(copy.name() == "test: md copy 1");
  CHECK(copy.on_text(usdm_ticker(500, "70000.00")) == ParseStatus::Ok);
  CHECK(copy.on_text(usdm_ticker(500, "70000.00")) == ParseStatus::Ok);  // another connection's
  CHECK(copy.on_text(usdm_ticker(499, "69999.00")) == ParseStatus::Ok);
  // Another connection delivered 501 first: this copy's is late.
  CHECK(filter.first(MdKind::BookTicker, InstrumentId{0}, 501));
  CHECK(copy.on_text(usdm_ticker(501, "70001.00")) == ParseStatus::Ok);
  CHECK(copy.on_text(usdm_ticker(502, "70002.00")) == ParseStatus::Ok);
  // Not a top or a trade: ignored.
  CHECK(
      copy.on_text(
          R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1,"T":1,"s":"BTCUSDT","U":1,"u":2,"pu":0,"b":[],"a":[]}})") ==
      ParseStatus::Ignored);
  CHECK(copy.on_text("{not json") == ParseStatus::Malformed);
  const auto all = md.drain();
  REQUIRE(all.size() == 2);
  CHECK(RecordingSink::as<BookTickerMsg>(all[0]).hdr.venue_seq == 500);
  CHECK(RecordingSink::as<BookTickerMsg>(all[1]).hdr.venue_seq == 502);
  const MdCopyStats st = copy.stats();
  CHECK(st.messages == 7);
  CHECK(st.first == 2);
  CHECK(st.late == 3);
  CHECK(st.malformed == 1);
  CHECK(st.dropped == 0);
}
