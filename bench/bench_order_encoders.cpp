// Venue order encoders: one new post-only limit order encoded into the connector's request buffer,
// as the network thread does for every OutNewOrder. Binance signs each WebSocket API request with
// HMAC-SHA256 (apiKey and signature inline, no session logon); Bybit's trade stream is
// authenticated once per connection, so its frame carries no signature; Deribit carries the access
// token. The ids and prices are those of tests/venues/*_order_encoder_test.cpp.
#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/binance/binance_order_encoder.hpp"
#include "fastmm/venues/bybit/bybit_auth.hpp"
#include "fastmm/venues/bybit/bybit_order_encoder.hpp"
#include "fastmm/venues/deribit/deribit_order_encoder.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <string>

using namespace fastmm;
using namespace fastmm::venues;

namespace {

// The HMAC key pair documented by Binance ("SIGNED Endpoint Examples"): realistic key lengths.
constexpr const char* kApiKey = "vmPUZE6mv9SD5VNHk4HlWFsOr6aKE2zvsw0MuIgwCIPy6utIco14y7Ju91duEh8A";
constexpr const char* kSecret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";

Price px(const char* s) {
  return Price::from_decimal(s).value();
}
Qty qt(const char* s) {
  return Qty::from_decimal(s).value();
}

Instrument instrument(const char* symbol, std::uint8_t venue, const char* tick, const char* lot) {
  Instrument i{};
  i.symbol = symbol;
  i.venue = VenueId{venue};
  i.base = "BTC";
  i.quote = "USDT";
  i.flags = Instrument::kEnabled;
  i.tick = px(tick);
  i.lot = qt(lot);
  return i;
}

struct Universe {
  InstrumentTable instruments;
  SymbolTable symbols;
  std::array<deribit::TickSchedule, kMaxInstruments> ticks{};
  Universe() {
    static_cast<void>(instruments.add(instrument("BTCUSDT", 0, "0.01", "0.00001")));  // 0 Binance
    static_cast<void>(instruments.add(instrument("BTCUSDT", 1, "0.1", "0.000001")));  // 1 Bybit
    Instrument call = instrument("BTC-15SEP26-77000-C", 2, "0.0001", "0.1");          // 2 Deribit
    call.asset_class = AssetClass::Option;
    call.quote = "BTC";
    static_cast<void>(instruments.add(call));
    static_cast<void>(symbols.build(instruments));
    ticks[2].base = px("0.0001");
    static_cast<void>(ticks[2].steps.push_back(deribit::TickStep{px("0.005"), px("0.0005")}));
  }
};

OutNewOrderMsg new_order(std::uint32_t instrument,
                         std::uint8_t venue,
                         const char* price,
                         const char* qty) {
  OutNewOrderMsg n{};
  init_header(n, EventType::OutNewOrder, InstrumentId{instrument}, VenueId{venue});
  n.cl_ord_id = decode_cl_ord_id("fm000100000001").value();
  n.side = Side::Buy;
  n.type = OrderType::PostOnly;
  n.tif = TimeInForce::Gtc;
  n.price = px(price);
  n.qty = qt(qty);
  return n;
}

template <class Encode>
void run_encode(benchmark::State& state, const OutNewOrderMsg& msg, Encode&& encode) {
  const OrderCommand cmd = *OrderCommand::from(msg.hdr);
  std::array<char, 4096> buf{};
  std::size_t bytes = 0;
  for (auto _ : state) {
    bytes = encode(cmd, std::span<char>(buf));
    benchmark::DoNotOptimize(bytes);
    if (bytes == 0) state.SkipWithError("encode failed");
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes));
}

void BM_Encode_BinanceOrderPlace(benchmark::State& state) {
  Universe u;
  binance::Credentials c;
  c.api_key = kApiKey;
  c.secret.value = kSecret;
  const binance::Signer signer(c);
  binance::BinanceOrderEncoder enc(signer, u.symbols, 3000);
  run_encode(state,
             new_order(0, 0, "70000.5", "0.001"),
             [&](const OrderCommand& cmd, std::span<char> out) {
               return enc.encode_ws(cmd, nullptr, 1789295199000, out);
             });
}
BENCHMARK(BM_Encode_BinanceOrderPlace);

void BM_Encode_BybitOrderCreate(benchmark::State& state) {
  Universe u;
  bybit::Credentials c;
  c.api_key = kApiKey;
  c.secret.value = kSecret;
  const bybit::Signer signer(c);
  const bybit::BybitOrderEncoder enc(signer, u.symbols, 5000);
  run_encode(state,
             new_order(1, 1, "60000.1", "0.001"),
             [&](const OrderCommand& cmd, std::span<char> out) {
               return enc.encode_ws(cmd, nullptr, 1789299700000, out);
             });
}
BENCHMARK(BM_Encode_BybitOrderCreate);

void BM_Encode_DeribitBuy(benchmark::State& state) {
  Universe u;
  const deribit::DeribitOrderEncoder enc(u.symbols, u.instruments, u.ticks, true);
  // A Deribit access token is ~80 characters.
  const std::string token(80, 'a');
  run_encode(
      state, new_order(2, 2, "0.00523", "0.5"), [&](const OrderCommand& cmd, std::span<char> out) {
        return enc.encode(cmd, nullptr, token, out);
      });
}
BENCHMARK(BM_Encode_DeribitBuy);

}  // namespace
