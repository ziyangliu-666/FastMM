#pragma once
// Control-path decoders for Bybit REST bodies (allocation allowed):
//   GET /v5/market/instruments-info?category=spot
//   (https://bybit-exchange.github.io/docs/v5/market/instrument)
//       result.list[] {symbol, baseCoin, quoteCoin, status, lotSizeFilter{basePrecision,
//       minOrderQty, maxOrderQty, minOrderAmt, maxOrderAmt}, priceFilter{tickSize}}
//   GET /v5/market/time -> result.timeNano / top-level `time` (ms)
#include "fastmm/core/fixed_point.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::bybit {

struct InstrumentInfo {
  std::string symbol;
  std::string base_coin;
  std::string quote_coin;
  std::string status;    // "Trading"
  Price tick{};          // priceFilter.tickSize
  Qty base_precision{};  // lotSizeFilter.basePrecision (quantity step)
  Qty min_qty{};
  Qty max_qty{};
  Notional min_amount{};
  Notional max_amount{};
};

// Empty string on success, else an error description.
std::string decode_instruments(std::string_view json, std::vector<InstrumentInfo>& out);
std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms);
// {"retCode":..,"retMsg":..}; false if the body is not an envelope.
bool decode_envelope(std::string_view json, int& ret_code, std::string& ret_msg);

}  // namespace fastmm::venues::bybit
