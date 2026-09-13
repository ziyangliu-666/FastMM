#pragma once
// Binance Spot wire encoding for the simulated exchange (8.3, ADR-0008): JSON builders for
// every REST body, WebSocket API envelope, market-data stream payload and user-data event the
// server emits. Builders append to a caller-owned std::string (the server reuses buffers).
// Decimals are printed with 8 fraction digits like Binance ("0.00100000"); no double.
//
// This header deliberately has no JSON library dependency; request parsing (simdjson) lives in
// request.hpp / request.cpp.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::sim::server {

enum class BinanceOrderType : std::uint8_t { Limit = 0, LimitMaker = 1, Market = 2 };
enum class BinanceOrderStatus : std::uint8_t {
  New = 0,
  PartiallyFilled = 1,
  Filled = 2,
  Canceled = 3,
  Rejected = 4,
  Expired = 5,
  ExpiredInMatch = 6,
};
enum class ExecType : std::uint8_t {
  New = 0,
  Canceled = 1,
  Replaced = 2,
  Rejected = 3,
  Trade = 4,
  Expired = 5,
  TradePrevention = 6,
};

[[nodiscard]] std::string_view to_text(BinanceOrderType t) noexcept;
[[nodiscard]] std::string_view to_text(BinanceOrderStatus s) noexcept;
[[nodiscard]] std::string_view to_text(ExecType x) noexcept;
[[nodiscard]] std::string_view side_text(Side s) noexcept;
[[nodiscard]] std::string_view tif_text(TimeInForce t) noexcept;
[[nodiscard]] bool is_terminal(BinanceOrderStatus s) noexcept;

// ---- primitives -----------------------------------------------------------------------------

void append_int(std::string& out, std::int64_t v);
void append_uint(std::string& out, std::uint64_t v);
// "-12.34000000": integer part, '.', exactly 8 fraction digits.
void append_decimal_raw(std::string& out, std::int64_t raw);
template <class Tag>
void append_decimal(std::string& out, Fixed<Tag> v) {
  append_decimal_raw(out, v.raw);
}
// Quoted JSON string with the mandatory escapes.
void append_json_string(std::string& out, std::string_view s);

// Streaming writer for one JSON object: {"k":v,...}. Keys are trusted literals.
class JsonObjectWriter {
 public:
  explicit JsonObjectWriter(std::string& out) : out_(out) { out_.push_back('{'); }
  JsonObjectWriter(const JsonObjectWriter&) = delete;
  JsonObjectWriter& operator=(const JsonObjectWriter&) = delete;

  JsonObjectWriter& str(std::string_view k, std::string_view v) {
    key(k);
    append_json_string(out_, v);
    return *this;
  }
  JsonObjectWriter& num(std::string_view k, std::int64_t v) {
    key(k);
    append_int(out_, v);
    return *this;
  }
  JsonObjectWriter& unum(std::string_view k, std::uint64_t v) {
    key(k);
    append_uint(out_, v);
    return *this;
  }
  template <class Tag>
  JsonObjectWriter& dec(std::string_view k, Fixed<Tag> v) {
    key(k);
    out_.push_back('"');
    append_decimal_raw(out_, v.raw);
    out_.push_back('"');
    return *this;
  }
  JsonObjectWriter& boolean(std::string_view k, bool v) {
    key(k);
    out_ += v ? "true" : "false";
    return *this;
  }
  JsonObjectWriter& null(std::string_view k) {
    key(k);
    out_ += "null";
    return *this;
  }
  // `json` must already be valid JSON.
  JsonObjectWriter& raw(std::string_view k, std::string_view json) {
    key(k);
    out_ += json;
    return *this;
  }
  void close() { out_.push_back('}'); }

 private:
  void key(std::string_view k) {
    if (!first_) out_.push_back(',');
    first_ = false;
    out_.push_back('"');
    out_ += k;
    out_ += "\":";
  }
  std::string& out_;
  bool first_ = true;
};

// ---- orders -----------------------------------------------------------------------------------

// Everything the order-shaped bodies need about one order.
struct OrderView {
  std::string_view symbol;
  std::int64_t order_id = 0;
  std::string_view client_order_id;
  Side side = Side::Buy;
  BinanceOrderType type = BinanceOrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  Price price{};
  Qty orig_qty{};
  Qty executed_qty{};
  Notional cum_quote{};
  BinanceOrderStatus status = BinanceOrderStatus::New;
  std::int64_t time_ms = 0;
  std::int64_t update_ms = 0;
};

struct FillView {
  Price price{};
  Qty qty{};
  Notional commission{};
  std::string_view commission_asset;
  std::uint64_t trade_id = 0;
};

// GET /api/v3/openOrders element, openOrders.status / order.status result.
void append_order_object(std::string& out, const OrderView& o);
// newOrderRespType=ACK
void append_ack_result(std::string& out, const OrderView& o, std::int64_t transact_ms);
// newOrderRespType=RESULT (fills == nullptr) or FULL
void append_result_result(std::string& out,
                          const OrderView& o,
                          std::int64_t transact_ms,
                          std::span<const FillView> fills,
                          bool full);
