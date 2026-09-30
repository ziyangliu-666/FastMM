// OKX v5 mark-price, index-tickers, funding-rate and open-interest -> PerpStateMsg. Frames recorded
// on production (wss://ws.okx.com:8443/ws/v5/public, fastmm-live --dry-run --record-raw on
// 2026-09-30; tests/fixtures/okx/fixtures.meta.json). Field meanings:
// https://www.okx.com/docs-v5/en/#public-data-websocket-mark-price-channel, ...-index-tickers-
// channel, ...-funding-rate-channel, ...-open-interest-channel (read 2026-09-30).
#include "venue_test_util.hpp"

#include "fastmm/venues/okx/okx_md_feed.hpp"
#include "fastmm/venues/okx/okx_md_parser.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::okx;
using fastmm::venues::test::make_instrument;
using fastmm::venues::test::RecordingSink;
using fastmm::venues::test::Scratch;

namespace {

constexpr VenueId kOkx{1};
constexpr InstrumentId kBtcSwap{0};
constexpr InstrumentId kEthSwap{1};
constexpr InstrumentId kBtcSpot{2};

// Two swaps and the spot pair whose name is the BTC swap's index.
struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    REQUIRE(instruments.add(make_instrument("BTC-USDT-SWAP", 1, "BTC", "USDT")));
    REQUIRE(instruments.add(make_instrument("ETH-USDT-SWAP", 1, "ETH", "USDT")));
    REQUIRE(instruments.add(make_instrument("BTC-USDT", 1, "BTC", "USDT")));
    REQUIRE(symbols.build(instruments));
  }
};

// The JSON of every line of a recorded stream (rx_ns \t frame).
std::vector<std::string> recorded(const char* name) {
  const std::string raw = fastmm::test::fixture(name);
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

// The n-th (0-based) data push of `channel` on `inst_id` in the recorded stream.
std::string push_of(const std::vector<std::string>& frames,
                    const std::string& channel,
                    const std::string& inst_id,
                    int n = 0) {
  const std::string arg = R"({"arg":{"channel":")" + channel + R"(","instId":")" + inst_id + "\"}";
  int k = n;
  for (const std::string& f : frames) {
    if (f.starts_with(arg) && k-- == 0) return f;
  }
  FAIL("no push " << n << " of " << channel << " " << inst_id);
  return {};
}

MdDecodeResult decode(OkxMdParser& p, const std::string& frame, Scratch& s) {
  const PaddedJson j(frame);
  return p.decode(j.view(), Timestamp{5}, Cycles{}, s.span());
}

const PerpStateMsg& perp_at(Scratch& s, std::uint32_t i = 0) {
  return *reinterpret_cast<const PerpStateMsg*>(s.span().data() + i * sizeof(PerpStateMsg));
}

Timestamp at_ms(std::int64_t ms) {
  return Timestamp{ms * 1'000'000};
}
Price px(const char* s) {
  return Price::from_decimal(s).value_or(Price{});
}
Qty qty(const char* s) {
  return Qty::from_decimal(s).value_or(Qty{});
}
constexpr Duration kHour = seconds(3600);

}  // namespace

TEST_CASE("okx.perp: mark-price carries the mark and the venue time") {
  Universe u;
  OkxMdParser p(u.symbols, kOkx);
  Scratch s;
  const auto frames = recorded("okx/raw_perp_stream.jsonl");
  const MdDecodeResult r = decode(p, push_of(frames, "mark-price", "BTC-USDT-SWAP"), s);
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::PerpState);
  CHECK(r.count == 1);
  CHECK(r.len == sizeof(PerpStateMsg));
  const PerpStateMsg& m = perp_at(s);
  CHECK(m.hdr.type == EventType::PerpState);
  CHECK(m.hdr.len == sizeof(PerpStateMsg));
  CHECK(m.hdr.instrument == kBtcSwap);
  CHECK(m.hdr.venue == kOkx);
  CHECK(m.fields == PerpStateMsg::kMark);
  CHECK(m.mark_price == px("83003.8"));
  CHECK(m.hdr.exch_ts == at_ms(1790757203016));
  CHECK(m.hdr.recv_ts == Timestamp{5});
  CHECK(p.stats().perp_states == 1);

  REQUIRE(decode(p, push_of(frames, "mark-price", "ETH-USDT-SWAP"), s).ok());
  CHECK(perp_at(s).hdr.instrument == kEthSwap);
  CHECK(perp_at(s).mark_price == px("2664.48"));
  CHECK(perp_at(s).hdr.exch_ts == at_ms(1790757203050));
}

