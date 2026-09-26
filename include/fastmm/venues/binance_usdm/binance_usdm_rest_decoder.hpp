#pragma once
// Binance USDⓈ-M futures REST response decoders (control path, allocating): exchange information,
// position risk, position mode and symbol configuration. Server time, error bodies and listenKeys
// have the same shape as on Spot (binance_rest_decoder.hpp).
//
//   GET /fapi/v1/exchangeInfo      symbols[]{symbol,status,contractType,baseAsset,quoteAsset,
//                                  marginAsset,timeInForce[],filters[]}: PRICE_FILTER.tickSize,
//                                  LOT_SIZE.{stepSize,minQty,maxQty}, MIN_NOTIONAL.notional
//   GET /fapi/v3/positionRisk      [{symbol,positionSide,positionAmt,entryPrice,...}]
//   GET /fapi/v1/positionSide/dual {"dualSidePosition":bool}
//   GET /fapi/v1/symbolConfig      [{symbol,marginType,isAutoAddMargin,leverage,maxNotionalValue}]
//   GET /fapi/v1/income            [{symbol,incomeType,income,asset,info,time,tranId,tradeId}]
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/venues/binance/binance_rest_decoder.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::binance_usdm {

struct SymbolInfo {
  std::string symbol;
  std::string status;         // "TRADING"
  std::string contract_type;  // "PERPETUAL", "CURRENT_QUARTER", ...
  std::string base_asset;
  std::string quote_asset;
  std::string margin_asset;
  Price tick{};             // PRICE_FILTER.tickSize
  Qty step{};               // LOT_SIZE.stepSize
  Qty min_qty{};            // LOT_SIZE.minQty
  Qty max_qty{};            // LOT_SIZE.maxQty
  Notional min_notional{};  // MIN_NOTIONAL.notional
  bool gtx_allowed = true;  // "GTX" in timeInForce
};

struct ExchangeInfo {
  std::int64_t server_time_ms = 0;
  std::vector<binance::RateLimitRule> rate_limits;
  std::vector<SymbolInfo> symbols;
};

// Decodes only the symbols in `wanted` (case-insensitive); all of them when `wanted` is empty.
std::string decode_exchange_info(std::string_view json,
                                 ExchangeInfo& out,
                                 std::span<const std::string> wanted = {});

struct PositionRecord {
  std::string symbol;
  std::string position_side;  // "BOTH" in one-way mode
  Qty qty{};                  // positionAmt, signed
  Price entry_price{};
};
std::string decode_position_risk(std::string_view json, std::vector<PositionRecord>& out);

std::string decode_position_mode(std::string_view json, bool& dual_side_position);

struct SymbolConfig {
  std::string symbol;
  std::string margin_type;  // "CROSSED" | "ISOLATED"
  std::int64_t leverage = 0;
};
std::string decode_symbol_config(std::string_view json, std::vector<SymbolConfig>& out);

// GET /fapi/v3/balance: [{asset,balance,availableBalance,...}]
struct BalanceRecord {
  std::string asset;
  Notional balance{};
  Notional available{};
};
std::string decode_balance(std::string_view json, std::vector<BalanceRecord>& out);

// GET /fapi/v1/income: one row per income entry ("Get Income History"). tranId is "unique in the
// same incomeType for a user"; income is signed (negative paid).
struct IncomeRecord {
  std::string symbol;
  std::string income_type;  // "FUNDING_FEE"
  Notional income{};
  std::string asset;
  std::int64_t time_ms = 0;
  std::int64_t tran_id = 0;
};
std::string decode_income(std::string_view json, std::vector<IncomeRecord>& out);

}  // namespace fastmm::venues::binance_usdm
