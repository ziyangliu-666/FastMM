#pragma once
// Binance Spot order encoding + WebSocket API response decoding (6.4).
//
// Primary path: WebSocket API (web-socket-api.md, wss://ws-api.testnet.binance.vision/ws-api/v3)
//   order.place            symbol, side, type (LIMIT | LIMIT_MAKER | MARKET), timeInForce
//                          (GTC/IOC/FOK, LIMIT only), price, quantity, newClientOrderId,
//                          newOrderRespType=ACK, apiKey, recvWindow, timestamp, signature
//   order.cancel           symbol, orderId | origClientOrderId
//   order.cancelReplace    cancelReplaceMode=STOP_ON_FAILURE, cancelOrigClientOrderId |
//                          cancelOrderId, side, type, timeInForce, price, quantity,
//                          newClientOrderId, newOrderRespType=ACK
//   openOrders.cancelAll   symbol
//   openOrders.status      symbol
//   session.logon          apiKey, timestamp, signature (Ed25519 only)
//   userDataStream.subscribe.signature  apiKey, timestamp, signature (any key type)
//   userDataStream.subscribe            (after session.logon)
// Params are emitted in alphabetical order because the signature payload must be sorted
// ("SIGNED request security"); values are strings for decimals and integers for
// timestamp/recvWindow, matching the docs examples. After session.logon apiKey/signature
// are omitted.
//
// REST fallback (rest-api.md): POST /api/v3/order, DELETE /api/v3/order,
// POST /api/v3/order/cancelReplace, DELETE /api/v3/openOrders, GET /api/v3/openOrders with
// every parameter in the query string and X-MBX-APIKEY in the headers.
//
// Request ids: "<kind><cl_ord_id>" with kind n/c/r (request_id.hpp). The WS API id is
// "INT / STRING / null", echoed back verbatim (websocket-api general-api-information), so a
// response maps back to the command with no lookup table.
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/net/url.hpp"
#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/request_id.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace fastmm::venues::binance {

inline constexpr std::size_t kMaxRequestBytes = 1536;
inline constexpr int kDefaultRecvWindowMs = 3000;  // plan 6.4 (docs default 5000, max 60000)

// What the encoder must remember about a working order to build a cancelReplace (the
// engine's OutReplaceMsg carries only ids/price/qty; Binance wants side/type/tif again).
struct OrderShadow {
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  InstrumentId instrument{};
};

struct RestRequest {
  std::string_view method;
  std::string_view path;
  net::QueryBuilder<kMaxRequestBytes> query;  // signed; send as "<path>?<query>"
  std::uint32_t weight = 1;
  bool is_order = false;
};

class BinanceOrderEncoder {
 public:
  BinanceOrderEncoder(const Signer& signer,
                      const SymbolTable& symbols,
                      int recv_window_ms = kDefaultRecvWindowMs) noexcept
      : signer_(signer), symbols_(symbols), recv_window_ms_(recv_window_ms) {}

  // Ed25519 sessions: after session.logon apiKey/signature are omitted per request.
  void set_session_authenticated(bool v) noexcept { session_auth_ = v; }
  [[nodiscard]] bool session_authenticated() const noexcept { return session_auth_; }

  // ---- WebSocket API frames (bytes written; 0 = unsupported command / overflow) ----------
  std::size_t encode_ws(const OrderCommand& cmd,
                        const OrderShadow* shadow,  // required for Replace
                        std::int64_t timestamp_ms,
                        std::span<char> out) noexcept;
  std::size_t encode_ws_cancel_all(std::string_view symbol,
                                   std::string_view request_id,
                                   std::int64_t timestamp_ms,
                                   std::span<char> out) noexcept;
  std::size_t encode_ws_open_orders(std::string_view symbol,
                                    std::string_view request_id,
                                    std::int64_t timestamp_ms,
                                    std::span<char> out) noexcept;
  std::size_t encode_ws_logon(std::string_view request_id,
                              std::int64_t timestamp_ms,
                              std::span<char> out);
  std::size_t encode_ws_user_stream_subscribe(std::string_view request_id,
                                              std::int64_t timestamp_ms,
                                              bool with_signature,
                                              std::span<char> out);
  static std::size_t encode_ws_ping(std::string_view request_id, std::span<char> out) noexcept;

