#pragma once
// Gate USDT futures order encoding and response decoding.
//
// Primary path: the WebSocket API on the futures stream (futures WS docs, "Futures Account
// Trade", read 2026-10-03). After futures.login on the connection:
//   {"time":T,"channel":"futures.order_place","event":"api","payload":{"req_id":"<n|c|r><cl_ord_id>",
//    "req_param":{"contract":..,"size":<signed contracts>,"price":"..","tif":"gtc|ioc|poc|fok",
//    "text":"t-<cl_ord_id>","reduce_only":true?}}}
//   futures.order_cancel  req_param {"order_id":"<venue id>" | "t-<cl_ord_id>"}
//   futures.order_amend   req_param {"order_id":..,"size":<signed total>,"price":".."}
// The venue answers twice: {"ack":true,...} when it took the request, then the result
// {"request_id","header":{"status","channel","response_time","x_gate_ratelimit_*"},
//  "data":{"result":{order}}} or {"data":{"errs":{"label","message"}}}.
// Sizes are contracts, positive for a bid and negative for an ask; `text` must start with "t-"
// (then at most 28 of [0-9A-Za-z_.-]): "t-" + FastMM's 14-character id.
//
// REST fallback and control (APIv4, prefix /api/v4/futures/{settle}): POST /orders (the same
// object), DELETE /orders/{id}, PUT /orders/{id} {"size","price"}, DELETE /orders?contract=X
// (cancel-all), GET /orders?status=open&contract=X&limit=&offset=, GET /positions?holding=true,
// GET /accounts, GET /my_trades_timerange?contract=&from=&to=&limit=&offset=, GET /fee,
// POST /countdown_cancel_all {"timeout":N}; signed per gate_auth.hpp over the exact bytes sent.
#include "fastmm/core/messages.hpp"
#include "fastmm/venues/feed.hpp"
#include "fastmm/venues/gate/gate_auth.hpp"
#include "fastmm/venues/order_commands.hpp"
#include "fastmm/venues/request_id.hpp"
#include "fastmm/venues/symbology.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::venues::gate {

inline constexpr std::size_t kMaxRequestBytes = 1024;
inline constexpr std::string_view kTextPrefix = "t-";

// Per working order: what a cancel, an amend and the fills need that the engine message does not
// carry. `link_id` is the client id the venue knows the order by (its `text`); after an in-place
// amend the engine's id changes but the venue keeps the order and its text.
struct OrderShadow {
  InstrumentId instrument{};
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  ClientOrderId link_id{};
  ClientOrderId replaces{};    // Replace: the order this one amends (venue bookkeeping)
  std::uint64_t sent_seq = 0;  // SentWatermark send sequence of its New or Replace
  Qty orig_qty{};              // the order's size (after an amend, the new total)
  Qty cum_qty{};               // filled so far, from the usertrades forwarded
  VenueOrderId venue_id{};     // the venue's order id, once a reply named it
  bool acked = false;          // an ack went to the engine (the orders channel repeats "open")
};

struct RestRequest {
  std::string_view method;  // "GET" | "POST" | "PUT" | "DELETE"
  std::string path;         // full path incl. /api/v4
  std::string query;        // without the '?'
  std::string body;
  bool is_order = false;
  [[nodiscard]] std::string target() const { return query.empty() ? path : path + "?" + query; }
};

class GateOrderEncoder {
 public:
  GateOrderEncoder(const Signer& signer,
                   const SymbolTable& symbols,
                   std::string_view settle = "usdt") noexcept
      : signer_(signer), symbols_(symbols), settle_(settle) {}

  [[nodiscard]] std::string_view settle() const noexcept { return settle_; }

  // ---- WebSocket API frames (bytes written; 0 = unsupported / overflow) ------------------
  std::size_t encode_ws(const OrderCommand& cmd,
                        const OrderShadow* shadow,  // required for Replace; optional otherwise
                        std::int64_t time_s,
                        std::span<char> out) const noexcept;
  // futures.login with payload {api_key, signature, timestamp, req_id}.
  std::size_t encode_ws_login(std::int64_t time_s,
                              std::string_view req_id,
                              std::span<char> out) const noexcept;
  // {"time":T,"channel":"futures.ping"}
  static std::size_t encode_ping(std::int64_t time_s, std::span<char> out) noexcept;
  // A subscription with the auth block: {"time":T,"channel":C,"event":"subscribe","payload":[..],
  // "auth":{"method":"api_key","KEY":..,"SIGN":..}}.
  std::size_t encode_subscribe(std::string_view channel,
                               std::span<const std::string_view> payload,
                               std::int64_t time_s,
                               std::span<char> out) const noexcept;

