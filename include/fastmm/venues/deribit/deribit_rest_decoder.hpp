#pragma once
// Control-path decoders for Deribit JSON-RPC bodies (allocation allowed). REST base
// https://test.deribit.com/api/v2 (https://docs.deribit.com/articles/json-rpc-overview, "HTTP
// REST").
//
//   public/get_instruments?currency=C[&kind=K][&expired=false]
//   (https://docs.deribit.com/api-reference/market-data/public-get_instruments)
//     result[] {instrument_name, kind, option_type, strike, expiration_timestamp (ms),
//               tick_size, tick_size_steps[] {above_price, tick_size}, contract_size,
//               min_trade_amount, instrument_type linear|reversed, settlement_period,
//               base_currency, quote_currency, counter_currency, settlement_currency,
//               is_active, state, maker_commission, taker_commission}
//     The OpenAPI schema types tick_size_steps as an object; the testnet sends an array (possibly
//     empty), which is what is decoded. settlement_period is documented as month|week|perpetual;
//     the testnet also sends "day" (daily expiries).
//   public/get_time -> result (ms)
//   any method      -> {"result": ..} or {"error": {"code", "message", "data"}}
#include "fastmm/core/fixed_point.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::deribit {

struct TickStep {
  Price above_price{};  // the step tick applies to prices above this
  Price tick{};
};

struct InstrumentInfo {
  std::string name;
  std::string kind;             // future | option | spot | future_combo | option_combo
  std::string option_type;      // call | put (options)
  std::string instrument_type;  // linear | reversed
  std::string settlement_period;
  std::string base_currency;
  std::string quote_currency;
  std::string counter_currency;
  std::string settlement_currency;
  std::string state;  // open | settlement | delivered | inactive | locked | halted | archivized
  bool is_active = false;
  Price tick{};
  std::vector<TickStep> tick_steps;
  Qty contract_size{};
  Qty min_trade_amount{};
  Price strike{};
  std::int64_t expiration_ms = 0;
  double maker_commission = 0.0;
  double taker_commission = 0.0;
  // Inverse means priced in one currency and settled in another: the perpetual and the futures,
  // quoted in USD and settled in BTC. A BTC option is `reversed` too but priced in the coin it
  // settles in (quote_currency BTC): its PnL is linear in the premium, and treating it as inverse
  // valued a 0.0065 BTC option at qty / 0.0065 BTC of notional.
  // A `reversed` option priced in the coin it settles in (quote_currency BTC).
  [[nodiscard]] bool coin_quoted() const noexcept {
    return instrument_type == "reversed" && kind == "option" && !quote_currency.empty() &&
           quote_currency == settlement_currency;
  }
  [[nodiscard]] bool inverse() const noexcept {
    if (instrument_type != "reversed") return false;
    return quote_currency.empty() || settlement_currency.empty() ||
           quote_currency != settlement_currency;
  }
};

struct RpcEnvelope {
  bool has_result = false;
  std::int64_t error_code = 0;  // 0 when the body carries a result
  std::string message;          // error.message
  std::string reason;           // error.data.reason, when present
};

// Empty string on success, else an error description.
std::string decode_instruments(std::string_view json, std::vector<InstrumentInfo>& out);
std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms);
// False if the body is not a JSON-RPC envelope.
bool decode_envelope(std::string_view json, RpcEnvelope& out);

}  // namespace fastmm::venues::deribit
