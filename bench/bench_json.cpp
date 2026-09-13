// JSON decode micro-benchmarks (plan 5.12 budget: simdjson depth parse 20/100 levels
// 1.5/6 µs p50). Each benchmark decodes one venue frame into its normalised message in the
// same scratch buffer the connectors use; the frames are synthesised in the shape of the
// recorded testnet fixtures (tests/fixtures/{binance,bybit}). "20 levels" = 10 bids + 10
// asks, matching tests/fixtures/binance/depth_update_20.json.
#include "fastmm/venues/binance/binance_md_parser.hpp"
#include "fastmm/venues/binance/binance_user_parser.hpp"
#include "fastmm/venues/bybit/bybit_md_parser.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <benchmark/benchmark.h>

#include <cstdio>
#include <string>

using namespace fastmm;
using namespace fastmm::venues;

namespace {

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  Universe() {
    for (std::uint8_t v = 0; v < 2; ++v) {
      Instrument i{};
      i.symbol = "BTCUSDT";
      i.venue = VenueId{v};
      i.base = "BTC";
      i.quote = "USDT";
      i.flags = Instrument::kEnabled;
      i.tick = Price::from_decimal("0.01").value();
      i.lot = Qty::from_decimal("0.00001").value();
      static_cast<void>(instruments.add(i));
    }
    static_cast<void>(symbols.build(instruments));
  }
};

std::string levels(int n, int start_cents, int step_cents) {
  std::string s = "[";
  char buf[64];
  for (int i = 0; i < n; ++i) {
    const int cents = start_cents + i * step_cents;
    std::snprintf(buf,
                  sizeof buf,
                  "%s[\"%d.%02d000000\",\"%d.%08d\"]",
                  i == 0 ? "" : ",",
                  cents / 100,
                  cents % 100,
                  i % 7,
                  12345678 + i);
    s += buf;
  }
  return s + "]";
}

std::string binance_depth(int per_side) {
  return R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":1789295134334,"s":"BTCUSDT","U":1801512,"u":1801600,"b":)" +
         levels(per_side, 7674518, -1) + R"(,"a":)" + levels(per_side, 7674519, 1) + "}}";
}

std::string bybit_delta(int per_side) {
  std::string b = levels(per_side, 7714050, -10);
  std::string a = levels(per_side, 7714060, 10);
  return R"({"topic":"orderbook.50.BTCUSDT","ts":1789299658933,"type":"delta","data":{"s":"BTCUSDT","b":)" +
         b + R"(,"a":)" + a + R"(,"u":3734359,"seq":2189232166},"cts":1789299658878})";
}

const char* kBookTicker =
    R"({"stream":"btcusdt@bookTicker","data":{"u":1801512,"s":"BTCUSDT","b":"76745.18000000","B":"8.74206000","a":"76745.19000000","A":"12.06313000"}})";
const char* kTrade =
    R"({"stream":"btcusdt@trade","data":{"e":"trade","E":1789295134226,"s":"BTCUSDT","t":388510,"p":"76745.19000000","q":"0.00065000","T":1789295134225,"m":false,"M":true}})";
const char* kExecReport =
    R"({"subscriptionId":0,"event":{"e":"executionReport","E":1789295200000,"s":"BTCUSDT","c":"fm000100000001","S":"BUY","o":"LIMIT_MAKER","f":"GTC","q":"0.00100000","p":"70000.00000000","P":"0.00000000","F":"0.00000000","g":-1,"C":"","x":"TRADE","X":"PARTIALLY_FILLED","r":"NONE","i":4293153,"l":"0.00040000","z":"0.00040000","L":"70000.00000000","n":"0.00000040","N":"BTC","T":1789295199990,"t":12345,"I":8641984,"w":false,"m":true,"M":true,"O":1789295199990,"Z":"28.00000000","Y":"28.00000000","Q":"0.00000000","W":1789295199990,"V":"EXPIRE_MAKER"}})";

struct Scratch {
  alignas(64) std::byte buf[kDecoderScratchBytes];
};

template <class Parser, class... Args>
void run_decode(benchmark::State& state, const std::string& json, Parser& parser) {
  const PaddedJson frame(json);
  static Scratch scratch;
  for (auto _ : state) {
    auto r = parser.decode(frame.view(), Timestamp{}, Cycles{}, scratch.buf);
    benchmark::DoNotOptimize(r);
    if (!r.ok()) state.SkipWithError("decode failed");
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(json.size()));
  state.counters["ns/msg"] =
      benchmark::Counter(static_cast<double>(state.iterations()),
                         benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

void BM_Json_BinanceDepth20(benchmark::State& state) {
  Universe u;
  binance::BinanceMdParser p(u.symbols, VenueId{0});
  run_decode(state, binance_depth(10), p);
}
BENCHMARK(BM_Json_BinanceDepth20);

void BM_Json_BinanceDepth100(benchmark::State& state) {
  Universe u;
  binance::BinanceMdParser p(u.symbols, VenueId{0});
  run_decode(state, binance_depth(50), p);
}
BENCHMARK(BM_Json_BinanceDepth100);

void BM_Json_BinanceBookTicker(benchmark::State& state) {
  Universe u;
  binance::BinanceMdParser p(u.symbols, VenueId{0});
  run_decode(state, kBookTicker, p);
}
BENCHMARK(BM_Json_BinanceBookTicker);

void BM_Json_BinanceTrade(benchmark::State& state) {
  Universe u;
  binance::BinanceMdParser p(u.symbols, VenueId{0});
  run_decode(state, kTrade, p);
}
BENCHMARK(BM_Json_BinanceTrade);

void BM_Json_BinanceExecutionReport(benchmark::State& state) {
  Universe u;
  binance::BinanceUserParser p(u.symbols, u.instruments, VenueId{0});
  run_decode(state, kExecReport, p);
}
BENCHMARK(BM_Json_BinanceExecutionReport);

void BM_Json_BybitOrderbook20(benchmark::State& state) {
  Universe u;
  bybit::BybitMdParser p(u.symbols, VenueId{1});
  run_decode(state, bybit_delta(10), p);
}
BENCHMARK(BM_Json_BybitOrderbook20);

}  // namespace
