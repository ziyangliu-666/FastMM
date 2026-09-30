#pragma once
// Control-path decoders for Gemini REST bodies (allocation allowed). Shapes from the OpenAPI spec
// https://developer.gemini.com/specs/openapi/rest.yaml and production replies (read 2026-09-30).
// A failed request is a non-200 status with {"result":"error","reason":R,"message":M}.
//
//   GET  /v1/symbols/details/{symbol}  {symbol, base_currency, quote_currency, tick_size
//        (number: the quantity step, "the number of decimal places in the base_currency"),
//        quote_increment (number: the price step), min_order_size (string), status open |
//        closed | cancel_only | post_only | limit_only, product_type spot | swap, contract_type
//        vanilla | linear | inverse, contract_price_currency (a perpetual's collateral)}
//   POST /v1/orders           [{order_id, client_order_id, symbol, side buy | sell, price,
//                              original_amount, executed_amount, remaining_amount, is_live}]
//   POST /v1/positions        {"openPositions":[..]} (the spec's schema) or a bare array (its
//                              example): {symbol, instrument_type spot | perp, quantity (negative
//                              short), average_cost, mark_price}
//   POST /v1/mytrades         [{price, amount, timestampms, type Buy | Sell, aggressor,
//                              fee_currency, fee_amount, tid (number), order_id, client_order_id,
//                              symbol, break}], sorted by timestamp descending
//   POST /v1/perpetuals/fundingPayment
//                             [{eventType, hourlyFundingTransfer:{timestamp, assetCode, action
//                              Credit | Debit, quantity:{currency, value}, instrumentSymbol}}]
//   POST /v1/balances         [{type exchange, currency, amount ("the confirmed balance"),
//                              available ("the amount available for trading ... reduced
//                              immediately when an order hold or withdrawal hold is placed"),
//                              availableForWithdrawal, _timestamp (ISO 8601, "server-side
//                              monotonically increasing clock value")}]; numbers or strings, up to
//                              19 decimals ("truncated to 15 and 19 decimal places"): rounded to 8
//   POST /v1/margin {symbol}  {margin_assets_value ("the $ equivalent value of all the assets ...
//                              that can contribute to funding a derivatives position"),
//                              initial_margin (positions and open orders), available_margin
//                              (margin_assets_value - initial_margin), margin_maintenance_limit,
//                              ..} as decimal strings; an exchange account answers
//                              AccountNotOfTypeRequired
//   POST /v1/order/cancel/session, /v1/order/cancel/all
//                             {"result":"ok","details":{"cancelledOrders":[..],"cancelRejects":[..]}}
#include "fastmm/core/fixed_point.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::gemini {

struct SymbolDetails {
  std::string symbol;
  std::string base;
  std::string quote;
  std::string collateral;     // contract_price_currency
  std::string product_type;   // spot | swap
  std::string contract_type;  // vanilla | linear | inverse
  std::string status;         // open | closed | cancel_only | post_only | limit_only
  Price tick{};               // quote_increment
  Qty lot{};                  // tick_size
  Qty min_qty{};              // min_order_size
};

struct ActiveOrder {
  std::string order_id;
  std::string client_order_id;
  std::string symbol;
  std::string side;  // buy | sell
  Price price{};
  Qty original{};
  Qty executed{};
};

struct PositionRow {
  std::string symbol;
  std::string instrument_type;  // spot | perp
  Qty qty{};                    // signed
  Price avg_px{};
};

struct TradeRow {
  std::string tid;
  std::string order_id;
  std::string client_order_id;
  std::string symbol;
  std::string fee_currency;
  std::string break_type;  // "" or the reason the trade was broken
  bool buy = false;
  bool aggressor = false;
  Price price{};
  Qty qty{};
  Notional fee{};  // as sent: positive paid
  std::int64_t time_ms = 0;
};

struct FundingRow {
  std::string symbol;
  std::string asset;
  Notional amount{};  // signed: Credit positive, Debit negative
  std::int64_t time_ms = 0;
};

struct BalanceRow {
  std::string currency;
  Notional amount{};         // confirmed
  Notional available{};      // tradable now
  std::int64_t time_ms = 0;  // _timestamp; 0 when absent
};

// The derivatives account's margin, in USD.
struct MarginRow {
  Notional assets_value{};  // margin_assets_value
  Notional initial{};       // initial_margin
  Notional available{};     // available_margin
  Notional maintenance{};   // margin_maintenance_limit
};

// Empty string on success, else an error description (an error envelope included).
std::string decode_symbol_details(std::string_view json, SymbolDetails& out);
std::string decode_active_orders(std::string_view json, std::vector<ActiveOrder>& out);
std::string decode_positions(std::string_view json, std::vector<PositionRow>& out);
std::string decode_trades(std::string_view json, std::vector<TradeRow>& out);
std::string decode_funding(std::string_view json, std::vector<FundingRow>& out);
std::string decode_balances(std::string_view json, std::vector<BalanceRow>& out);
std::string decode_margin(std::string_view json, MarginRow& out);
// cancel/session and cancel/all: the order ids the venue did not cancel.
std::string decode_cancel_result(std::string_view json,
                                 std::size_t& cancelled,
                                 std::vector<std::string>& rejects);
// {"result":"error","reason":..,"message":..}; false when the body is not one.
bool decode_error(std::string_view json, std::string& reason, std::string& message);

}  // namespace fastmm::venues::gemini
