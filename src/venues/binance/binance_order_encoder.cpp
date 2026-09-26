#include "fastmm/venues/binance/binance_order_encoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <simdjson.h>

#include <array>
#include <cstring>

namespace fastmm::venues::binance {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

// ---- encoder -------------------------------------------------------------------------------

std::size_t BinanceOrderEncoder::finish_ws(ParamList& params,
                                           std::string_view method,
                                           std::string_view request_id,
                                           std::span<char> out) {
  return write_signed_ws_request(params, signer_, session_auth_, method, request_id, out);
}

bool BinanceOrderEncoder::finish_rest(ParamList& params, RestRequest& out) {
  return write_signed_rest_query(params, signer_, out.query);
}

namespace {

void add_auth(BinanceOrderEncoder::ParamList& p, const Signer& signer, bool session_auth) noexcept {
  if (!session_auth && signer.usable()) p.add("apiKey", signer.api_key());
}

// The request builders and the response decoder are split into non-inlined
// functions per request kind and response section: large functions full of
// inlined parameter appends and simdjson lookups made gcc's UBSan
// instrumentation (null, alignment, object-size) take minutes to compile this
// file at -O1.
//
// Shared parameter list for New / Cancel / Replace. Alphabetical order:
//   apiKey < cancelOrderId < cancelOrigClientOrderId < cancelReplaceMode < newClientOrderId
//   < newOrderRespType < orderId < origClientOrderId < price < quantity < recvWindow < side
//   < symbol < timeInForce < timestamp < type
[[gnu::noinline]] void add_new_params(BinanceOrderEncoder::ParamList& p,
                                      const OrderCommand& cmd,
                                      std::string_view symbol,
                                      std::int64_t timestamp_ms,
                                      int recv_window_ms) noexcept {
  p.add("newClientOrderId", encode_cl_ord_id(cmd.cl_ord_id).view());
  p.add("newOrderRespType", "ACK");
  if (cmd.type != OrderType::Market) p.add_decimal("price", cmd.price);
  p.add_decimal("quantity", cmd.qty);
  p.add_int("recvWindow", recv_window_ms);
  p.add("side", BinanceOrderEncoder::side_text(cmd.side));
  p.add("symbol", symbol);
  // timeInForce is mandatory for LIMIT, forbidden for LIMIT_MAKER / MARKET
  // (web-socket-api.md "Place new order" mandatory parameters by type).
  if (cmd.type == OrderType::Limit) p.add("timeInForce", BinanceOrderEncoder::tif_text(cmd.tif));
  p.add_int("timestamp", timestamp_ms);
  p.add("type", BinanceOrderEncoder::type_text(cmd.type));
}

[[gnu::noinline]] void add_cancel_params(BinanceOrderEncoder::ParamList& p,
                                         const OrderCommand& cmd,
                                         std::string_view symbol,
                                         std::int64_t timestamp_ms,
                                         int recv_window_ms) noexcept {
  // orderId alone is fastest (docs note); fall back to the client id before the ack.
  const bool have_venue_id = cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty();
  if (have_venue_id) {
    p.add("orderId", cmd.venue_order_id->view(), true);
  } else {
    p.add("origClientOrderId", encode_cl_ord_id(cmd.cl_ord_id).view());
  }
  p.add_int("recvWindow", recv_window_ms);
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
}

[[gnu::noinline]] void add_replace_params(BinanceOrderEncoder::ParamList& p,
                                          const OrderCommand& cmd,
                                          const OrderShadow& shadow,
                                          std::string_view symbol,
                                          std::int64_t timestamp_ms,
                                          int recv_window_ms) noexcept {
  const bool have_venue_id = cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty();
  if (have_venue_id) {
    p.add("cancelOrderId", cmd.venue_order_id->view(), true);
  } else {
    p.add("cancelOrigClientOrderId", encode_cl_ord_id(cmd.orig_cl_ord_id).view());
  }
  p.add("cancelReplaceMode", "STOP_ON_FAILURE");
  p.add("newClientOrderId", encode_cl_ord_id(cmd.cl_ord_id).view());
  p.add("newOrderRespType", "ACK");
  p.add_decimal("price", cmd.price);
  p.add_decimal("quantity", cmd.qty);
  p.add_int("recvWindow", recv_window_ms);
  p.add("side", BinanceOrderEncoder::side_text(shadow.side));
  p.add("symbol", symbol);
  if (shadow.type == OrderType::Limit)
    p.add("timeInForce", BinanceOrderEncoder::tif_text(shadow.tif));
  p.add_int("timestamp", timestamp_ms);
  p.add("type", BinanceOrderEncoder::type_text(shadow.type));
}

// order.amend.keepPriority / PUT /api/v3/order/amend/keepPriority. Alphabetical order:
//   apiKey < newClientOrderId < newQty < orderId < origClientOrderId < recvWindow < symbol
//   < timestamp
// There is no price parameter: the amendment is a quantity reduction and nothing else, which is
// why the venue can keep the order where it is in the queue. newQty is the new *total* quantity,
// as the engine's replace quantity is.
[[gnu::noinline]] void add_amend_params(BinanceOrderEncoder::ParamList& p,
                                        const OrderCommand& cmd,
                                        std::string_view symbol,
                                        std::int64_t timestamp_ms,
                                        int recv_window_ms) noexcept {
  p.add("newClientOrderId", encode_cl_ord_id(cmd.cl_ord_id).view());
  p.add_decimal("newQty", cmd.qty);
  const bool have_venue_id = cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty();
  if (have_venue_id) {
    p.add("orderId", cmd.venue_order_id->view(), true);
  } else {
    p.add("origClientOrderId", encode_cl_ord_id(cmd.orig_cl_ord_id).view());
  }
  p.add_int("recvWindow", recv_window_ms);
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
}

bool build_params(BinanceOrderEncoder::ParamList& p,
                  const OrderCommand& cmd,
                  const OrderShadow* shadow,
                  std::string_view symbol,
                  std::int64_t timestamp_ms,
                  int recv_window_ms,
                  const Signer& signer,
                  bool session_auth,
                  bool amend_in_place) noexcept {
  add_auth(p, signer, session_auth);
  switch (cmd.kind) {
    case OrderCommandKind::New:
      add_new_params(p, cmd, symbol, timestamp_ms, recv_window_ms);
      return true;
    case OrderCommandKind::Cancel:
      add_cancel_params(p, cmd, symbol, timestamp_ms, recv_window_ms);
      return true;
    case OrderCommandKind::Replace:
      if (shadow == nullptr) return false;
      if (amend_in_place) {
        if (!is_quantity_reduction(cmd, *shadow)) return false;
        add_amend_params(p, cmd, symbol, timestamp_ms, recv_window_ms);
        return true;
      }
      add_replace_params(p, cmd, *shadow, symbol, timestamp_ms, recv_window_ms);
      return true;
  }
  return false;
}

}  // namespace

std::size_t BinanceOrderEncoder::encode_ws(const OrderCommand& cmd,
                                           const OrderShadow* shadow,
                                           std::int64_t timestamp_ms,
                                           std::span<char> out,
                                           bool amend_in_place) noexcept {
  const std::string_view symbol = symbols_.venue_symbol(cmd.instrument);
  if (symbol.empty()) return 0;
  ParamList p;
  if (!build_params(p,
                    cmd,
                    shadow,
                    symbol,
                    timestamp_ms,
                    recv_window_ms_,
                    signer_,
                    session_auth_,
                    amend_in_place))
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
      method = amend_in_place ? "order.amend.keepPriority" : "order.cancelReplace";
      kind = amend_in_place ? RequestKind::Amend : RequestKind::Replace;
      break;
  }
  const RequestId id = make_request_id(kind, cmd.cl_ord_id);
  return finish_ws(p, method, id.view(), out);
}

