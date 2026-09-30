#pragma once
// Control-path decoders for Coinbase Exchange REST bodies (allocation allowed). Numbers are strings
// unless noted (https://docs.cdp.coinbase.com/api-reference/exchange-api/rest-api/, read
// 2026-09-30):
//   GET /products/P     {id, base_currency, quote_currency, quote_increment, base_increment,
//                        min_market_funds, status online | ..., trading_disabled, cancel_only,
//                        limit_only, post_only (booleans); base_min_size where still sent}
//   GET /time           {iso, epoch (a JSON number, seconds)}
//   GET /orders, POST /orders, GET /orders/<id>
//                       {id, client_oid, product_id, side, type, price, size, filled_size,
//                        status open | pending | active | done | rejected, reject_reason,
//                        post_only}
//   GET /fills          [{trade_id (number), order_id, product_id, price, size, fee, side,
//                         liquidity M | T | O, created_at}], newest first
//   DELETE /orders      ["<order id>", ..]; DELETE /orders/<id> "<order id>"
//   GET /accounts       [{id, currency, balance, hold, available, profile_id, trading_enabled
//                        (boolean), pending_deposit, display_name}], one per currency of the
//                        key's profile, not paged; amounts with 16 decimals
//                        (.../rest-api/accounts/get-all-account-profile)
//   an error            {"message": "..."}
#include "fastmm/core/fixed_point.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::coinbase {

struct ProductInfo {
  std::string id;
  std::string base;
  std::string quote;
  std::string status;
  Price tick{};
  Qty lot{};
  Qty min_size{};        // base_min_size when sent, else zero
  Notional min_funds{};  // min_market_funds
  bool trading_disabled = false;
  bool cancel_only = false;
  bool limit_only = false;
  bool post_only = false;
};

struct OrderRow {
  std::string id;
  std::string client_oid;
  std::string product_id;
  std::string side;
  std::string type;
  std::string status;
  std::string reject_reason;
  Price price{};
  Qty size{};
  Qty filled_size{};
};

struct FillRow {
  std::int64_t trade_id = 0;
  std::string order_id;
  std::string product_id;
  std::string side;
  std::string liquidity;  // M | T | O
  Price price{};
  Qty size{};
  Notional fee{};  // quote currency, paid
  std::int64_t time_ms = 0;
};

// A trading account of the profile: available is free, hold is locked ("Holds are placed on an
// account for any active orders or pending withdraw requests"), balance = available + hold.
// Amounts truncated to 8 decimals.
struct AccountRow {
  std::string id;
  std::string currency;
  Notional balance{};
  Notional available{};
  Notional hold{};
  bool trading_enabled = false;
};

// Empty string on success, else an error description.
std::string decode_accounts(std::string_view json, std::vector<AccountRow>& out);
std::string decode_product(std::string_view json, ProductInfo& out);
std::string decode_server_time(std::string_view json, std::int64_t& epoch_ms);
std::string decode_orders(std::string_view json, std::vector<OrderRow>& out);
std::string decode_order(std::string_view json, OrderRow& out);
std::string decode_fills(std::string_view json, std::vector<FillRow>& out);
// ["id", ..] (cancel all) or "id" (one cancel).
std::string decode_ids(std::string_view json, std::vector<std::string>& out);
// {"message": ...}; empty when the body is not one.
std::string error_message(std::string_view json);

}  // namespace fastmm::venues::coinbase
