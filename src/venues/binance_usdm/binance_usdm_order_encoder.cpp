#include "fastmm/venues/binance_usdm/binance_usdm_order_encoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <array>
#include <cstring>

namespace fastmm::venues::binance_usdm {

namespace {

using ParamList = BinanceUsdmOrderEncoder::ParamList;

bool finish_rest(ParamList& params, const Signer& signer, RestRequest& out) {
  return binance::write_signed_rest_query(params, signer, out.query);
}

void add_auth(ParamList& p, const Signer& signer, bool ws) noexcept {
  // REST sends the key in the X-MBX-APIKEY header instead; a logged-on WS session sends none.
  if (ws && signer.usable()) p.add("apiKey", signer.api_key());
}

[[nodiscard]] bool have_venue_id(const OrderCommand& cmd) noexcept {
  return cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty();
}

// Alphabetical: apiKey < newClientOrderId < newOrderRespType < price < quantity < recvWindow
// < reduceOnly < side < symbol < timeInForce < timestamp < type
[[gnu::noinline]] void add_new_params(ParamList& p,
                                      const OrderCommand& cmd,
                                      std::string_view symbol,
                                      std::int64_t timestamp_ms,
                                      int recv_window_ms) noexcept {
  const bool market = cmd.type == OrderType::Market;
  p.add("newClientOrderId", encode_cl_ord_id(cmd.cl_ord_id).view());
  p.add("newOrderRespType", "ACK");
  if (!market) p.add_decimal("price", cmd.price);
  p.add_decimal("quantity", cmd.qty);
  p.add_int("recvWindow", recv_window_ms);
  if (cmd.reduce_only) p.add("reduceOnly", "true");
  p.add("side", BinanceUsdmOrderEncoder::side_text(cmd.side));
  p.add("symbol", symbol);
  if (!market) p.add("timeInForce", BinanceUsdmOrderEncoder::tif_text(cmd.type, cmd.tif));
  p.add_int("timestamp", timestamp_ms);
  p.add("type", BinanceUsdmOrderEncoder::type_text(cmd.type));
}

// apiKey < orderId < origClientOrderId < recvWindow < symbol < timestamp
[[gnu::noinline]] void add_cancel_params(ParamList& p,
                                         const OrderCommand& cmd,
                                         const OrderShadow* shadow,
                                         std::string_view symbol,
                                         std::int64_t timestamp_ms,
                                         int recv_window_ms) noexcept {
  if (have_venue_id(cmd)) {
    p.add("orderId", cmd.venue_order_id->view(), true);
  } else {
    // A modified order keeps its first client id on the venue.
    const ClientOrderId link =
        shadow != nullptr && shadow->link_id.valid() ? shadow->link_id : cmd.cl_ord_id;
    p.add("origClientOrderId", encode_cl_ord_id(link).view());
  }
  p.add_int("recvWindow", recv_window_ms);
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
}

// apiKey < orderId < origClientOrderId < price < quantity < recvWindow < side < symbol < timestamp
[[gnu::noinline]] void add_modify_params(ParamList& p,
                                         const OrderCommand& cmd,
                                         const OrderShadow& orig,
                                         std::string_view symbol,
                                         std::int64_t timestamp_ms,
                                         int recv_window_ms) noexcept {
  if (have_venue_id(cmd)) {
    p.add("orderId", cmd.venue_order_id->view(), true);
  } else {
    const ClientOrderId link = orig.link_id.valid() ? orig.link_id : cmd.orig_cl_ord_id;
    p.add("origClientOrderId", encode_cl_ord_id(link).view());
  }
  p.add_decimal("price", cmd.price);
  p.add_decimal("quantity", cmd.qty + orig.cum);
  p.add_int("recvWindow", recv_window_ms);
  p.add("side", BinanceUsdmOrderEncoder::side_text(orig.side));
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
}

bool build_params(ParamList& p,
                  const OrderCommand& cmd,
                  const OrderShadow* shadow,
                  std::string_view symbol,
                  std::int64_t timestamp_ms,
                  int recv_window_ms,
                  const Signer& signer,
                  bool ws) noexcept {
  add_auth(p, signer, ws);
  switch (cmd.kind) {
    case OrderCommandKind::New:
      add_new_params(p, cmd, symbol, timestamp_ms, recv_window_ms);
      return true;
    case OrderCommandKind::Cancel:
      add_cancel_params(p, cmd, shadow, symbol, timestamp_ms, recv_window_ms);
      return true;
    case OrderCommandKind::Replace:
      if (shadow == nullptr) return false;
      add_modify_params(p, cmd, *shadow, symbol, timestamp_ms, recv_window_ms);
      return true;
  }
  return false;
}

}  // namespace

std::size_t BinanceUsdmOrderEncoder::finish_ws(ParamList& params,
                                               std::string_view method,
                                               std::string_view request_id,
                                               std::span<char> out) noexcept {
  return binance::write_signed_ws_request(params, signer_, session_auth_, method, request_id, out);
}

std::size_t BinanceUsdmOrderEncoder::encode_ws_logon(std::string_view request_id,
                                                     std::int64_t timestamp_ms,
                                                     std::span<char> out) noexcept {
  // session.logon is always signed (it is what establishes the session).
  const bool saved = session_auth_;
  session_auth_ = false;
  ParamList p;
  p.add("apiKey", signer_.api_key());
  p.add_int("recvWindow", recv_window_ms_);
  p.add_int("timestamp", timestamp_ms);
  const std::size_t n = finish_ws(p, "session.logon", request_id, out);
  session_auth_ = saved;
  return n;
}

std::size_t BinanceUsdmOrderEncoder::encode_ws(const OrderCommand& cmd,
                                               const OrderShadow* shadow,
                                               std::int64_t timestamp_ms,
                                               std::span<char> out) noexcept {
  const std::string_view symbol = symbols_.venue_symbol(cmd.instrument);
  if (symbol.empty()) return 0;
  ParamList p;
  // A logged-on session sends no apiKey (the `ws` flag only controls it).
  if (!build_params(p, cmd, shadow, symbol, timestamp_ms, recv_window_ms_, signer_, !session_auth_))
    return 0;
  std::string_view method;
  RequestKind kind = RequestKind::New;
  switch (cmd.kind) {
    case OrderCommandKind::New:
      method = "order.place";
      kind = RequestKind::New;
      break;
    case OrderCommandKind::Cancel:
      method = "order.cancel";
      kind = RequestKind::Cancel;
      break;
    case OrderCommandKind::Replace:
      method = "order.modify";
      kind = RequestKind::Replace;
      break;
  }
  const RequestId id = make_request_id(kind, cmd.cl_ord_id);
  return finish_ws(p, method, id.view(), out);
}

bool BinanceUsdmOrderEncoder::encode_rest(const OrderCommand& cmd,
                                          const OrderShadow* shadow,
                                          std::int64_t timestamp_ms,
                                          RestRequest& out) noexcept {
  const std::string_view symbol = symbols_.venue_symbol(cmd.instrument);
  if (symbol.empty()) return false;
  ParamList p;
  if (!build_params(p, cmd, shadow, symbol, timestamp_ms, recv_window_ms_, signer_, false))
    return false;
  out.path = "/fapi/v1/order";
  switch (cmd.kind) {
    case OrderCommandKind::New:
      // "New Order": 1 on the 10 s and 1 min order limits, 0 on the IP limit.
      out.method = "POST";
      out.is_order = true;
      out.weight = 0;
      break;
    case OrderCommandKind::Cancel:
      out.method = "DELETE";  // "Cancel Order": weight 1
      out.is_order = false;
      out.weight = 1;
      break;
    case OrderCommandKind::Replace:
      out.method = "PUT";  // "Modify Order": 1 on the order limits, 0 on the IP limit
      out.is_order = true;
      out.weight = 0;
      break;
  }
  return finish_rest(p, signer_, out);
}

bool BinanceUsdmOrderEncoder::encode_rest_cancel_all(std::string_view symbol,
                                                     std::int64_t timestamp_ms,
                                                     RestRequest& out) {
  ParamList p;
  p.add_int("recvWindow", recv_window_ms_);
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
  out.method = "DELETE";
  out.path = "/fapi/v1/allOpenOrders";
  out.weight = 1;
  out.is_order = false;
  return finish_rest(p, signer_, out);
}

bool BinanceUsdmOrderEncoder::encode_rest_countdown_cancel_all(std::string_view symbol,
                                                               std::int64_t countdown_ms,
                                                               std::int64_t timestamp_ms,
                                                               RestRequest& out) {
  ParamList p;
  p.add_int("countdownTime", countdown_ms);
  p.add_int("recvWindow", recv_window_ms_);
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
  out.method = "POST";
  out.path = "/fapi/v1/countdownCancelAll";
  out.weight = kCountdownCancelAllWeight;
  out.is_order = false;
  return finish_rest(p, signer_, out);
}

bool BinanceUsdmOrderEncoder::encode_rest_open_orders(std::string_view symbol,
                                                      std::int64_t timestamp_ms,
                                                      RestRequest& out) {
  return encode_rest_signed_get(signer_,
                                recv_window_ms_,
                                "/fapi/v1/openOrders",
                                symbol,
                                timestamp_ms,
                                symbol.empty() ? 40 : 1,
                                out);
}

bool BinanceUsdmOrderEncoder::encode_rest_position_risk(std::string_view symbol,
                                                        std::int64_t timestamp_ms,
                                                        RestRequest& out) {
  return encode_rest_signed_get(
      signer_, recv_window_ms_, "/fapi/v3/positionRisk", symbol, timestamp_ms, 5, out);
}

bool BinanceUsdmOrderEncoder::encode_rest_signed_get(const Signer& signer,
                                                     int recv_window_ms,
                                                     std::string_view path,
                                                     std::string_view symbol,
                                                     std::int64_t timestamp_ms,
                                                     std::uint32_t weight,
                                                     RestRequest& out) {
  ParamList p;
  p.add_int("recvWindow", recv_window_ms);
  if (!symbol.empty()) p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
  out.method = "GET";
  out.path = path;
  out.weight = weight;
  out.is_order = false;
  return finish_rest(p, signer, out);
}

}  // namespace fastmm::venues::binance_usdm
