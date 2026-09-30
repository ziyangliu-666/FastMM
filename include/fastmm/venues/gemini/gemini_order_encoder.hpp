#pragma once
// Gemini order encoding: WebSocket requests on the authenticated wss://ws.gemini.com connection
// and the JSON payloads of the private REST requests (gemini_auth.hpp signs them). Sources: the
// AsyncAPI spec https://developer.gemini.com/specs/asyncapi/websocket.yaml (OrderPlaceParams,
// OrderCancelParams), https://developer.gemini.com/websocket/streams.md and the OpenAPI spec
// https://developer.gemini.com/specs/openapi/rest.yaml, read 2026-09-30.
//
//   {"id":"n<cl>","method":"order.place","params":{"symbol","side":BUY|SELL,"type":LIMIT|MARKET,
//     "timeInForce":GTC|IOC|FOK|MOC,"price","quantity","clientOrderId"}}
//     MOC is maker-or-cancel (post-only). No reduce-only and no position side exist; a
//     reduce_only order goes out as a plain one. clientOrderId must match [:\-_\.#a-zA-Z0-9]{1,36}
//     (https://developer.gemini.com/client-order-id.md): FastMM's is 14 alphanumerics.
//   {"id":"c<cl>","method":"order.cancel","params":{"orderId":"<venue id>"}}
//     by the venue's id only: an order is cancelled once its NEW event named it.
//   No amend or replace exists: the connector refuses Replace (caps.supports_replace = false).
//
// REST payloads: {"request":path,"nonce":<n>, ..}, all POST.
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/request_id.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues::gemini {

inline constexpr std::size_t kMaxRequestBytes = 1024;
// mytrades: "Default is 50, max is 500".
inline constexpr int kTradesPageLimit = 500;

struct RestRequest {
  std::string path;     // /v1/...; the payload's `request`
  std::string target;   // path plus query (fundingPayment takes since/to in the query)
  std::string payload;  // JSON, signed in the headers; the body stays empty
};

class GeminiOrderEncoder {
 public:
  explicit GeminiOrderEncoder(const SymbolTable& symbols) noexcept : symbols_(symbols) {}

  // order.place (New) or order.cancel (Cancel, `venue_order_id` = the venue's id, digits).
  // Bytes written; 0 when the command cannot be encoded (Replace, no venue id, overflow).
  std::size_t encode_ws(const OrderCommand& cmd,
                        std::string_view venue_order_id,
                        std::span<char> out) const noexcept;

  // {"id":id,"method":"subscribe","params":[streams..]}
  static std::size_t encode_subscribe(std::string_view id,
                                      std::span<const std::string_view> streams,
                                      std::span<char> out) noexcept;
  // {"id":id,"method":m} (time, ping)
  static std::size_t encode_method(std::string_view id,
                                   std::string_view method,
                                   std::span<char> out) noexcept;

  // ---- REST payloads -------------------------------------------------------------------
  static RestRequest active_orders(std::int64_t nonce);
  static RestRequest positions(std::int64_t nonce);
  // Trades of `symbol` at or after `since_ms`, at most `limit`.
  static RestRequest my_trades(std::int64_t nonce,
                               std::string_view symbol,
                               std::int64_t since_ms,
                               int limit);
  // Funding payments after `since_ms`, until `to_ms` (0: now).
  static RestRequest funding_payments(std::int64_t nonce,
                                      std::int64_t since_ms,
                                      std::int64_t to_ms);
  static RestRequest cancel_session(std::int64_t nonce);
  static RestRequest cancel_order(std::int64_t nonce, std::string_view order_id);
  static RestRequest heartbeat(std::int64_t nonce);

  [[nodiscard]] static std::string_view side_text(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
  }
  [[nodiscard]] static std::string_view tif_text(OrderType type, TimeInForce t) noexcept {
    if (type == OrderType::PostOnly) return "MOC";
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
  const SymbolTable& symbols_;
};

}  // namespace fastmm::venues::gemini