TEST_CASE("okx.perp: index-tickers maps the index to the swaps that follow it, not the spot pair") {
  Universe u;
  OkxMdParser p(u.symbols, kOkx);
  Scratch s;
  const auto frames = recorded("okx/raw_perp_stream.jsonl");
  const std::string btc_index = push_of(frames, "index-tickers", "BTC-USDT");
  // No swap registered for the index: not ours, although BTC-USDT is a known (spot) symbol.
  CHECK(decode(p, btc_index, s).status == ParseStatus::UnknownSymbol);

  p.add_index("BTC-USDT", kBtcSwap);
  p.add_index("ETH-USDT", kEthSwap);
  MdDecodeResult r = decode(p, btc_index, s);
  REQUIRE(r.ok());
  CHECK(r.kind == MdKind::PerpState);
  REQUIRE(r.count == 1);
  const PerpStateMsg& m = perp_at(s);
  CHECK(m.hdr.instrument == kBtcSwap);
  CHECK(m.fields == PerpStateMsg::kIndex);
  CHECK(m.index_price == px("83043.9"));
  CHECK(m.mark_price == Price{});
  CHECK(m.hdr.exch_ts == at_ms(1790757203066));

  r = decode(p, push_of(frames, "index-tickers", "ETH-USDT"), s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 1);
  CHECK(perp_at(s).hdr.instrument == kEthSwap);
  CHECK(perp_at(s).index_price == px("2665.79"));
  CHECK(perp_at(s).hdr.exch_ts == at_ms(1790757202575));

  // Two swaps on one index (the spot pair's id stands in for the second; registering the same
  // swap again changes nothing): one message each, back to back.
  p.add_index("BTC-USDT", kBtcSwap);
  p.add_index("BTC-USDT", kBtcSpot);
  r = decode(p, btc_index, s);
  REQUIRE(r.ok());
  REQUIRE(r.count == 2);
  CHECK(r.len == 2 * sizeof(PerpStateMsg));
  CHECK(perp_at(s, 0).hdr.instrument == kBtcSwap);
  CHECK(perp_at(s, 1).hdr.instrument == kBtcSpot);
  CHECK(perp_at(s, 1).index_price == px("83043.9"));
  CHECK(perp_at(s, 1).fields == PerpStateMsg::kIndex);
}

TEST_CASE("okx.perp: funding-rate gives the rate, the settlement time and the 8 h interval") {
  Universe u;
  OkxMdParser p(u.symbols, kOkx);
  Scratch s;
  const auto frames = recorded("okx/raw_perp_stream.jsonl");
  // fundingTime 2026-09-30 16:00 UTC, nextFundingTime 2026-10-01 00:00 UTC.
  REQUIRE(decode(p, push_of(frames, "funding-rate", "BTC-USDT-SWAP"), s).ok());
  const PerpStateMsg& m = perp_at(s);
  CHECK(m.hdr.instrument == kBtcSwap);
  CHECK(m.fields == PerpStateMsg::kFunding);
  CHECK(m.funding_rate == 0.0000252378080097);
  CHECK(m.next_funding == at_ms(1790784000000));
  CHECK(m.funding_interval == kHour * 8);
  CHECK(m.hdr.exch_ts == at_ms(1790757122992));

  REQUIRE(decode(p, push_of(frames, "funding-rate", "ETH-USDT-SWAP", 1), s).ok());
  CHECK(perp_at(s).hdr.instrument == kEthSwap);
  CHECK(perp_at(s).funding_rate == 0.0000539743191049);
  CHECK(perp_at(s).funding_interval == kHour * 8);
  CHECK(perp_at(s).hdr.exch_ts == at_ms(1790757206046));
}