std::size_t BinanceOrderEncoder::encode_ws_cancel_all(std::string_view symbol,
                                                      std::string_view request_id,
                                                      std::int64_t timestamp_ms,
                                                      std::span<char> out) noexcept {
  ParamList p;
  add_auth(p, signer_, session_auth_);
  p.add_int("recvWindow", recv_window_ms_);
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
  return finish_ws(p, "openOrders.cancelAll", request_id, out);
}

std::size_t BinanceOrderEncoder::encode_ws_open_orders(std::string_view symbol,
                                                       std::string_view request_id,
                                                       std::int64_t timestamp_ms,
                                                       std::span<char> out) noexcept {
  ParamList p;
  add_auth(p, signer_, session_auth_);
  p.add_int("recvWindow", recv_window_ms_);
  if (!symbol.empty()) p.add("symbol", symbol);  // omitted: all symbols (weight 80)
  p.add_int("timestamp", timestamp_ms);
  return finish_ws(p, "openOrders.status", request_id, out);
}

std::size_t BinanceOrderEncoder::encode_ws_logon(std::string_view request_id,
                                                 std::int64_t timestamp_ms,
                                                 std::span<char> out) {
  // session.logon is always signed explicitly (it is what establishes the session).
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

std::size_t BinanceOrderEncoder::encode_ws_user_stream_subscribe(std::string_view request_id,
                                                                 std::int64_t timestamp_ms,
                                                                 bool with_signature,
                                                                 std::span<char> out) {
  if (!with_signature) {
    JsonWriter w(out);
    w.begin_object()
        .key("id")
        .string(request_id)
        .key("method")
        .string("userDataStream.subscribe")
        .end_object();
    return w.ok() ? w.size() : 0;
  }
  const bool saved = session_auth_;
  session_auth_ = false;
  ParamList p;
  p.add("apiKey", signer_.api_key());
  p.add_int("recvWindow", recv_window_ms_);
  p.add_int("timestamp", timestamp_ms);
  const std::size_t n = finish_ws(p, "userDataStream.subscribe.signature", request_id, out);
  session_auth_ = saved;
  return n;
}

std::size_t BinanceOrderEncoder::encode_ws_ping(std::string_view request_id,
                                                std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object().key("id").string(request_id).key("method").string("ping").end_object();
  return w.ok() ? w.size() : 0;
}

bool BinanceOrderEncoder::encode_rest(const OrderCommand& cmd,
                                      const OrderShadow* shadow,
                                      std::int64_t timestamp_ms,
                                      RestRequest& out,
                                      bool amend_in_place) noexcept {
  const std::string_view symbol = symbols_.venue_symbol(cmd.instrument);
  if (symbol.empty()) return false;
  ParamList p;
  // REST never uses a session: apiKey goes in the header, not the params.
  if (!build_params(
          p, cmd, shadow, symbol, timestamp_ms, recv_window_ms_, signer_, true, amend_in_place))
    return false;
  out.weight = 1;
  switch (cmd.kind) {
    case OrderCommandKind::New:
      out.method = "POST";
      out.path = "/api/v3/order";
      out.is_order = true;
      break;
    case OrderCommandKind::Cancel:
      out.method = "DELETE";
      out.path = "/api/v3/order";
      out.is_order = false;
      break;
    case OrderCommandKind::Replace:
      if (amend_in_place) {
        out.method = "PUT";
        out.path = "/api/v3/order/amend/keepPriority";
        out.is_order = false;  // "Unfilled Order Count: 0": an amend adds no order
        out.weight = kAmendWeight;
        break;
      }
      out.method = "POST";
      out.path = "/api/v3/order/cancelReplace";
      out.is_order = true;
      break;
  }
  return finish_rest(p, out);
}

bool BinanceOrderEncoder::encode_rest_cancel_all(std::string_view symbol,
                                                 std::int64_t timestamp_ms,
                                                 RestRequest& out) {
  ParamList p;
  p.add_int("recvWindow", recv_window_ms_);
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
  out.method = "DELETE";
  out.path = "/api/v3/openOrders";
  out.weight = 1;
  return finish_rest(p, out);
}

bool BinanceOrderEncoder::encode_rest_open_orders(std::string_view symbol,
                                                  std::int64_t timestamp_ms,
                                                  RestRequest& out) {
  ParamList p;
  p.add_int("recvWindow", recv_window_ms_);
  if (!symbol.empty()) p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
  out.method = "GET";
  out.path = "/api/v3/openOrders";
  out.weight = symbol.empty() ? 80 : 6;  // rest-api.md "Current open orders" weights
  return finish_rest(p, out);
}

bool BinanceOrderEncoder::encode_rest_my_trades(std::string_view symbol,
                                                std::int64_t from_id,
                                                std::int64_t start_ms,
                                                int limit,
                                                std::int64_t timestamp_ms,
                                                RestRequest& out) {
  if (symbol.empty()) return false;  // rest-api.md: symbol is required
  ParamList p;                       // sorted by name, as the signature payload requires
  if (from_id > 0) p.add_int("fromId", from_id);
  p.add_int("limit", limit);
  p.add_int("recvWindow", recv_window_ms_);
  if (from_id <= 0 && start_ms > 0) p.add_int("startTime", start_ms);
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
  out.method = "GET";
  out.path = "/api/v3/myTrades";
  out.weight = 20;  // rest-api.md "Account trade list"
  return finish_rest(p, out);
}

bool BinanceOrderEncoder::encode_rest_commission(std::string_view symbol,
                                                 std::int64_t timestamp_ms,
                                                 RestRequest& out) {
  if (symbol.empty()) return false;
  ParamList p;  // the endpoint lists symbol only; timestamp as every signed request
  p.add("symbol", symbol);
  p.add_int("timestamp", timestamp_ms);
  out.method = "GET";
  out.path = "/api/v3/account/commission";
  out.weight = 20;  // rest-api.md "Query Commission Rates"
  return finish_rest(p, out);
}

// ---- response decoder ----------------------------------------------------------------------

struct BinanceWsApiDecoder::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

BinanceWsApiDecoder::BinanceWsApiDecoder(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}
BinanceWsApiDecoder::~BinanceWsApiDecoder() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

[[gnu::noinline]] void read_order_fields(od::object& o, WsApiResponse& r) noexcept {
  std::string_view s;
  std::int64_t i = 0;
  if (o["symbol"].get_string().get(s) == sj::SUCCESS) r.symbol = s;
  if (o["orderId"].get_int64().get(i) == sj::SUCCESS) r.order_id = i;
  if (o["clientOrderId"].get_string().get(s) == sj::SUCCESS) r.client_order_id = s;
  if (o["origClientOrderId"].get_string().get(s) == sj::SUCCESS) r.orig_client_order_id = s;
  if (o["status"].get_string().get(s) == sj::SUCCESS) r.order_status = s;
  if (o["executedQty"].get_string().get(s) == sj::SUCCESS) r.executed_qty = s;
}

// cancelReplace's cancelResponse, in a result or in error.data.
[[gnu::noinline]] void read_cancel_response(od::object& cr, WsApiResponse& r) noexcept {
  std::string_view s;
  std::int64_t i = 0;
  if (cr["orderId"].get_int64().get(i) == sj::SUCCESS) r.cancel_order_id = i;
  if (cr["origClientOrderId"].get_string().get(s) == sj::SUCCESS) r.cancel_client_order_id = s;
  if (cr["executedQty"].get_string().get(s) == sj::SUCCESS) r.cancel_executed_qty = s;
}

[[gnu::noinline]] void read_error_data(od::object& dobj, WsApiResponse& r) noexcept {
  std::int64_t ra = 0;
  if (dobj["retryAfter"].get_int64().get(ra) == sj::SUCCESS) r.retry_after_ms = ra;
  // cancelReplace failures (-2021/-2022, HTTP 409) report both legs here.
  std::string_view s;
  if (dobj["cancelResult"].get_string().get(s) == sj::SUCCESS) r.cancel_result = s;
  if (dobj["newOrderResult"].get_string().get(s) == sj::SUCCESS) r.new_order_result = s;
  od::object cr;
  if (dobj["cancelResponse"].get_object().get(cr) == sj::SUCCESS) read_cancel_response(cr, r);
  od::object nr;
  if (dobj["newOrderResponse"].get_object().get(nr) == sj::SUCCESS) {
    std::int64_t nr_code = 0;
    if (nr["code"].get_int64().get(nr_code) == sj::SUCCESS)
      r.new_order_code = static_cast<int>(nr_code);
    if (nr["msg"].get_string().get(s) == sj::SUCCESS) r.new_order_msg = s;
    nr.reset();
    read_order_fields(nr, r);
  }
}

[[gnu::noinline]] void read_error(od::value err, WsApiResponse& r) noexcept {
  od::object eo;
  if (err.get_object().get(eo) != sj::SUCCESS) return;
  r.is_error = true;
  std::int64_t code = 0;
  if (eo["code"].get_int64().get(code) == sj::SUCCESS) r.code = static_cast<int>(code);
  std::string_view msg;
  if (eo["msg"].get_string().get(msg) == sj::SUCCESS) r.msg = msg;
  od::value data;
  if (eo["data"].get(data) == sj::SUCCESS) {
    od::object dobj;
    if (data.get_object().get(dobj) == sj::SUCCESS) read_error_data(dobj, r);
  }
}

[[gnu::noinline]] void read_result(od::value res, WsApiResponse& r) noexcept {
  od::json_type t;
  if (res.type().get(t) != sj::SUCCESS) return;
  if (t == od::json_type::array) {
    r.result_is_array = true;
    return;
  }
  if (t != od::json_type::object) return;
  od::object o;
  if (res.get_object().get(o) != sj::SUCCESS) return;
  std::string_view s;
  std::int64_t i = 0;
  if (o["cancelResult"].get_string().get(s) == sj::SUCCESS) {
    r.cancel_result = s;
    if (o["newOrderResult"].get_string().get(s) == sj::SUCCESS) r.new_order_result = s;
    od::object cr;
    if (o["cancelResponse"].get_object().get(cr) == sj::SUCCESS) read_cancel_response(cr, r);
    od::object nr;
    if (o["newOrderResponse"].get_object().get(nr) == sj::SUCCESS) read_order_fields(nr, r);
  } else {
    o.reset();
    od::object amended;
    if (o["amendedOrder"].get_object().get(amended) == sj::SUCCESS) {
      // order.amend.keepPriority: {"transactTime","executionId","amendedOrder":{...}}. The
      // amended order keeps the orderId and reports origClientOrderId + the new clientOrderId.
      r.amended = true;
      read_order_fields(amended, r);
      return;
    }
    o.reset();
    if (o["subscriptionId"].get_int64().get(i) == sj::SUCCESS) {
      r.subscription_id = i;
    } else {
      o.reset();
      read_order_fields(o, r);
    }
  }
}

[[gnu::noinline]] void read_rate_limits(od::value v, RateLimitInfo& info) noexcept {
  od::array arr;
  if (v.get_array().get(arr) != sj::SUCCESS) return;
  std::int64_t best_order_window = -1;
  for (auto item : arr) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return;
    std::string_view type;
    std::string_view interval;
    std::int64_t num = 0;
    std::int64_t limit = 0;
    std::int64_t count = 0;
    if (o["rateLimitType"].get_string().get(type) != sj::SUCCESS) continue;
    if (o["interval"].get_string().get(interval) != sj::SUCCESS) continue;
    if (o["intervalNum"].get_int64().get(num) != sj::SUCCESS) continue;
    if (o["limit"].get_int64().get(limit) != sj::SUCCESS) continue;
    if (o["count"].get_int64().get(count) != sj::SUCCESS) continue;
    std::int64_t window_s = num;
    if (interval == "MINUTE") {
      window_s *= 60;
    } else if (interval == "HOUR") {
      window_s *= 3600;
    } else if (interval == "DAY") {
      window_s *= 86400;
    }
    if (type == "REQUEST_WEIGHT" && info.used_weight < 0) {
      info.used_weight = count;
      info.weight_limit = limit;
    } else if (type == "ORDERS" && (best_order_window < 0 || window_s < best_order_window)) {
      best_order_window = window_s;
      info.order_count = count;
      info.order_limit = limit;
    }
  }
}