// DELETE /api/v3/order, order.cancel, openOrders.cancelAll element.
void append_cancel_result(std::string& out,
                          const OrderView& o,
                          std::string_view cancel_client_order_id,
                          std::int64_t transact_ms);
// order.amend.keepPriority / PUT /api/v3/order/amend/keepPriority
void append_amend_result(std::string& out,
                         const OrderView& o,
                         std::string_view orig_client_order_id,
                         std::int64_t transact_ms,
                         std::int64_t execution_id);

// {"code":-NNNN,"msg":"..."}
void append_error(std::string& out, int code, std::string_view msg);
// {"code":-NNNN,"msg":"...","data":<data_json>}
void append_error_with_data(std::string& out,
                            int code,
                            std::string_view msg,
                            std::string_view data_json);

// ---- user data events ---------------------------------------------------------------------------

struct ExecReportView {
  OrderView order;                        // order.client_order_id goes into "c"
  ExecType exec_type = ExecType::New;     // x
  std::string_view orig_client_order_id;  // C ("" unless CANCELED / REPLACED)
  std::string_view reject_reason = "NONE";
  Qty last_qty{};
  Price last_price{};
  Notional commission{};
  std::string_view commission_asset;  // "" -> null
  std::int64_t trade_id = -1;
  bool maker = false;
  bool working = true;
  std::int64_t event_ms = 0;
  std::int64_t transact_ms = 0;
  std::int64_t execution_id = 0;
};
void append_execution_report(std::string& out, const ExecReportView& r);

struct BalanceView {
  std::string_view asset;
  Qty free{};
  Qty locked{};
};
void append_account_position(std::string& out,
                             std::int64_t event_ms,
                             std::int64_t update_ms,
                             std::span<const BalanceView> balances);
// GET /api/v3/account, account.status
void append_account_info(std::string& out,
                         std::int64_t update_ms,
                         std::int64_t maker_bps,
                         std::int64_t taker_bps,
                         std::span<const BalanceView> balances);
// {"e":"listenKeyExpired","E":ms,"listenKey":"..."}
void append_listen_key_expired(std::string& out, std::int64_t event_ms, std::string_view key);

// ---- market data ------------------------------------------------------------------------------

void append_depth_update(std::string& out,
                         std::int64_t event_ms,
                         std::string_view symbol,
                         std::uint64_t first_update_id,
                         std::uint64_t last_update_id,
                         std::span<const Level> bids,
                         std::span<const Level> asks);
void append_book_ticker(
    std::string& out, std::uint64_t update_id, std::string_view symbol, Level bid, Level ask);
void append_trade(std::string& out,
                  std::int64_t event_ms,
                  std::string_view symbol,
                  std::uint64_t trade_id,
                  Price price,
                  Qty qty,
                  std::int64_t trade_ms,
                  bool buyer_is_maker);
// GET /api/v3/depth
void append_depth_snapshot(std::string& out,
                           std::uint64_t last_update_id,
                           std::span<const Level> bids,
                           std::span<const Level> asks);
// GET /api/v3/ticker/bookTicker
void append_rest_book_ticker(std::string& out, std::string_view symbol, Level bid, Level ask);

// ---- reference data -----------------------------------------------------------------------------

struct SymbolInfoView {
  std::string_view symbol;
  std::string_view base_asset;
  std::string_view quote_asset;
  Price tick{};
  Qty lot{};
  Qty min_qty{};
  Qty max_qty{};
  Notional min_notional{};
  Notional max_notional{};
};
struct RateLimitView {
  std::string_view type;      // REQUEST_WEIGHT | ORDERS
  std::string_view interval;  // SECOND | MINUTE | DAY
  int interval_num = 1;
  std::int64_t limit = 0;
  std::int64_t count = -1;  // < 0: omitted (exchangeInfo); >= 0: WS API rateLimits[]
};
void append_rate_limits(std::string& out, std::span<const RateLimitView> limits);
void append_exchange_info(std::string& out,
                          std::int64_t server_ms,
                          std::span<const RateLimitView> limits,
                          std::span<const SymbolInfoView> symbols);

// ---- envelopes ---------------------------------------------------------------------------------

// {"id":<id_json>,"status":S,"result":<body>|"error":<body>[,"rateLimits":<json>]}
void append_ws_api_response(std::string& out,
                            std::string_view id_json,
                            int status,
                            bool is_error,
                            std::string_view body_json,
                            std::string_view rate_limits_json);
// {"stream":"<name>","data":<data_json>}
void append_stream_message(std::string& out, std::string_view stream, std::string_view data_json);
// {"subscriptionId":N,"event":<event_json>}
void append_user_event(std::string& out, std::int64_t subscription_id, std::string_view event_json);

}  // namespace fastmm::sim::server
