#pragma once
// Bybit v5 spot order encoding and response decoding (6.5).
//
// Primary path: WebSocket trade endpoint wss://stream[-testnet].bybit.com/v5/trade
// (https://bybit-exchange.github.io/docs/v5/websocket/trade/guideline), after `op: auth`:
//   {"reqId":"<n|c|r><cl_ord_id>","header":{"X-BAPI-TIMESTAMP":"<ms>","X-BAPI-RECV-WINDOW":"<ms>"},
//    "op":"order.create|order.amend|order.cancel","args":[{...}]}
//   response {"reqId","retCode","retMsg","op","data":{"orderId","orderLinkId"},"retExtInfo",
//             "header":{"X-Bapi-Limit","X-Bapi-Limit-Status","X-Bapi-Limit-Reset-Timestamp",..},
//             "connId"}
// Order args (https://bybit-exchange.github.io/docs/v5/order/create-order): category=spot,
// symbol, side Buy|Sell, orderType Limit|Market, qty, price, timeInForce GTC|IOC|FOK|PostOnly,
// orderLinkId (<= 36 chars; FastMM ids are 14). Amend (.../amend-order): orderId|orderLinkId,
// qty, price. Cancel (.../cancel-order): orderId|orderLinkId.
//
// REST fallback: POST /v5/order/create|amend|cancel|cancel-all with a JSON body, GET
// /v5/order/realtime?category=spot[&symbol=] for reconciliation; signed per bybit_auth.hpp
// over the exact bytes sent.
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/bybit/bybit_auth.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/request_id.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues::bybit {

inline constexpr std::size_t kMaxRequestBytes = 1024;
// Disconnect-Cancel-All: "Disconnection timing window time. [3, 300], unit: second".
inline constexpr int kMinDcpWindowS = 3;
inline constexpr int kMaxDcpWindowS = 300;

// Per working order: what an amend/cancel needs that the engine message does not carry.
// `link_id` is the orderLinkId the venue knows the order by; after an in-place amend the
// engine's id changes but the venue keeps the original orderLinkId.
struct OrderShadow {
  InstrumentId instrument{};
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  ClientOrderId link_id{};
  ClientOrderId replaces{};  // Replace: the order this one amends (venue bookkeeping)
};

struct RestRequest {
  std::string_view method;  // "GET" | "POST"
  std::string_view path;
  std::string query;  // GET: signed payload
  std::string body;   // POST: signed payload
  bool is_order = false;
  [[nodiscard]] std::string target() const {
    return query.empty() ? std::string(path) : std::string(path) + "?" + query;
  }
  [[nodiscard]] std::string_view payload() const noexcept {
    return method == "GET" ? std::string_view(query) : std::string_view(body);
  }
};

class BybitOrderEncoder {
 public:
  BybitOrderEncoder(const Signer& signer,
                    const SymbolTable& symbols,
                    int recv_window_ms = kDefaultRecvWindowMs) noexcept
      : signer_(signer), symbols_(symbols), recv_window_ms_(recv_window_ms) {}

  // ---- WebSocket trade frames (bytes written; 0 = unsupported / overflow) ---------------
  std::size_t encode_ws(const OrderCommand& cmd,
                        const OrderShadow* shadow,  // required for Replace; optional otherwise
                        std::int64_t timestamp_ms,
                        std::span<char> out) const noexcept;
  // {"op":"auth","args":[key, expires, sign]}
  std::size_t encode_ws_auth(std::int64_t expires_ms, std::span<char> out) const noexcept;
  // {"req_id":id,"op":"ping"}
  static std::size_t encode_ping(std::string_view req_id, std::span<char> out) noexcept;
  // {"req_id":id,"op":"subscribe","args":[...]}
  static std::size_t encode_subscribe(std::string_view req_id,
                                      std::span<const std::string_view> topics,
                                      std::span<char> out) noexcept;

  // ---- REST ----------------------------------------------------------------------------
  bool encode_rest(const OrderCommand& cmd, const OrderShadow* shadow, RestRequest& out) const;
  bool encode_rest_cancel_all(std::string_view symbol, RestRequest& out) const;
  // POST /v5/order/disconnected-cancel-all: Bybit's Disconnect-Cancel-All. `product` is
  // SPOT | DERIVATIVES | OPTIONS and `time_window_s` is seconds in [3, 300]. This is not a
  // countdown the client refreshes: the setting persists on the account and Bybit starts the
  // clock itself when every private connection that subscribed a `dcp.*` topic is gone.
  bool encode_rest_set_dcp(std::string_view product, int time_window_s, RestRequest& out) const;
  // `cursor` is result.nextPageCursor of the previous page (empty for the first).
  bool encode_rest_open_orders(std::string_view symbol,
                               std::string_view cursor,
                               RestRequest& out) const;
  // Header block signing exactly out.payload().
  [[nodiscard]] std::string rest_headers(const RestRequest& req, std::int64_t timestamp_ms) const {
    return signer_.rest_headers(timestamp_ms, recv_window_ms_, req.payload(), req.method == "POST");
  }

  [[nodiscard]] static std::string_view side_text(Side s) noexcept {
    return s == Side::Buy ? "Buy" : "Sell";
  }
  [[nodiscard]] static std::string_view tif_text(OrderType type, TimeInForce t) noexcept {
    if (type == OrderType::PostOnly) return "PostOnly";
    switch (t) {
      case TimeInForce::Ioc:
        return "IOC";
      case TimeInForce::Fok:
        return "FOK";
      case TimeInForce::Gtc:
      case TimeInForce::Day:
        return "GTC";
    }
    return "GTC";
  }

 private:
  std::size_t write_args(const OrderCommand& cmd,
                         const OrderShadow* shadow,
                         std::span<char> out) const noexcept;

  const Signer& signer_;
  const SymbolTable& symbols_;
  int recv_window_ms_;
};

// ---- responses -----------------------------------------------------------------------------

struct TradeResponse {
  std::string_view req_id;
  std::string_view op;
  int ret_code = -1;
  std::string_view ret_msg;
  std::string_view order_id;
  std::string_view order_link_id;
  std::int64_t limit = -1;         // header X-Bapi-Limit
  std::int64_t limit_status = -1;  // header X-Bapi-Limit-Status (remaining)
  std::int64_t limit_reset_ms = -1;
  bool is_op_ack = false;  // {"success":..,"op":..} style (auth/ping/subscribe)
  bool success = false;
};

struct RestResponse {
  int ret_code = -1;
  std::string_view ret_msg;
  std::string_view order_id;
  std::string_view order_link_id;
  std::int64_t time_ms = 0;
};

struct OpenOrderRecord {
  std::string_view symbol;
  std::string_view order_id;
  std::string_view order_link_id;
  std::string_view price;
  std::string_view qty;
  std::string_view cum_exec_qty;
  std::string_view side;
  std::string_view status;
};

class BybitResponseDecoder {
 public:
  explicit BybitResponseDecoder(std::size_t capacity = 1U << 20);
  ~BybitResponseDecoder();
  BybitResponseDecoder(const BybitResponseDecoder&) = delete;
  BybitResponseDecoder& operator=(const BybitResponseDecoder&) = delete;

  // Trade/control frame -> TradeResponse; Ignored for topic (stream) frames. Views point
  // into `json` (padded).
  ParseStatus decode_ws(std::string_view json, TradeResponse& out) noexcept;
  ParseStatus decode_rest(std::string_view json, RestResponse& out) noexcept;
  // GET /v5/order/realtime body: result.list[]. Error for a retCode != 0 body (Bybit answers
  // rate limits and clock/signature errors with HTTP 200). `next_cursor` receives
  // result.nextPageCursor, empty after the last page.
  ParseStatus decode_open_orders(std::string_view json,
                                 std::string& next_cursor,
                                 const std::function<void(const OpenOrderRecord&)>& fn) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fastmm::venues::bybit