// False if the trade is malformed. Spot (myTrades) says `isBuyer`/`isMaker`; USDⓈ-M
// (userTrades) says `buyer`/`maker` and also `side`, and has no symbol-less variant either.
[[gnu::noinline]] bool read_my_trade(od::object& o, MyTradeRecord& rec, bool futures) noexcept {
  if (o["symbol"].get_string().get(rec.symbol) != sj::SUCCESS) return false;
  if (o["id"].get_int64().get(rec.id) != sj::SUCCESS) return false;
  if (o["orderId"].get_int64().get(rec.order_id) != sj::SUCCESS) return false;
  if (o["price"].get_string().get(rec.price) != sj::SUCCESS) return false;
  if (o["qty"].get_string().get(rec.qty) != sj::SUCCESS) return false;
  if (o["commission"].get_string().get(rec.commission) != sj::SUCCESS) rec.commission = {};
  if (o["commissionAsset"].get_string().get(rec.commission_asset) != sj::SUCCESS)
    rec.commission_asset = {};
  if (o["time"].get_int64().get(rec.time_ms) != sj::SUCCESS) return false;
  if (o[futures ? "buyer" : "isBuyer"].get_bool().get(rec.is_buyer) != sj::SUCCESS) return false;
  if (o[futures ? "maker" : "isMaker"].get_bool().get(rec.is_maker) != sj::SUCCESS)
    rec.is_maker = false;
  return true;
}

