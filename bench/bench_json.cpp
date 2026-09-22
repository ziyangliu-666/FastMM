// JSON decode micro-benchmarks (plan 5.12 budget: simdjson depth parse 20/100 levels
// 1.5/6 µs p50). Each benchmark decodes one venue frame into its normalised message in the
// same scratch buffer the connectors use; the frames are synthesised in the shape of the
// recorded testnet fixtures (tests/fixtures/{binance,bybit}). "20 levels" = 10 bids + 10
// asks, matching tests/fixtures/binance/depth_update_20.json.
//
// BM_Sbe_Binance*: the same Binance events as SBE frames (md_format = "sbe", stream_1_0 schema)
// through BinanceSbeMdParser into the same messages, for a per-message JSON vs SBE comparison.
#include "fastmm/venues/binance/binance_md_parser.hpp"
#include "fastmm/venues/binance/binance_sbe_md_parser.hpp"
#include "fastmm/venues/binance/binance_user_parser.hpp"
#include "fastmm/venues/binance/generated/binance_stream_sbe.hpp"
#include "fastmm/venues/bybit/bybit_md_parser.hpp"
#include "fastmm/venues/bybit/bybit_private_parser.hpp"
#include "fastmm/venues/deribit/deribit_md_parser.hpp"
#include "fastmm/venues/deribit/deribit_private_parser.hpp"
#include "fastmm/venues/padded_json.hpp"

#include <benchmark/benchmark.h>

#include <cstdio>
#include <string>
#include <vector>

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

// ---- SBE (md_format = "sbe") -----------------------------------------------------------------

namespace ss = binance::sbe_stream;
using Frame = std::vector<std::byte>;

// DepthDiffStreamEvent with the same levels as binance_depth(per_side), exponents -8 / -8.
Frame sbe_depth(int per_side) {
  Frame f(64 + 32 * static_cast<std::size_t>(per_side) * 2);
  ss::DepthDiffStreamEventWriter w(std::span<std::byte>(f).subspan(8));
  w.set_event_time(1789295134334000);
  w.set_first_book_update_id(1801512);
  w.set_last_book_update_id(1801600);
  w.set_price_exponent(-8);
  w.set_qty_exponent(-8);
  auto fill = [&](auto g, int start_cents, int step_cents) {
    for (int i = 0; i < per_side; ++i) {
      g[static_cast<std::size_t>(i)].set_price(
          static_cast<std::int64_t>(start_cents + i * step_cents) * 1'000'000);
      g[static_cast<std::size_t>(i)].set_qty(static_cast<std::int64_t>(i % 7) * 100'000'000 +
                                             12345678 + i);
    }
  };
  fill(w.bids(static_cast<std::size_t>(per_side)), 7674518, -1);
  fill(w.asks(static_cast<std::size_t>(per_side)), 7674519, 1);
  static_cast<void>(w.set_symbol("BTCUSDT"));
  ss::DepthDiffStreamEventWriter::header().store(f.data());
  f.resize(8 + w.size_bytes());
  return f;
}

Frame sbe_best_bid_ask() {
  Frame f(128);
  ss::BestBidAskStreamEventWriter w(std::span<std::byte>(f).subspan(8));
  w.set_event_time(1789295134226000);
  w.set_book_update_id(1801512);
  w.set_price_exponent(-8);
  w.set_qty_exponent(-8);
  w.set_bid_price(7674518000000);
  w.set_bid_qty(874206000);
  w.set_ask_price(7674519000000);
  w.set_ask_qty(1206313000);
  static_cast<void>(w.set_symbol("BTCUSDT"));
  ss::BestBidAskStreamEventWriter::header().store(f.data());
  f.resize(8 + w.size_bytes());
  return f;
}

Frame sbe_trade() {
  Frame f(128);
  ss::TradesStreamEventWriter w(std::span<std::byte>(f).subspan(8));
  w.set_event_time(1789295134226000);
  w.set_transact_time(1789295134225000);
  w.set_price_exponent(-8);
  w.set_qty_exponent(-8);
  auto t = w.trades(1);
  t[0].set_id(388510);
  t[0].set_price(7674519000000);
  t[0].set_qty(65000);
  t[0].set_is_buyer_maker(ss::boolEnum::False);
  static_cast<void>(w.set_symbol("BTCUSDT"));
  ss::TradesStreamEventWriter::header().store(f.data());
  f.resize(8 + w.size_bytes());
  return f;
}