  // ---- REST fallback -----------------------------------------------------------------------
  bool encode_rest(const OrderCommand& cmd,
                   const OrderShadow* shadow,
                   std::int64_t timestamp_ms,
                   RestRequest& out) noexcept;
  bool encode_rest_cancel_all(std::string_view symbol, std::int64_t timestamp_ms, RestRequest& out);
  bool encode_rest_open_orders(std::string_view symbol,
                               std::int64_t timestamp_ms,
                               RestRequest& out);

  // Sorted parameter list for one request; builds both the signature payload and the JSON.
  // Public so the .cpp helpers can build lists; not part of the stable API.
  struct ParamList;

  [[nodiscard]] static std::string_view side_text(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
  }
  [[nodiscard]] static std::string_view type_text(OrderType t) noexcept {
    switch (t) {
      case OrderType::Limit:
        return "LIMIT";
      case OrderType::Market:
        return "MARKET";
      case OrderType::PostOnly:
        return "LIMIT_MAKER";  // rest-api.md: LIMIT_MAKER "is also known as a POST-ONLY order"
    }
    return "LIMIT";
  }
  [[nodiscard]] static std::string_view tif_text(TimeInForce t) noexcept {
    switch (t) {
      case TimeInForce::Gtc:
      case TimeInForce::Day:
        return "GTC";
      case TimeInForce::Ioc:
        return "IOC";
      case TimeInForce::Fok:
        return "FOK";
    }
    return "GTC";
  }

 private:
  std::size_t finish_ws(ParamList& params,
                        std::string_view method,
                        std::string_view request_id,
                        std::span<char> out);
  bool finish_rest(ParamList& params, RestRequest& out);

  const Signer& signer_;
  const SymbolTable& symbols_;
  int recv_window_ms_;
  bool session_auth_ = false;
};

// ---- WebSocket API response ---------------------------------------------------------------

struct RateLimitInfo {
  std::int64_t used_weight = -1;  // REQUEST_WEIGHT count (first entry)
  std::int64_t weight_limit = -1;
  std::int64_t order_count = -1;  // ORDERS count (shortest interval entry)
  std::int64_t order_limit = -1;
};

struct WsApiResponse {
  std::string_view id;
  int status = 0;
  bool is_error = false;
  int code = 0;
  std::string_view msg;
  std::int64_t retry_after_ms = 0;  // error.data.retryAfter (epoch ms) when present
  int new_order_code = 0;           // error.data.newOrderResponse.code (cancelReplace)
  std::string_view new_order_msg;
  // result (order.place / order.cancel / cancelReplace.newOrderResponse)
  std::string_view symbol;
  std::string_view client_order_id;
  std::string_view orig_client_order_id;
  std::int64_t order_id = 0;
  std::string_view order_status;
  std::string_view executed_qty;
  std::string_view transact_time_unused;
  // cancelReplace
  std::string_view cancel_result;
  std::string_view new_order_result;
  std::string_view cancel_client_order_id;
  std::int64_t cancel_order_id = 0;
  std::string_view cancel_executed_qty;
  // session / user stream
  std::int64_t subscription_id = -1;
  bool result_is_array = false;  // openOrders.status / cancelAll
  RateLimitInfo rate;
};

// One open order as returned by openOrders.status / GET /api/v3/openOrders.
struct OpenOrderRecord {
  std::string_view symbol;
  std::int64_t order_id = 0;
  std::string_view client_order_id;
  std::string_view price;
  std::string_view orig_qty;
  std::string_view executed_qty;
  std::string_view status;
  std::string_view side;
  std::string_view type;
  std::string_view tif;
};

class BinanceWsApiDecoder {
 public:
  explicit BinanceWsApiDecoder(std::size_t capacity = 1U << 20);
  ~BinanceWsApiDecoder();
  BinanceWsApiDecoder(const BinanceWsApiDecoder&) = delete;
  BinanceWsApiDecoder& operator=(const BinanceWsApiDecoder&) = delete;

  // Ignored when the frame is not a response (no "id"/"status"). Views point into `json`.
  ParseStatus decode(std::string_view json, WsApiResponse& out) noexcept;
  // Iterates result[] of an openOrders.status response (or a bare REST array when
  // `rest_array` is true). Control path: std::function is fine.
  ParseStatus decode_open_orders(std::string_view json,
                                 bool rest_array,
                                 const std::function<void(const OpenOrderRecord&)>& fn) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fastmm::venues::binance
