#include "fastmm/venues/gemini/gemini_order_encoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

namespace fastmm::venues::gemini {

namespace {

bool all_digits(std::string_view s) noexcept {
  if (s.empty() || s.size() > 20) return false;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

std::string payload_start(std::string_view path, std::int64_t nonce) {
  std::string p = R"({"request":")";
  p += path;
  p += R"(","nonce":)";
  p += std::to_string(nonce);
  return p;
}

RestRequest simple(std::string_view path, std::int64_t nonce) {
  RestRequest r;
  r.path = path;
  r.target = path;
  r.payload = payload_start(path, nonce) + "}";
  return r;
}

}  // namespace

std::size_t GeminiOrderEncoder::encode_ws(const OrderCommand& cmd,
                                          std::string_view venue_order_id,
                                          std::span<char> out) const noexcept {
  JsonWriter w(out);
  switch (cmd.kind) {
    case OrderCommandKind::New: {
      const std::string_view sym = symbols_.lower_symbol(cmd.instrument);
      if (sym.empty()) return 0;
      const RequestId rid = make_request_id(RequestKind::New, cmd.cl_ord_id);
      w.begin_object().key("id").string(rid.view()).key("method").string("order.place");
      w.key("params").begin_object();
      w.key("symbol").string(sym);
      w.key("side").string(side_text(cmd.side));
      const bool market = cmd.type == OrderType::Market;
      w.key("type").string(market ? "MARKET" : "LIMIT");
      w.key("timeInForce").string(tif_text(cmd.type, cmd.tif));
      if (!market) {
        const DecimalText px(cmd.price);
        w.key("price").string(px.view());
      }
      const DecimalText qty(cmd.qty);
      w.key("quantity").string(qty.view());
      w.key("clientOrderId").string(encode_cl_ord_id(cmd.cl_ord_id).view());
      w.end_object().end_object();
      break;
    }
    case OrderCommandKind::Cancel: {
      if (!all_digits(venue_order_id)) return 0;
      const RequestId rid = make_request_id(RequestKind::Cancel, cmd.cl_ord_id);
      w.begin_object().key("id").string(rid.view()).key("method").string("order.cancel");
      // A number is refused (-1013 "Invalid parameters", sandbox 2026-09-30); a string is taken.
      w.key("params").begin_object().key("orderId").string(venue_order_id);
      w.end_object().end_object();
      break;
    }
    case OrderCommandKind::Replace:
      return 0;
  }
  return w.ok() ? w.size() : 0;
}

std::size_t GeminiOrderEncoder::encode_subscribe(std::string_view id,
                                                 std::span<const std::string_view> streams,
                                                 std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object().key("id").string(id).key("method").string("subscribe");
  w.key("params").begin_array();
  for (std::string_view s : streams) w.string(s);
  w.end_array().end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t GeminiOrderEncoder::encode_method(std::string_view id,
                                              std::string_view method,
                                              std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object().key("id").string(id).key("method").string(method).end_object();
  return w.ok() ? w.size() : 0;
}

RestRequest GeminiOrderEncoder::active_orders(std::int64_t nonce) {
  return simple("/v1/orders", nonce);
}

RestRequest GeminiOrderEncoder::positions(std::int64_t nonce) {
  return simple("/v1/positions", nonce);
}

RestRequest GeminiOrderEncoder::balances(std::int64_t nonce) {
  return simple("/v1/balances", nonce);
}

RestRequest GeminiOrderEncoder::margin(std::int64_t nonce, std::string_view symbol) {
  RestRequest r;
  r.path = "/v1/margin";
  r.target = r.path;
  r.payload = payload_start(r.path, nonce);
  r.payload += R"(,"symbol":")";
  r.payload += symbol;
  r.payload += R"("})";
  return r;
}

RestRequest GeminiOrderEncoder::my_trades(std::int64_t nonce,
                                          std::string_view symbol,
                                          std::int64_t since_ms,
                                          int limit) {
  RestRequest r;
  r.path = "/v1/mytrades";
  r.target = r.path;
  r.payload = payload_start(r.path, nonce);
  r.payload += R"(,"symbol":")";
  r.payload += symbol;
  r.payload += R"(","timestamp":)";
  r.payload += std::to_string(since_ms);  // milliseconds, as the spec recommends
  r.payload += R"(,"limit_trades":)";
  r.payload += std::to_string(limit);
  r.payload += '}';
  return r;
}

RestRequest GeminiOrderEncoder::funding_payments(std::int64_t nonce,
                                                 std::int64_t since_ms,
                                                 std::int64_t to_ms) {
  RestRequest r;
  r.path = "/v1/perpetuals/fundingPayment";
  r.target = r.path + "?since=" + std::to_string(since_ms);
  if (to_ms > 0) r.target += "&to=" + std::to_string(to_ms);
  r.payload = payload_start(r.path, nonce) + "}";
  return r;
}

RestRequest GeminiOrderEncoder::cancel_session(std::int64_t nonce) {
  return simple("/v1/order/cancel/session", nonce);
}

RestRequest GeminiOrderEncoder::cancel_order(std::int64_t nonce, std::string_view order_id) {
  RestRequest r;
  r.path = "/v1/order/cancel";
  r.target = r.path;
  r.payload = payload_start(r.path, nonce);
  r.payload += R"(,"order_id":)";
  r.payload += all_digits(order_id) ? std::string(order_id) : std::string("0");
  r.payload += '}';
  return r;
}

RestRequest GeminiOrderEncoder::heartbeat(std::int64_t nonce) {
  return simple("/v1/heartbeat", nonce);
}

}  // namespace fastmm::venues::gemini
