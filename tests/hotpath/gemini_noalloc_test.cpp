// The Gemini connector's hot paths do not allocate after warm-up: the market-data parser and feed
// on the recorded production frames (tests/fixtures/gemini/raw_md_stream.jsonl), the order-event
// parser and the order encoder. The first pass is the warm-up; the measured rounds must then
// decode and encode everything without a single allocation.
#if defined(FASTMM_HOTPATH_VENUES)

#include "../venues/venue_test_util.hpp"
#include "alloc_counter.hpp"

#include "fastmm/venues/gemini/gemini_md_feed.hpp"
#include "fastmm/venues/gemini/gemini_md_parser.hpp"
#include "fastmm/venues/gemini/gemini_order_encoder.hpp"
#include "fastmm/venues/gemini/gemini_private_parser.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using fastmm::test::NoAllocScope;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::Scratch;

namespace {

constexpr int kRounds = 200;

std::vector<PaddedJson> recorded_frames() {
  const std::string raw = fastmm::test::fixture("gemini/raw_md_stream.jsonl");
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

template <class Parser>
void check_decoder_noalloc(Parser& parser, const std::vector<PaddedJson>& in) {
  Scratch s;
  for (const PaddedJson& f : in)
    static_cast<void>(parser.decode(f.view(), Timestamp{1}, Cycles{1}, s.span()));
  int failed = 0;
  {
    NoAllocScope guard;
    for (int round = 0; round < kRounds; ++round) {
      for (const PaddedJson& f : in) {
        const auto r = parser.decode(f.view(), Timestamp{round}, Cycles{1}, s.span());
        if (r.status == ParseStatus::Malformed) ++failed;
      }
    }
  }
  CHECK(failed == 0);
}

}  // namespace

TEST_CASE("hotpath.noalloc: Gemini market-data parser and feed, order events and encoder") {
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BTCGUSDPERP", 1, "BTC", "GUSD")));
  REQUIRE(instruments.add(make_instrument("ETHGUSDPERP", 1, "ETH", "GUSD")));
  REQUIRE(instruments.add(make_instrument("BTCUSD", 1, "BTC", "USD")));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  const std::vector<PaddedJson> md_frames = recorded_frames();
  REQUIRE(md_frames.size() > 100);
  {
    gemini::GeminiMdParser md(symbols, VenueId{1});
    check_decoder_noalloc(md, md_frames);
    CHECK(md.stats().malformed == 0);
  }
  {
    // The feed over the whole recording, again and again: every book resyncs to the recorded
    // snapshot at the start of each round (on_connected), so the snapshot path runs too.
    fastmm::venues::test::RecordingSink sink(32U << 20);
    gemini::GeminiMdFeed feed(symbols, VenueId{1}, sink.sink, InstrumentCallback{});
    for (std::uint16_t i = 0; i < 3; ++i) REQUIRE(feed.add_instrument(InstrumentId{i}));
    auto run = [&] {
      feed.on_connected();
      int failed = 0;
      for (const PaddedJson& f : md_frames) {
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
    CHECK(feed.synced_count() == 3);
  }
  {
    gemini::GeminiPrivateParser priv(symbols, VenueId{1});
    std::vector<PaddedJson> frames;
    frames.emplace_back(
        R"({"e":"orderUpdate","E":1759291847686856569,"s":"BTCGUSDPERP","i":73797746498585286,"c":"fm000100000001","S":"BUY","o":"LIMIT","X":"NEW","p":"83000.50","q":"0.0010","z":"0.0010","T":1759291847686856569})");
    frames.emplace_back(
        R"({"e":"orderUpdate","E":1759291847700000000,"s":"BTCGUSDPERP","i":73797746498585286,"c":"fm000100000001","S":"BUY","o":"LIMIT","X":"PARTIALLY_FILLED","p":"83000.50","q":"0.0010","z":"0.0006","Z":"0.0004","L":"83000.50","t":1893456012054189,"m":true,"T":1759291847700000000})");
    frames.emplace_back(
        R"({"e":"orderUpdate","E":1759291847731455006,"s":"BTCGUSDPERP","i":73797746498585286,"c":"fm000100000001","X":"CANCELED","Z":"0.0004","T":1759291847731455006})");
    frames.emplace_back(
        R"({"id":"nfm000100000001","status":200,"result":{"orderId":"73797746498585286"}})");
    check_decoder_noalloc(priv, frames);
  }
  const gemini::GeminiOrderEncoder enc(symbols);
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{0}, VenueId{1});
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.price = Price::from_decimal("83000.5").value();
  n.qty = Qty::from_decimal("0.001").value();
  OutCancelMsg c{};
  init_header(c, EventType::OutCancel, InstrumentId{0}, VenueId{1});
  c.cl_ord_id = n.cl_ord_id;
  const std::array<OrderCommand, 2> cmds{*OrderCommand::from(n.hdr), *OrderCommand::from(c.hdr)};
  std::array<char, gemini::kMaxRequestBytes> buf{};
  for (const OrderCommand& cmd : cmds) REQUIRE(enc.encode_ws(cmd, "73797746498585286", buf) > 0);
  int failed = 0;
  {
    NoAllocScope guard;
    for (int round = 0; round < kRounds; ++round) {
      for (const OrderCommand& cmd : cmds) {
        if (enc.encode_ws(cmd, "73797746498585286", buf) == 0) ++failed;
      }
    }
  }
  CHECK(failed == 0);
}

#endif  // FASTMM_HOTPATH_VENUES