void run_sbe(benchmark::State& state, const Frame& frame) {
  Universe u;
  binance::BinanceSbeMdParser p(u.symbols, VenueId{0});
  static Scratch scratch;
  std::uint32_t emitted = 0;
  for (auto _ : state) {
    const ParseStatus st =
        p.decode(frame, Timestamp{}, Cycles{}, scratch.buf, [&](EventHeader& h, MdKind) {
          benchmark::DoNotOptimize(h);
          ++emitted;
        });
    ParseStatus sink = st;
    benchmark::DoNotOptimize(sink);
    if (st != ParseStatus::Ok) state.SkipWithError("decode failed");
  }
  benchmark::DoNotOptimize(emitted);
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
  state.counters["ns/msg"] =
      benchmark::Counter(static_cast<double>(state.iterations()),
                         benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

void BM_Sbe_BinanceDepth20(benchmark::State& state) {
  run_sbe(state, sbe_depth(10));
}
BENCHMARK(BM_Sbe_BinanceDepth20);

void BM_Sbe_BinanceDepth100(benchmark::State& state) {
  run_sbe(state, sbe_depth(50));
}
BENCHMARK(BM_Sbe_BinanceDepth100);

void BM_Sbe_BinanceBestBidAsk(benchmark::State& state) {
  run_sbe(state, sbe_best_bid_ask());
}
BENCHMARK(BM_Sbe_BinanceBestBidAsk);

void BM_Sbe_BinanceTrade(benchmark::State& state) {
  run_sbe(state, sbe_trade());
}
BENCHMARK(BM_Sbe_BinanceTrade);

void BM_Json_BybitOrderbook20(benchmark::State& state) {
  Universe u;
  bybit::BybitMdParser p(u.symbols, VenueId{1});
  run_decode(state, bybit_delta(10), p);
}
BENCHMARK(BM_Json_BybitOrderbook20);

// Recorded Bybit and Deribit frames (tests/fixtures/{bybit,deribit}), verbatim.
const char* kBybitTrade =
    R"({"topic":"publicTrade.BTCUSDT","ts":1789299660724,"type":"snapshot","data":[{"i":"2100000000188691524","T":1789299660688,"p":"77140.5","v":"0.00389","S":"Sell","seq":2189232173,"s":"BTCUSDT","BT":false,"RPI":false}]})";
const char* kBybitExecution =
    R"({"topic":"execution","id":"386825804_BTCUSDT_140612148849382","creationTime":1789299703460,"data":[{"category":"spot","symbol":"BTCUSDT","execFee":"0.000001","execId":"0ab1bdf7-4219-438b-b30a-32ec863018f7","execPrice":"77140.5","execQty":"0.001","execType":"Trade","execValue":"77.1405","feeRate":"0.001","orderId":"2012345678901234569","orderLinkId":"fm000100000003","orderPrice":"77200","orderQty":"0.001","side":"Buy","leavesQty":"0","execTime":"1789299703453","isMaker":false,"seq":140612148849382}]})";
const char* kDeribitBookChange =
    R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"book.BTC-PERPETUAL.100ms","data":{"timestamp":1789344931639,"type":"change","change_id":118850969743,"instrument_name":"BTC-PERPETUAL","bids":[["change",76914.0,1.0002e6],["new",76899.0,10.0],["delete",76893.0,0.0],["new",76871.5,3.0e6],["delete",76867.0,0.0],["change",38458.5,10.0],["new",38457.0,10.0]],"asks":[["new",77023.0,10.0],["delete",77027.5,0.0],["new",115372.0,10.0]],"prev_change_id":118850968930}}})";
const char* kDeribitTickerOption =
    R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"ticker.BTC-15SEP26-77000-C.100ms","data":{"timestamp":1789344931096,"state":"open","stats":{"high":0.0118,"low":0.0065,"price_change":-38.6792,"volume":1555.0,"volume_usd":1268694.68},"greeks":{"delta":0.47736,"gamma":2.8e-4,"vega":18.43602,"theta":-217.49347,"rho":1.31068},"index_price":76900.24,"instrument_name":"BTC-15SEP26-77000-C","last_price":0.0065,"settlement_price":0.00897474,"min_price":0.0001,"max_price":0.0365,"open_interest":534.4,"mark_price":0.0069,"interest_rate":0.0,"estimated_delivery_price":76900.24,"best_ask_price":0.0075,"best_bid_price":0.0065,"mark_iv":31.2,"bid_iv":29.57,"ask_iv":33.74,"underlying_price":76904.4,"underlying_index":"BTC-15SEP26","best_ask_amount":10.0,"best_bid_amount":10.0}}})";
const char* kDeribitTrades =
    R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"trades.BTC-PERPETUAL.100ms","data":[{"timestamp":1789344933994,"price":76917.5,"amount":8450.0,"direction":"buy","index_price":76906.67,"instrument_name":"BTC-PERPETUAL","trade_seq":140284854,"mark_price":76917.12,"tick_direction":0,"starbase_match_id":225046781992378368,"trade_id":"267258393","contracts":845.0,"starbase_timestamp":1789344933994718924},{"timestamp":1789344933994,"price":76918.5,"amount":6000.0,"direction":"buy","index_price":76906.67,"instrument_name":"BTC-PERPETUAL","trade_seq":140284855,"mark_price":76917.12,"tick_direction":0,"starbase_match_id":225046781992378369,"trade_id":"267258394","contracts":600.0,"starbase_timestamp":1789344933994718924},{"timestamp":1789344933994,"price":76919.5,"amount":6000.0,"direction":"buy","index_price":76906.67,"instrument_name":"BTC-PERPETUAL","trade_seq":140284856,"mark_price":76917.12,"tick_direction":0,"starbase_match_id":225046781992378370,"trade_id":"267258395","contracts":600.0,"starbase_timestamp":1789344933994718924}]}})";
