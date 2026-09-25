#include "fastmm/venues/bybit/bybit_order_encoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::bybit {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

// ---- encoder -------------------------------------------------------------------------------

std::size_t BybitOrderEncoder::write_args(const OrderCommand& cmd,
                                          const OrderShadow* shadow,
                                          std::span<char> out) const noexcept {
  const std::string_view symbol = symbols_.venue_symbol(cmd.instrument);
  if (symbol.empty()) return 0;
  JsonWriter w(out);
  w.begin_object().key("category").string("spot").key("symbol").string(symbol);
  const bool have_venue_id = cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty();
  switch (cmd.kind) {
    case OrderCommandKind::New: {
      w.key("side").string(side_text(cmd.side));
      w.key("orderType").string(cmd.type == OrderType::Market ? "Market" : "Limit");
      DecimalText qty(cmd.qty);
      w.key("qty").string(qty.view());
      if (cmd.type == OrderType::Market) {
        // Spot market buys default to a quote-coin quantity (create-order "marketUnit");
        // FastMM quantities are always base units.
        w.key("marketUnit").string("baseCoin");
      } else {
        DecimalText px(cmd.price);
        w.key("price").string(px.view());
        w.key("timeInForce").string(tif_text(cmd.type, cmd.tif));
      }
      w.key("orderLinkId").string(encode_cl_ord_id(cmd.cl_ord_id).view());
      break;
    }
    case OrderCommandKind::Cancel: {
      if (have_venue_id) {
        w.key("orderId").string(cmd.venue_order_id->view());
      } else {
        const ClientOrderId link =
            shadow != nullptr && shadow->link_id.valid() ? shadow->link_id : cmd.cl_ord_id;
        w.key("orderLinkId").string(encode_cl_ord_id(link).view());
      }
      break;
    }
    case OrderCommandKind::Replace: {
      if (shadow == nullptr) return 0;
      if (have_venue_id) {
        w.key("orderId").string(cmd.venue_order_id->view());
      } else {
        const ClientOrderId link = shadow->link_id.valid() ? shadow->link_id : cmd.orig_cl_ord_id;
        w.key("orderLinkId").string(encode_cl_ord_id(link).view());
      }
      // Amend `qty` is "Order quantity after modification" (amend-order page). On the order
      // object `qty` is the order quantity and `leavesQty` "the remaining qty not executed"
      // (open-order page), so the amended value is the total, as the engine's replace qty is.
      // Checked against the docs 2026-09-14; not yet observed on a partially filled order.
      DecimalText qty(cmd.qty);
      DecimalText px(cmd.price);
      w.key("qty").string(qty.view()).key("price").string(px.view());
      break;
    }
  }
  w.end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t BybitOrderEncoder::encode_ws(const OrderCommand& cmd,
                                         const OrderShadow* shadow,
                                         std::int64_t timestamp_ms,
                                         std::span<char> out) const noexcept {
  char args[512];
  const std::size_t n = write_args(cmd, shadow, args);
  if (n == 0) return 0;
  RequestKind kind = RequestKind::New;
  std::string_view op = "order.create";
  if (cmd.kind == OrderCommandKind::Cancel) {
    kind = RequestKind::Cancel;
    op = "order.cancel";
  } else if (cmd.kind == OrderCommandKind::Replace) {
    kind = RequestKind::Replace;
    op = "order.amend";
  }
  const RequestId rid = make_request_id(kind, cmd.cl_ord_id);
  char ts[24];
  char rw[24];
  JsonWriter w(out);
  w.begin_object().key("reqId").string(rid.view());
  w.key("header").begin_object();
  w.key("X-BAPI-TIMESTAMP").string(std::string_view(ts, format_int64(timestamp_ms, ts)));
  w.key("X-BAPI-RECV-WINDOW").string(std::string_view(rw, format_int64(recv_window_ms_, rw)));
  w.end_object();
  w.key("op").string(op).key("args").begin_array().raw_value(std::string_view(args, n)).end_array();
  w.end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t BybitOrderEncoder::encode_ws_auth(std::int64_t expires_ms,
                                              std::span<char> out) const noexcept {
  if (!signer_.usable()) return 0;
  const net::HexSha256 sig = signer_.sign_ws_auth(expires_ms);
  JsonWriter w(out);
  w.begin_object().key("op").string("auth").key("args").begin_array();
  w.string(signer_.api_key()).integer(expires_ms).string(sig.view());
  w.end_array().end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t BybitOrderEncoder::encode_ping(std::string_view req_id, std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object().key("req_id").string(req_id).key("op").string("ping").end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t BybitOrderEncoder::encode_subscribe(std::string_view req_id,
                                                std::span<const std::string_view> topics,
                                                std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object()
      .key("req_id")
      .string(req_id)
      .key("op")
      .string("subscribe")
      .key("args")
      .begin_array();
  for (std::string_view t : topics) w.string(t);
  w.end_array().end_object();
  return w.ok() ? w.size() : 0;
}

bool BybitOrderEncoder::encode_rest(const OrderCommand& cmd,
                                    const OrderShadow* shadow,
                                    RestRequest& out) const {
  char args[512];
  const std::size_t n = write_args(cmd, shadow, args);
  if (n == 0) return false;
  out.method = "POST";
  out.query.clear();
  out.body.assign(args, n);
  switch (cmd.kind) {
    case OrderCommandKind::New:
      out.path = "/v5/order/create";
      out.is_order = true;
      break;
    case OrderCommandKind::Cancel:
      out.path = "/v5/order/cancel";
      out.is_order = false;
      break;
    case OrderCommandKind::Replace:
      out.path = "/v5/order/amend";
      out.is_order = true;
      break;
  }
  return true;
}

bool BybitOrderEncoder::encode_rest_cancel_all(std::string_view symbol, RestRequest& out) const {
  char buf[128];
  JsonWriter w(buf);
  w.begin_object().key("category").string("spot");
  if (!symbol.empty()) w.key("symbol").string(symbol);
  w.end_object();
  if (!w.ok()) return false;
  out.method = "POST";
  out.path = "/v5/order/cancel-all";
  out.query.clear();
  out.body.assign(w.view());
  out.is_order = false;
  return true;
}

bool BybitOrderEncoder::encode_rest_set_dcp(std::string_view product,
                                            int time_window_s,
                                            RestRequest& out) const {
  char buf[96];
  JsonWriter w(buf);
  w.begin_object().key("product").string(product).key("timeWindow").integer(time_window_s);
  w.end_object();
  if (!w.ok()) return false;
  out.method = "POST";
  out.path = "/v5/order/disconnected-cancel-all";
  out.query.clear();
  out.body.assign(w.view());
  out.is_order = false;
  return true;
}

bool BybitOrderEncoder::encode_rest_open_orders(std::string_view symbol,
                                                std::string_view cursor,
                                                RestRequest& out) const {
  out.method = "GET";
  out.path = "/v5/order/realtime";
  out.body.clear();
  out.query = "category=spot";
  if (!symbol.empty()) {
    out.query += "&symbol=";
    out.query += symbol;
  }
  out.query += "&limit=50";
  if (!cursor.empty()) {
    out.query += "&cursor=";
    out.query += cursor;
  }
  out.is_order = false;
  return true;
}

bool BybitOrderEncoder::encode_rest_executions(std::int64_t start_ms,
                                               std::int64_t end_ms,
                                               int limit,
                                               std::string_view cursor,
                                               RestRequest& out) const {
  if (start_ms <= 0) return false;
  out.method = "GET";
  out.path = "/v5/execution/list";
  out.body.clear();
  out.query = "category=spot&startTime=" + std::to_string(start_ms);
  if (end_ms > 0) out.query += "&endTime=" + std::to_string(end_ms);
  out.query += "&limit=" + std::to_string(limit);
  if (!cursor.empty()) {
    out.query += "&cursor=";
    out.query += cursor;
  }
  out.is_order = false;
  return true;
}

// ---- decoder -------------------------------------------------------------------------------

struct BybitResponseDecoder::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

BybitResponseDecoder::BybitResponseDecoder(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}
BybitResponseDecoder::~BybitResponseDecoder() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// The decoders are split into non-inlined functions per response section: large
// functions full of inlined simdjson lookups made gcc's UBSan instrumentation
// (null, alignment, object-size) slow to compile this file at -O1.
[[nodiscard]] [[gnu::noinline]] std::int64_t int_text(od::object& o,
                                                      std::string_view key) noexcept {
  std::string_view s;
  if (o[key].get_string().get(s) != sj::SUCCESS) return -1;
  const auto v = parse_int64(s);
  return v ? *v : -1;
}

// data.orderId / data.orderLinkId and the X-Bapi-Limit headers of a trade response.
[[gnu::noinline]] void read_ws_order_fields(od::object& root, TradeResponse& r) noexcept {
  std::string_view s;
  root.reset();
  {
    od::object data;
    if (root["data"].get_object().get(data) == sj::SUCCESS) {
      if (data["orderId"].get_string().get(s) == sj::SUCCESS) r.order_id = s;
      data.reset();
      if (data["orderLinkId"].get_string().get(s) == sj::SUCCESS) r.order_link_id = s;
    }
  }
  root.reset();
  {
    od::object header;
    if (root["header"].get_object().get(header) == sj::SUCCESS) {
      r.limit = int_text(header, "X-Bapi-Limit");
      header.reset();
      r.limit_status = int_text(header, "X-Bapi-Limit-Status");
      header.reset();
      r.limit_reset_ms = int_text(header, "X-Bapi-Limit-Reset-Timestamp");
    }
  }
}

// False if the order is malformed.
[[gnu::noinline]] bool read_open_order(od::object& o, OpenOrderRecord& rec) noexcept {
  if (o["orderId"].get_string().get(rec.order_id) != sj::SUCCESS) return false;
  if (o["orderLinkId"].get_string().get(rec.order_link_id) != sj::SUCCESS) rec.order_link_id = {};
  o.reset();
  if (o["symbol"].get_string().get(rec.symbol) != sj::SUCCESS) return false;
  o.reset();
  if (o["price"].get_string().get(rec.price) != sj::SUCCESS) return false;
  o.reset();
  if (o["qty"].get_string().get(rec.qty) != sj::SUCCESS) return false;
  o.reset();
  if (o["side"].get_string().get(rec.side) != sj::SUCCESS) return false;
  o.reset();
  if (o["orderStatus"].get_string().get(rec.status) != sj::SUCCESS) return false;
  o.reset();
  if (o["cumExecQty"].get_string().get(rec.cum_exec_qty) != sj::SUCCESS) rec.cum_exec_qty = {};
  return true;
}

// False if the row is malformed. Optional fields are left empty.
[[gnu::noinline]] bool read_execution(od::object& o, ExecutionRecord& rec) noexcept {
  auto req = [&](const char* key, std::string_view& out) {
    o.reset();
    return o[key].get_string().get(out) == sj::SUCCESS;
  };
  auto opt = [&](const char* key, std::string_view& out) {
    o.reset();
    if (o[key].get_string().get(out) != sj::SUCCESS) out = {};
  };
  if (!req("symbol", rec.symbol) || !req("execId", rec.exec_id) ||
      !req("execType", rec.exec_type) || !req("orderId", rec.order_id) || !req("side", rec.side) ||
      !req("execPrice", rec.exec_price) || !req("execQty", rec.exec_qty))
    return false;
  opt("orderLinkId", rec.order_link_id);
  opt("execFee", rec.exec_fee);
  opt("feeCurrency", rec.fee_currency);
  opt("feeRate", rec.fee_rate);
  std::string_view t;
  if (!req("execTime", t)) return false;
  const auto ms = parse_int64(t);
  if (!ms) return false;
  rec.exec_time_ms = *ms;
  o.reset();
  if (o["isMaker"].get_bool().get(rec.is_maker) != sj::SUCCESS) rec.is_maker = false;
  return true;
}

}  // namespace

ParseStatus BybitResponseDecoder::decode_ws(std::string_view json, TradeResponse& r) noexcept {
  r = TradeResponse{};
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  {
    od::value topic;
    if (root["topic"].get(topic) == sj::SUCCESS) return ParseStatus::Ignored;
  }
  root.reset();
  std::string_view s;
  std::int64_t code = 0;
  if (root["reqId"].get_string().get(s) == sj::SUCCESS) r.req_id = s;
  root.reset();
  if (root["retCode"].get_int64().get(code) == sj::SUCCESS) {
    r.ret_code = static_cast<int>(code);
    r.success = code == 0;
  } else {
    root.reset();
    bool ok = false;
    if (root["success"].get_bool().get(ok) != sj::SUCCESS) {
      root.reset();
      if (root["op"].get_string().get(s) != sj::SUCCESS) return ParseStatus::Ignored;
      r.op = s;
      r.is_op_ack = true;
      r.success = s == "pong";
      return ParseStatus::Ok;
    }
    r.is_op_ack = true;
    r.success = ok;
    r.ret_code = ok ? 0 : -1;
    root.reset();
    if (root["req_id"].get_string().get(s) == sj::SUCCESS) r.req_id = s;
    root.reset();
    if (root["ret_msg"].get_string().get(s) == sj::SUCCESS) r.ret_msg = s;
  }
  root.reset();
  if (root["retMsg"].get_string().get(s) == sj::SUCCESS) r.ret_msg = s;
  root.reset();
  if (root["op"].get_string().get(s) == sj::SUCCESS) r.op = s;
  read_ws_order_fields(root, r);
  return ParseStatus::Ok;
}

ParseStatus BybitResponseDecoder::decode_rest(std::string_view json, RestResponse& r) noexcept {
  r = RestResponse{};
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  std::int64_t code = 0;
  if (root["retCode"].get_int64().get(code) != sj::SUCCESS) return ParseStatus::Malformed;
  r.ret_code = static_cast<int>(code);
  std::string_view s;
  if (root["retMsg"].get_string().get(s) == sj::SUCCESS) r.ret_msg = s;
  {
    od::object res;
    if (root["result"].get_object().get(res) == sj::SUCCESS) {
      if (res["orderId"].get_string().get(s) == sj::SUCCESS) r.order_id = s;
      res.reset();
      if (res["orderLinkId"].get_string().get(s) == sj::SUCCESS) r.order_link_id = s;
    }
  }
  std::int64_t t = 0;
  if (root["time"].get_int64().get(t) == sj::SUCCESS) r.time_ms = t;
  return ParseStatus::Ok;
}

ParseStatus BybitResponseDecoder::decode_open_orders(
    std::string_view json,
    std::string& next_cursor,
    const std::function<void(const OpenOrderRecord&)>& fn) noexcept {
  next_cursor.clear();
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  std::int64_t code = 0;
  if (root["retCode"].get_int64().get(code) != sj::SUCCESS || code != 0) return ParseStatus::Error;
  od::object result;
  if (root["result"].get_object().get(result) != sj::SUCCESS) return ParseStatus::Malformed;
  od::array list;
  if (result["list"].get_array().get(list) != sj::SUCCESS) return ParseStatus::Malformed;
  for (auto item : list) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return ParseStatus::Malformed;
    OpenOrderRecord rec;
    if (!read_open_order(o, rec)) return ParseStatus::Malformed;
    fn(rec);
  }
  result.reset();
  std::string_view cursor;
  if (result["nextPageCursor"].get_string().get(cursor) == sj::SUCCESS) next_cursor = cursor;
  return ParseStatus::Ok;
}

ParseStatus BybitResponseDecoder::decode_executions(
    std::string_view json,
    std::string& next_cursor,
    const std::function<void(const ExecutionRecord&)>& fn) noexcept {
  next_cursor.clear();
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  std::int64_t code = 0;
  if (root["retCode"].get_int64().get(code) != sj::SUCCESS || code != 0) return ParseStatus::Error;
  od::object result;
  if (root["result"].get_object().get(result) != sj::SUCCESS) return ParseStatus::Malformed;
  od::array list;
  if (result["list"].get_array().get(list) != sj::SUCCESS) return ParseStatus::Malformed;
  for (auto item : list) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return ParseStatus::Malformed;
    ExecutionRecord rec;
    if (!read_execution(o, rec)) return ParseStatus::Malformed;
    fn(rec);
  }
  result.reset();
  std::string_view cursor;
  if (result["nextPageCursor"].get_string().get(cursor) == sj::SUCCESS) next_cursor = cursor;
  return ParseStatus::Ok;
}

}  // namespace fastmm::venues::bybit