  // ---- REST ----------------------------------------------------------------------------
  bool encode_rest(const OrderCommand& cmd, const OrderShadow* shadow, RestRequest& out) const;
  bool encode_rest_cancel_all(std::string_view contract, RestRequest& out) const;
  void encode_rest_open_orders(std::string_view contract,
                               int limit,
                               int offset,
                               RestRequest& out) const;
  void encode_rest_positions(RestRequest& out) const;
  void encode_rest_accounts(RestRequest& out) const;
  // from / to in Unix seconds (inclusive); `contract` empty for every contract.
  void encode_rest_my_trades(std::string_view contract,
                             std::int64_t from_s,
                             std::int64_t to_s,
                             int limit,
                             int offset,
                             RestRequest& out) const;
  void encode_rest_fee(std::string_view contract, RestRequest& out) const;
  // POST /countdown_cancel_all {"timeout":N[,"contract":C]}: N seconds, 0 cancels the countdown.
  bool encode_rest_countdown(std::int64_t timeout_s,
                             std::string_view contract,
                             RestRequest& out) const;
  // Header block signing exactly the request's method, path, query and body.
  [[nodiscard]] std::string rest_headers(const RestRequest& req, std::int64_t time_s) const {
    return signer_.rest_headers(req.method, req.path, req.query, req.body, time_s);
  }
  [[nodiscard]] std::string prefix() const { return "/api/v4/futures/" + settle_; }

  [[nodiscard]] static std::string_view tif_text(OrderType type, TimeInForce t) noexcept {
    if (type == OrderType::PostOnly) return "poc";
    if (type == OrderType::Market) return "ioc";
    switch (t) {
      case TimeInForce::Ioc:
        return "ioc";
      case TimeInForce::Fok:
        return "fok";
      case TimeInForce::Gtc:
      case TimeInForce::Day:
        return "gtc";
    }
    return "gtc";
  }
  // "t-fm.." of a client order id.
  [[nodiscard]] static FixedString<24> text_of(ClientOrderId id) noexcept {
    FixedString<24> t;
    t.append(kTextPrefix);
    t.append(encode_cl_ord_id(id).view());
    return t;
  }

 private:
  // The req_param object of an order request (also the REST body / the amend body).
  std::size_t write_param(const OrderCommand& cmd,
                          const OrderShadow* shadow,
                          std::span<char> out) const noexcept;
  // The order the venue is to act on: its id when a reply named it, else our text id.
  static std::size_t write_order_ref(const OrderCommand& cmd,
                                     const OrderShadow* shadow,
                                     std::span<char> out) noexcept;

  const Signer& signer_;
  const SymbolTable& symbols_;
  std::string settle_;
};

// ---- responses -----------------------------------------------------------------------------

// A WebSocket API reply or a REST order reply, with the fields of the order object it carries.
struct ApiResponse {
  std::string_view request_id;
  std::string_view channel;
  int status = 0;          // header.status (WS) or the HTTP status (REST)
  bool ack = false;        // {"ack":true}: the venue took the request; the result follows
  bool success = false;    // a result and no errs
  std::string_view label;  // errs.label
  std::string_view message;
  std::string_view uid;  // futures.login result
  // The order object (result of order_place / order_cancel / order_amend, or a REST reply).
  std::string_view order_id;  // result.id as text
  std::string_view text;
  std::string_view order_status;  // open | finished
  std::string_view finish_as;
  Qty size{};  // magnitude
  Qty left{};  // magnitude
  bool bid = true;
  Price price{};
  std::int64_t create_time_ms = 0;
  std::int64_t response_time_ms = 0;
  std::int64_t limit = -1;     // x_gate_ratelimit_limit
  std::int64_t remain = -1;    // x_gate_ratelimit_requests_remain
  std::int64_t reset_ms = -1;  // x_gat_ratelimit_reset_timestamp
};

class GateResponseDecoder {
 public:
  explicit GateResponseDecoder(std::size_t capacity = 1U << 20);
  ~GateResponseDecoder();
  GateResponseDecoder(const GateResponseDecoder&) = delete;
  GateResponseDecoder& operator=(const GateResponseDecoder&) = delete;

  // A frame of the stream: Ok for a WebSocket API reply ({"request_id",...}), Ignored for
  // anything else (channel pushes, subscription acks). Views point into `json` (padded).
  ParseStatus decode_ws(std::string_view json, ApiResponse& out) noexcept;
  // A REST order reply: the order object (2xx) or {"label","message"}.
  ParseStatus decode_rest(std::string_view json, int http_status, ApiResponse& out) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  char id_buf_[24];  // result.id arrives as a JSON number; its text lives here
};

}  // namespace fastmm::venues::gate
