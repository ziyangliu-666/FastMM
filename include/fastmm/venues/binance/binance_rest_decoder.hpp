#pragma once
// Control-path decoders for Binance REST bodies (allocation allowed; never on the hot path):
//   GET /api/v3/exchangeInfo  -> per-symbol filters (rest-api.md "Exchange information",
//                                filters.md PRICE_FILTER / LOT_SIZE / NOTIONAL / MIN_NOTIONAL)
//   GET /api/v3/time          -> serverTime
//   {"code":..,"msg":..}      -> error envelope (errors.md)
//   POST /api/v3/userDataStream -> listenKey (legacy; the sim exchange)
#include "fastmm/core/fixed_point.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::binance {

struct SymbolFilters {
  std::string symbol;
  std::string status;  // "TRADING"
  std::string base_asset;
  std::string quote_asset;
  Price tick{};                   // PRICE_FILTER.tickSize
  Qty step{};                     // LOT_SIZE.stepSize
  Qty min_qty{};                  // LOT_SIZE.minQty
  Qty max_qty{};                  // LOT_SIZE.maxQty
  Notional min_notional{};        // NOTIONAL.minNotional or MIN_NOTIONAL.minNotional
  Notional max_notional{};        // NOTIONAL.maxNotional (0 = none)
  bool post_only_allowed = true;  // "LIMIT_MAKER" in orderTypes
};

struct RateLimitRule {
  std::string type;      // REQUEST_WEIGHT | ORDERS | RAW_REQUESTS
  std::string interval;  // SECOND | MINUTE | DAY
  int interval_num = 1;
  std::int64_t limit = 0;
  [[nodiscard]] std::int64_t window_ns() const noexcept {
    std::int64_t s = interval_num;
    if (interval == "MINUTE") s *= 60;
    if (interval == "HOUR") s *= 3600;
    if (interval == "DAY") s *= 86400;
    return s * 1'000'000'000;
  }
};

struct ExchangeInfo {
  std::int64_t server_time_ms = 0;
  std::vector<RateLimitRule> rate_limits;
  std::vector<SymbolFilters> symbols;
};

// Returns an error description or empty on success.
std::string decode_exchange_info(std::string_view json, ExchangeInfo& out);
std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms);
// True when the body is a {"code":..,"msg":..} error envelope.
bool decode_rest_error(std::string_view json, int& code, std::string& msg);
std::string decode_listen_key(std::string_view json, std::string& listen_key);

}  // namespace fastmm::venues::binance