TEST_CASE("okx.perp: a 4 h funding interval is read from fundingTime and nextFundingTime") {
  InstrumentTable instruments;
  REQUIRE(instruments.add(make_instrument("BIGTIME-USDT-SWAP", 1, "BIGTIME", "USDT")));
  SymbolTable symbols;
  REQUIRE(symbols.build(instruments));
  OkxMdParser p(symbols, kOkx);
  Scratch s;
  const auto frames = recorded("okx/raw_perp_4h.jsonl");
  REQUIRE(decode(p, push_of(frames, "funding-rate", "BIGTIME-USDT-SWAP"), s).ok());
  const PerpStateMsg& m = perp_at(s);
  CHECK(m.fields == PerpStateMsg::kFunding);
  CHECK(m.funding_interval == kHour * 4);
  CHECK(m.next_funding.ns % (4LL * 3600 * 1'000'000'000) == 0);
}

TEST_CASE("okx.perp: open-interest in contracts, rounded to the fixed point") {
  Universe u;
  OkxMdParser p(u.symbols, kOkx);
  Scratch s;
  const auto frames = recorded("okx/raw_perp_stream.jsonl");
  // "oi":"2874486.99000000925": more decimals than the fixed point holds, rounded half up.
  REQUIRE(decode(p, push_of(frames, "open-interest", "BTC-USDT-SWAP"), s).ok());
  CHECK(perp_at(s).fields == PerpStateMsg::kOpenInterest);
  CHECK(perp_at(s).open_interest == qty("2874486.99000001"));
  CHECK(perp_at(s).hdr.exch_ts == at_ms(1790757195759));
  REQUIRE(decode(p, push_of(frames, "open-interest", "ETH-USDT-SWAP"), s).ok());
  CHECK(perp_at(s).hdr.instrument == kEthSwap);
  CHECK(perp_at(s).open_interest == qty("5836712.83000002"));
}

TEST_CASE("okx.perp: a bad or empty field is malformed, a zero mark reports nothing") {
  Universe u;
  OkxMdParser p(u.symbols, kOkx);
  Scratch s;
  const std::string head =
      R"({"arg":{"channel":"funding-rate","instId":"BTC-USDT-SWAP"},"data":[{)";
  CHECK(
      decode(p, head + R"("fundingRate":"","fundingTime":"1790784000000","ts":"1"}]})", s).status ==
      ParseStatus::Malformed);
  CHECK(decode(p, head + R"("fundingRate":"0.0001","ts":"1"}]})", s).status ==
        ParseStatus::Malformed);
  // No nextFundingTime: the rate and the time, without an interval.
  REQUIRE(decode(p, head + R"("fundingRate":"-0.0001","fundingTime":"1790784000000"}]})", s).ok());
  CHECK(perp_at(s).funding_rate == -0.0001);
  CHECK(perp_at(s).funding_interval == Duration{});
  CHECK(perp_at(s).hdr.exch_ts == Timestamp{});
  CHECK(
      decode(
          p,
          R"({"arg":{"channel":"mark-price","instId":"BTC-USDT-SWAP"},"data":[{"markPx":"x","ts":"1"}]})",
          s)
          .status == ParseStatus::Malformed);
  CHECK(
      decode(
          p,
          R"({"arg":{"channel":"mark-price","instId":"BTC-USDT-SWAP"},"data":[{"markPx":"0","ts":"1"}]})",
          s)
          .status == ParseStatus::Ignored);
  CHECK(
      decode(
          p,
          R"({"arg":{"channel":"mark-price","instId":"SOL-USDT-SWAP"},"data":[{"markPx":"1","ts":"1"}]})",
          s)
          .status == ParseStatus::UnknownSymbol);
  CHECK(p.stats().malformed == 3);
}

TEST_CASE("okx.perp: swaps subscribe mark-price, funding-rate, open-interest and their index") {
  Universe u;
  RecordingSink sink(1U << 20);
  {
    OkxMdFeed feed(u.symbols, kOkx, sink.sink, ResubscribeRequester{});
    REQUIRE(feed.add_instrument(kBtcSpot));
    REQUIRE(feed.subscription_payloads().size() == 1);  // a spot pair: the book channels only
    CHECK(feed.subscription_payloads()[0].find("mark-price") == std::string::npos);
    REQUIRE(feed.add_instrument(kBtcSwap));
    REQUIRE(feed.add_instrument(kEthSwap));
    REQUIRE(feed.subscription_payloads().size() == 2);
    CHECK(
        feed.subscription_payloads()[1] ==
        R"({"id":"perp","op":"subscribe","args":[)"
        R"({"channel":"mark-price","instId":"BTC-USDT-SWAP"},{"channel":"funding-rate","instId":"BTC-USDT-SWAP"},)"
        R"({"channel":"open-interest","instId":"BTC-USDT-SWAP"},{"channel":"index-tickers","instId":"BTC-USDT"},)"
        R"({"channel":"mark-price","instId":"ETH-USDT-SWAP"},{"channel":"funding-rate","instId":"ETH-USDT-SWAP"},)"
        R"({"channel":"open-interest","instId":"ETH-USDT-SWAP"},{"channel":"index-tickers","instId":"ETH-USDT"}]})");
  }
  CHECK(is_swap_symbol("BTC-USDT-SWAP"));
  CHECK(swap_index("BTC-USDT-SWAP") == "BTC-USDT");
  CHECK(swap_index("btc-usdt-swap") == "btc-usdt");
  CHECK(swap_index("BTC-USDT").empty());
  CHECK(swap_index("-SWAP").empty());
}

TEST_CASE("okx.perp: the recorded production stream reaches the sink as PerpState messages") {
  Universe u;
  RecordingSink sink(8U << 20);
  OkxMdFeed feed(u.symbols, kOkx, sink.sink, ResubscribeRequester{});
  REQUIRE(feed.add_instrument(kBtcSwap));
  REQUIRE(feed.add_instrument(kEthSwap));
  REQUIRE(feed.add_instrument(kBtcSpot));
  feed.on_connected();
  int ok = 0;
  int acks = 0;
  for (const std::string& f : recorded("okx/raw_perp_stream.jsonl")) {
    const PaddedJson j(f);
    const ParseStatus st = feed.on_message(j.view(), 1);
    if (st == ParseStatus::Ok) {
      ++ok;
    } else {
      CHECK(st == ParseStatus::Ignored);
      CHECK(feed.last().control == ControlOp::Subscribe);
      ++acks;
    }
  }
  CHECK(acks == 8);
  CHECK(ok == 22);  // 3 marks, 3 indices, 2 funding and 3 open interest a swap
  CHECK(feed.stats().malformed == 0);
  CHECK(feed.stats().dropped == 0);
  CHECK(feed.parser_stats().perp_states == 22);

  struct Seen {
    int mark = 0, index = 0, funding = 0, oi = 0;
    PerpStateMsg last{};
  };
  std::array<Seen, 2> seen{};
  for (const auto& raw : sink.drain()) {
    REQUIRE(RecordingSink::type_of(raw) == EventType::PerpState);
    const auto& m = RecordingSink::as<PerpStateMsg>(raw);
    REQUIRE(m.hdr.instrument.value < 2);  // never the spot pair
    Seen& e = seen[m.hdr.instrument.value];
    e.mark += (m.fields & PerpStateMsg::kMark) != 0 ? 1 : 0;
    e.index += (m.fields & PerpStateMsg::kIndex) != 0 ? 1 : 0;
    e.funding += (m.fields & PerpStateMsg::kFunding) != 0 ? 1 : 0;
    e.oi += (m.fields & PerpStateMsg::kOpenInterest) != 0 ? 1 : 0;
    CHECK(m.hdr.exch_ts.valid());
    if ((m.fields & PerpStateMsg::kFunding) != 0) e.last = m;
  }
  for (const Seen& e : seen) {
    CHECK(e.mark == 3);
    CHECK(e.index == 3);
    CHECK(e.funding == 2);
    CHECK(e.oi == 3);
    CHECK(e.last.funding_interval == kHour * 8);
  }
  CHECK(seen[0].last.funding_rate == 0.0000249625867043);
  CHECK(seen[1].last.funding_rate == 0.0000539743191049);
}