// False if the order is malformed.
[[gnu::noinline]] bool read_open_order(od::object& o, OpenOrderRecord& rec) noexcept {
  if (o["symbol"].get_string().get(rec.symbol) != sj::SUCCESS) return false;
  if (o["orderId"].get_int64().get(rec.order_id) != sj::SUCCESS) return false;
  if (o["clientOrderId"].get_string().get(rec.client_order_id) != sj::SUCCESS) return false;
  if (o["price"].get_string().get(rec.price) != sj::SUCCESS) return false;
  if (o["origQty"].get_string().get(rec.orig_qty) != sj::SUCCESS) return false;
  if (o["executedQty"].get_string().get(rec.executed_qty) != sj::SUCCESS) return false;
  if (o["status"].get_string().get(rec.status) != sj::SUCCESS) return false;
  if (o["timeInForce"].get_string().get(rec.tif) != sj::SUCCESS) rec.tif = {};
  if (o["type"].get_string().get(rec.type) != sj::SUCCESS) return false;
  if (o["side"].get_string().get(rec.side) != sj::SUCCESS) return false;
  return true;
}

}  // namespace

ParseStatus BinanceWsApiDecoder::decode(std::string_view json, WsApiResponse& r) noexcept {
  r = WsApiResponse{};
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  {
    od::value idv;
    if (root["id"].get(idv) != sj::SUCCESS) return ParseStatus::Ignored;
    std::string_view ids;
    if (idv.get_string().get(ids) == sj::SUCCESS) {
      r.id = ids;
    } else {
      r.id = {};  // numeric/null ids are not ours
    }
  }
  std::int64_t status = 0;
  if (root["status"].get_int64().get(status) != sj::SUCCESS) return ParseStatus::Ignored;
  r.status = static_cast<int>(status);
  {
    od::value err;
    if (root["error"].get(err) == sj::SUCCESS) read_error(err, r);
  }
  {
    od::value res;
    if (root["result"].get(res) == sj::SUCCESS) read_result(res, r);
  }
  {
    od::value rl;
    if (root["rateLimits"].get(rl) == sj::SUCCESS) read_rate_limits(rl, r.rate);
  }
  return ParseStatus::Ok;
}