const char* kDeribitUserOrder =
    R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.orders.option.BTC.raw","data":{"time_in_force":"good_til_cancelled","reduce_only":false,"price":0.0065,"post_only":true,"order_type":"limit","order_state":"open","order_id":"42710123456","max_show":1.0,"last_update_timestamp":1789345400123,"label":"fm000100000001","is_rebalance":false,"is_liquidation":false,"instrument_name":"BTC-15SEP26-77000-C","filled_amount":0.0,"direction":"buy","creation_timestamp":1789345400100,"average_price":0.0,"api":true,"amount":1.0,"contracts":1.0,"reject_post_only":true,"replaced":false,"mmp":false,"risk_reducing":false,"web":false}}})";
const char* kDeribitUserTrade =
    R"({"jsonrpc":"2.0","method":"subscription","params":{"channel":"user.trades.option.BTC.raw","data":[{"trade_seq":1966,"trade_id":"267259001","timestamp":1789345401234,"tick_direction":0,"state":"open","reduce_only":false,"price":0.006,"post_only":true,"order_type":"limit","order_id":"42710123456","matching_id":null,"mark_price":0.0061,"liquidity":"M","label":"fm000100000001","iv":30.8,"instrument_name":"BTC-15SEP26-77000-C","index_price":76950.1,"fee_currency":"BTC","fee":0.00015,"direction":"buy","amount":0.5,"contracts":0.5,"underlying_price":76955.0,"api":true,"mmp":false,"risk_reducing":false,"profit_loss":0.0}]}})";

// Deribit: 0 BTC-15SEP26-77000-C (option), 1 BTC-PERPETUAL (10 USD contracts), as in
// tests/venues/deribit_md_parser_test.cpp.
struct DeribitUniverse {
  InstrumentTable instruments;
  SymbolTable symbols;
  DeribitUniverse() {
    Instrument call{};
    call.symbol = "BTC-15SEP26-77000-C";
    call.venue = VenueId{2};
    call.base = "BTC";
    call.quote = "BTC";
    call.asset_class = AssetClass::Option;
    call.option_type = OptionType::Call;
    call.flags = Instrument::kEnabled;
    call.tick = Price::from_decimal("0.0001").value();
    call.lot = Qty::from_decimal("0.1").value();
    static_cast<void>(instruments.add(call));
    Instrument perp{};
    perp.symbol = "BTC-PERPETUAL";
    perp.venue = VenueId{2};
    perp.base = "BTC";
    perp.quote = "USD";
    perp.asset_class = AssetClass::Perpetual;
    perp.flags = Instrument::kEnabled;
    perp.tick = Price::from_decimal("0.5").value();
    perp.lot = Qty::from_int(1);
    perp.contract_multiplier = Qty::from_int(10);
    static_cast<void>(instruments.add(perp));
    static_cast<void>(symbols.build(instruments));
  }
};

void BM_Json_BybitTrade(benchmark::State& state) {
  Universe u;
  bybit::BybitMdParser p(u.symbols, VenueId{1});
  run_decode(state, kBybitTrade, p);
}
BENCHMARK(BM_Json_BybitTrade);

void BM_Json_BybitExecution(benchmark::State& state) {
  Universe u;
  bybit::BybitPrivateParser p(u.symbols, u.instruments, VenueId{1});
  run_decode(state, kBybitExecution, p);
}
BENCHMARK(BM_Json_BybitExecution);

void BM_Json_DeribitBookChange(benchmark::State& state) {
  DeribitUniverse u;
  deribit::DeribitMdParser p(u.symbols, u.instruments, VenueId{2});
  run_decode(state, kDeribitBookChange, p);
}
BENCHMARK(BM_Json_DeribitBookChange);

void BM_Json_DeribitTickerOption(benchmark::State& state) {
  DeribitUniverse u;
  deribit::DeribitMdParser p(u.symbols, u.instruments, VenueId{2});
  run_decode(state, kDeribitTickerOption, p);
}
BENCHMARK(BM_Json_DeribitTickerOption);

void BM_Json_DeribitTrades3(benchmark::State& state) {
  DeribitUniverse u;
  deribit::DeribitMdParser p(u.symbols, u.instruments, VenueId{2});
  run_decode(state, kDeribitTrades, p);
}
BENCHMARK(BM_Json_DeribitTrades3);

void BM_Json_DeribitUserOrder(benchmark::State& state) {
  DeribitUniverse u;
  deribit::DeribitPrivateParser p(u.symbols, u.instruments, VenueId{2});
  run_decode(state, kDeribitUserOrder, p);
}
BENCHMARK(BM_Json_DeribitUserOrder);

void BM_Json_DeribitUserTrade(benchmark::State& state) {
  DeribitUniverse u;
  deribit::DeribitPrivateParser p(u.symbols, u.instruments, VenueId{2});
  run_decode(state, kDeribitUserTrade, p);
}
BENCHMARK(BM_Json_DeribitUserTrade);

}  // namespace
