#pragma once
// Control-path decoders for Bybit REST bodies (allocation allowed):
//   GET /v5/market/instruments-info?category=spot|linear
//   (https://bybit-exchange.github.io/docs/v5/market/instrument)
//       spot:   result.list[] {symbol, baseCoin, quoteCoin, status, lotSizeFilter{basePrecision,
//               minOrderQty, maxOrderQty, minOrderAmt, maxOrderAmt}, priceFilter{tickSize}}
//       linear: result.list[] {symbol, contractType, status, baseCoin, quoteCoin, settleCoin,
//               lotSizeFilter{qtyStep, minOrderQty, maxOrderQty, minNotionalValue},
//               priceFilter{tickSize}}; qty is in the base coin, so the multiplier is 1
//   GET /v5/market/time -> result.timeNano / top-level `time` (ms)
//   GET /v5/position/list (https://bybit-exchange.github.io/docs/v5/position)
//       result.list[] {symbol, positionIdx, side Buy|Sell|"", size (unsigned), avgPrice},
//       result.nextPageCursor
//   GET /v5/account/wallet-balance?accountType=UNIFIED
//   (https://bybit-exchange.github.io/docs/v5/account/wallet-balance, read 2026-09-30)
//       result.list[0] {accountType, totalEquity, totalWalletBalance, totalAvailableBalance,
//               totalInitialMargin, totalMaintenanceMargin, coin[] {coin, walletBalance, locked,
//               equity, totalOrderIM, totalPositionIM, totalPositionMM, bonus}}, top-level `time`
//               (ms); mapped as bybit_balance.hpp says
//   GET /v5/account/info (https://bybit-exchange.github.io/docs/v5/account/account-info)
//       result.marginMode ISOLATED_MARGIN | REGULAR_MARGIN | PORTFOLIO_MARGIN
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/venues/bybit/bybit_balance.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::bybit {

struct InstrumentInfo {
  std::string symbol;
  std::string base_coin;
  std::string quote_coin;
  std::string status;         // "Trading"
  std::string settle_coin;    // linear
  std::string contract_type;  // linear: "LinearPerpetual" | "LinearFutures"
  Price tick{};               // priceFilter.tickSize
  Qty base_precision{};       // lotSizeFilter.basePrecision (spot) or qtyStep (linear)
  Qty min_qty{};
  Qty max_qty{};
  Notional min_amount{};  // minOrderAmt (spot) or minNotionalValue (linear)
  Notional max_amount{};
};

// One row of GET /v5/position/list.
struct PositionRecord {
  std::string symbol;
  int position_idx = 0;  // 0 one-way, 1 / 2 the buy / sell side in hedge mode
  Qty qty{};             // signed: size, negative when side is Sell
  Price avg_px{};
};

// GET /v5/account/wallet-balance, mapped (bybit_balance.hpp).
struct WalletCoin {
  std::string coin;
  BalanceFields fields;
};
struct WalletBalance {
  std::vector<WalletCoin> coins;
  BalanceFields account{};   // the account-level fields in USD
  std::int64_t time_ms = 0;  // the reply's `time`
};

// Empty string on success, else an error description.
std::string decode_wallet_balance(std::string_view json, WalletBalance& out);
std::string decode_margin_mode(std::string_view json, std::string& margin_mode);
std::string decode_instruments(std::string_view json, std::vector<InstrumentInfo>& out);
// Empty string on success; `next_cursor` receives result.nextPageCursor.
std::string decode_positions(std::string_view json,
                             std::vector<PositionRecord>& out,
                             std::string& next_cursor);
std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms);
// {"retCode":..,"retMsg":..}; false if the body is not an envelope.
bool decode_envelope(std::string_view json, int& ret_code, std::string& ret_msg);

}  // namespace fastmm::venues::bybit