ParseStatus BinanceWsApiDecoder::decode_open_orders(
    std::string_view json,
    bool rest_array,
    const std::function<void(const OpenOrderRecord&)>& fn) noexcept {
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return ParseStatus::Malformed;
  od::array arr;
  if (rest_array) {
    if (doc.get_array().get(arr) != sj::SUCCESS) return ParseStatus::Malformed;
  } else {
    od::object root;
    if (doc.get_object().get(root) != sj::SUCCESS) return ParseStatus::Malformed;
    if (root["result"].get_array().get(arr) != sj::SUCCESS) return ParseStatus::Malformed;
  }
  for (auto item : arr) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return ParseStatus::Malformed;
    OpenOrderRecord rec;
    if (!read_open_order(o, rec)) return ParseStatus::Malformed;
    fn(rec);
  }
  return ParseStatus::Ok;
}

ParseStatus BinanceWsApiDecoder::decode_my_trades(
    std::string_view json, const std::function<void(const MyTradeRecord&)>& fn) noexcept {
  return decode_trades(json, /*futures=*/false, fn);
}

ParseStatus BinanceWsApiDecoder::decode_user_trades(
    std::string_view json, const std::function<void(const MyTradeRecord&)>& fn) noexcept {
  return decode_trades(json, /*futures=*/true, fn);
}

ParseStatus BinanceWsApiDecoder::decode_trades(
    std::string_view json,
    bool futures,
    const std::function<void(const MyTradeRecord&)>& fn) noexcept {
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return ParseStatus::Malformed;
  od::array arr;
  if (doc.get_array().get(arr) != sj::SUCCESS) return ParseStatus::Malformed;
  for (auto item : arr) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return ParseStatus::Malformed;
    MyTradeRecord rec;
    if (!read_my_trade(o, rec, futures)) return ParseStatus::Malformed;
    fn(rec);
  }
  return ParseStatus::Ok;
}

}  // namespace fastmm::venues::binance
