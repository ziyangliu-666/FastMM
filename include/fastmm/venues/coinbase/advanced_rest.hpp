#pragma once
// Coinbase Advanced Trade REST requests and replies (OpenAPI spec
// https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/rest-api/advanced-trade-spec.yaml,
// read 2026-09-30), base https://api.coinbase.com:
//
//   POST /api/v3/brokerage/orders   {"client_order_id","product_id","side":"BUY"|"SELL",
//        "order_configuration":{"limit_limit_gtc":{"base_size","limit_price","post_only"}} |
//        {"sor_limit_ioc":{..}} | {"limit_limit_fok":{..}} | {"market_market_ioc":{"base_size"}}}
//        -> {"success", "success_response":{"order_id",..}, "error_response":{..}}
//   POST /api/v3/brokerage/orders/batch_cancel {"order_ids":[..]}
//        -> {"results":[{"success","failure_reason","order_id"}]}
//   GET  /api/v3/brokerage/orders/historical/batch?order_status=OPEN&product_ids=..&limit=&cursor=
//        -> {"orders":[..],"has_next","cursor"}
//   GET  /api/v3/brokerage/orders/historical/fills?product_ids=..|order_ids=..
//        &start_sequence_timestamp=&end_sequence_timestamp=&limit=&cursor= -> {"fills":[..],
//        "cursor"}
//   GET  /api/v3/brokerage/orders/historical/<order_id> -> {"order":{..}}
//   GET  /api/v3/brokerage/market/products/<id> (public), /api/v3/brokerage/time (public),
//        /api/v3/brokerage/accounts
// FastMM's client order id ("fm" + 12 hex digits) is the client_order_id as it is. There is no
// cancel by client id and no cancel-all: a cancel names the venue's order_id.
// encode_new() writes into fixed buffers and does not allocate; the rest builds std::strings.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/venues/coinbase/coinbase_order_encoder.hpp"
#include "fastmm/venues/coinbase/coinbase_rest_decoder.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::coinbase {

inline constexpr std::string_view kAdvancedPrefix = "/api/v3/brokerage";

class AdvancedOrderEncoder {
 public:
  explicit AdvancedOrderEncoder(const SymbolTable& symbols) noexcept : symbols_(symbols) {}

  // POST /api/v3/brokerage/orders for a New; false for an unknown instrument or a request that
  // does not fit.
  bool encode_new(const OrderCommand& cmd, OrderRequest& out) const noexcept;

  // POST /api/v3/brokerage/orders/batch_cancel.
  static std::string cancel_body(std::span<const std::string> order_ids);
  static std::string cancel_path() { return "/api/v3/brokerage/orders/batch_cancel"; }
  static std::string open_orders_path(std::span<const std::string> products,
                                      std::string_view cursor);
  static std::string fills_path(std::span<const std::string> products,
                                std::int64_t start_ms,
                                std::int64_t end_ms,
                                std::string_view cursor,
                                int limit);
  static std::string order_fills_path(std::string_view order_id);
  static std::string order_path(std::string_view order_id);
  static std::string product_path(std::string_view product);

 private:
  const SymbolTable& symbols_;
};

struct CreateReply {
  bool success = false;
  std::string order_id;
  std::string failure_reason;  // new_order_failure_reason, else error, else preview_failure_reason
  std::string message;
};

struct CancelResult {
  bool success = false;
  std::string order_id;
  std::string failure_reason;
};

struct AdvOrderRow {
  std::string order_id;
  std::string client_order_id;
  std::string product_id;
  std::string side;    // BUY | SELL
  std::string status;  // OPEN | ...
  Price price{};       // limit_price of the order's configuration
  Qty size{};          // base_size of the order's configuration
  Qty filled_size{};
};

struct AdvFillRow {
  std::string trade_id;
  std::string order_id;
  std::string product_id;
  std::string side;       // BUY | SELL: the order's
  std::string liquidity;  // MAKER | TAKER
  Price price{};
  Qty size{};
  Notional commission{};
  std::int64_t time_ms = 0;  // trade_time
};

// Empty string on success, else an error description.
std::string decode_adv_product(std::string_view json, ProductInfo& out);
std::string decode_adv_time(std::string_view json, std::int64_t& epoch_ms);
std::string decode_create_reply(std::string_view json, CreateReply& out);
std::string decode_cancel_results(std::string_view json, std::vector<CancelResult>& out);
std::string decode_adv_orders(std::string_view json,
                              std::vector<AdvOrderRow>& out,
                              std::string& cursor,
                              bool& has_next);
std::string decode_adv_order(std::string_view json, AdvOrderRow& out);
std::string decode_adv_fills(std::string_view json,
                             std::vector<AdvFillRow>& out,
                             std::string& cursor);
// GET /accounts: the number of accounts and whether the reply had the documented shape.
std::string decode_adv_accounts(std::string_view json, std::size_t& accounts, bool& has_next);
// {"error","code","message"} (or {"message"}); empty when the body is not one.
std::string adv_error_message(std::string_view json);

}  // namespace fastmm::venues::coinbase
