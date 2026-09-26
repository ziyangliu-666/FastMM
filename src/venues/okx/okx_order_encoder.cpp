#include "fastmm/venues/okx/okx_order_encoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"
#include "fastmm/venues/okx/okx_error_map.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::okx {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

// ---- encoder -------------------------------------------------------------------------------

std::size_t OkxOrderEncoder::write_args(const OrderCommand& cmd,
                                        const OrderShadow* shadow,
                                        bool ws,
                                        std::span<char> out) const noexcept {
  JsonWriter w(out);
  w.begin_object();
  if (ws) {
    const std::int64_t code = inst_id_code(cmd.instrument);
    if (code < 0) return 0;
    w.key("instIdCode").integer(code);
  } else {
    const std::string_view inst = symbols_.venue_symbol(cmd.instrument);
    if (inst.empty()) return 0;
    w.key("instId").string(inst);
  }
  const bool have_venue_id = cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty();
  switch (cmd.kind) {
    case OrderCommandKind::New: {
      w.key("tdMode").string(to_string(td_mode_));
      w.key("clOrdId").string(encode_cl_ord_id(cmd.cl_ord_id).view());
      w.key("side").string(side_text(cmd.side));
      w.key("ordType").string(ord_type_text(cmd.type, cmd.tif));
      if (cmd.type != OrderType::Market) {
        DecimalText px(cmd.price);
        w.key("px").string(px.view());
      }
      DecimalText sz(cmd.qty);
      w.key("sz").string(sz.view());
      if (cmd.reduce_only) w.key("reduceOnly").boolean(true);
      break;
    }
    case OrderCommandKind::Cancel: {
      if (have_venue_id) {
        w.key("ordId").string(cmd.venue_order_id->view());
      } else {
        const ClientOrderId link =
            shadow != nullptr && shadow->link_id.valid() ? shadow->link_id : cmd.cl_ord_id;
        w.key("clOrdId").string(encode_cl_ord_id(link).view());
      }
      break;
    }
    case OrderCommandKind::Replace: {
      if (shadow == nullptr) return 0;
      if (have_venue_id) {
        w.key("ordId").string(cmd.venue_order_id->view());
      } else {
        const ClientOrderId link = shadow->link_id.valid() ? shadow->link_id : cmd.orig_cl_ord_id;
        w.key("clOrdId").string(encode_cl_ord_id(link).view());
      }
      // The orders channel reports the amend's outcome under this id (amendResult, reqId).
      w.key("reqId").string(encode_cl_ord_id(cmd.cl_ord_id).view());
      DecimalText sz(cmd.qty);
      DecimalText px(cmd.price);
      w.key("newSz").string(sz.view()).key("newPx").string(px.view());
      break;
    }
  }
  w.end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t OkxOrderEncoder::encode_ws(const OrderCommand& cmd,
                                       const OrderShadow* shadow,
                                       std::span<char> out) const noexcept {
  char args[512];
  const std::size_t n = write_args(cmd, shadow, true, args);
  if (n == 0) return 0;
  RequestKind kind = RequestKind::New;
  std::string_view op = "order";
  if (cmd.kind == OrderCommandKind::Cancel) {
    kind = RequestKind::Cancel;
    op = "cancel-order";
  } else if (cmd.kind == OrderCommandKind::Replace) {
    kind = RequestKind::Replace;
    op = "amend-order";
  }
  const RequestId rid = make_request_id(kind, cmd.cl_ord_id);
  JsonWriter w(out);
  w.begin_object().key("id").string(rid.view()).key("op").string(op);
  w.key("args").begin_array().raw_value(std::string_view(args, n)).end_array();
  w.end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t OkxOrderEncoder::encode_login(const Signer& signer,
                                          std::int64_t unix_s,
                                          std::span<char> out) noexcept {
  if (!signer.usable()) return 0;
  const Base64Sha256 sig = signer.sign_login(unix_s);
  char ts[24];
  const std::size_t tn = format_int64(unix_s, ts);
  JsonWriter w(out);
  w.begin_object().key("op").string("login").key("args").begin_array().begin_object();
  w.key("apiKey").string(signer.api_key());
  w.key("passphrase").string(signer.passphrase());
  w.key("timestamp").string(std::string_view(ts, tn));
  w.key("sign").string(sig.view());
  w.end_object().end_array().end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t OkxOrderEncoder::encode_private_subscribe(std::string_view id,
                                                      std::span<const std::string_view> channels,
                                                      std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object().key("id").string(id).key("op").string("subscribe").key("args").begin_array();
  for (std::string_view ch : channels) {
    w.begin_object().key("channel").string(ch);
    // balance_and_position takes no instType; the positions channel is asked for events only,
    // "updateInterval": "0" (extraParams is a JSON string).
    if (ch != "balance_and_position") w.key("instType").string("SWAP");
    if (ch == "positions") w.key("extraParams").string(R"({"updateInterval":"0"})");
    w.end_object();
  }
  w.end_array().end_object();
  return w.ok() ? w.size() : 0;
}

bool OkxOrderEncoder::encode_rest(const OrderCommand& cmd,
                                  const OrderShadow* shadow,
                                  RestRequest& out) const {
  char args[512];
  const std::size_t n = write_args(cmd, shadow, false, args);
  if (n == 0) return false;
  out.method = "POST";
  out.body.assign(args, n);
  switch (cmd.kind) {
    case OrderCommandKind::New:
      out.path = "/api/v5/trade/order";
      out.is_order = true;
      break;
    case OrderCommandKind::Cancel:
      out.path = "/api/v5/trade/cancel-order";
      out.is_order = false;
      break;
    case OrderCommandKind::Replace:
      out.path = "/api/v5/trade/amend-order";
      out.is_order = true;
      break;
  }
  return true;
}

bool OkxOrderEncoder::encode_rest_cancel_batch(std::span<const CancelEntry> orders,
                                               RestRequest& out) {
  if (orders.empty() || orders.size() > kMaxBatch) return false;
  out.method = "POST";
  out.path = "/api/v5/trade/cancel-batch-orders";
  out.body = "[";
  for (std::size_t i = 0; i < orders.size(); ++i) {
    char buf[160];
    JsonWriter w(buf);
    w.begin_object().key("instId").string(orders[i].inst_id);
    w.key("ordId").string(orders[i].ord_id).end_object();
    if (!w.ok()) return false;
    if (i != 0) out.body += ',';
    out.body += w.view();
  }
  out.body += ']';
  out.is_order = false;
  return true;
}

bool OkxOrderEncoder::encode_rest_cancel_all_after(int timeout_s, RestRequest& out) {
  if (timeout_s != 0 && (timeout_s < kMinCancelAfterS || timeout_s > kMaxCancelAfterS))
    return false;
  out.method = "POST";
  out.path = "/api/v5/trade/cancel-all-after";
  out.body = R"({"timeOut":")" + std::to_string(timeout_s) + R"("})";
  out.is_order = false;
  return true;
}

void OkxOrderEncoder::encode_rest_orders_pending(std::string_view after, RestRequest& out) {
  out.method = "GET";
  out.path = "/api/v5/trade/orders-pending?instType=SWAP&limit=100";
  if (!after.empty()) {
    out.path += "&after=";
    out.path += after;
  }
  out.body.clear();
  out.is_order = false;
}

void OkxOrderEncoder::encode_rest_fills(bool history,
                                        std::int64_t begin_ms,
                                        std::int64_t end_ms,
                                        std::string_view after,
                                        int limit,
                                        RestRequest& out) {
  out.method = "GET";
  out.path =
      history ? "/api/v5/trade/fills-history?instType=SWAP" : "/api/v5/trade/fills?instType=SWAP";
  if (!after.empty()) {
    out.path += "&after=";
    out.path += after;
  }
  if (begin_ms > 0) out.path += "&begin=" + std::to_string(begin_ms);
  if (end_ms > 0) out.path += "&end=" + std::to_string(end_ms);
  out.path += "&limit=" + std::to_string(limit);
  out.body.clear();
  out.is_order = false;
}

void OkxOrderEncoder::encode_rest_funding_bills(
    bool archive, std::int64_t begin_ms, std::string_view after, int limit, RestRequest& out) {
  out.method = "GET";
  out.path = archive ? "/api/v5/account/bills-archive?instType=SWAP&type=8"
                     : "/api/v5/account/bills?instType=SWAP&type=8";
  if (!after.empty()) {
    out.path += "&after=";
    out.path += after;
  }
  if (begin_ms > 0) out.path += "&begin=" + std::to_string(begin_ms);
  out.path += "&limit=" + std::to_string(limit);
  out.body.clear();
  out.is_order = false;
}

void OkxOrderEncoder::encode_rest_positions(RestRequest& out) {
  out.method = "GET";
  out.path = "/api/v5/account/positions?instType=SWAP";
  out.body.clear();
  out.is_order = false;
}

void OkxOrderEncoder::encode_rest_account_config(RestRequest& out) {
  out.method = "GET";
  out.path = "/api/v5/account/config";
  out.body.clear();
  out.is_order = false;
}

// ---- decoder -------------------------------------------------------------------------------

struct OkxResponseDecoder::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

OkxResponseDecoder::OkxResponseDecoder(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}
OkxResponseDecoder::~OkxResponseDecoder() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// data[0] of a reply.
[[gnu::noinline]] bool read_first_order(od::value data_val, TradeResponse& r) noexcept {
  od::array data;
  if (data_val.get_array().get(data) != sj::SUCCESS) return false;
  for (auto item : data) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return false;
    for (auto field : o) {
      std::string_view key;
      std::string_view v;
      if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
      if (key != "ordId" && key != "clOrdId" && key != "reqId" && key != "sCode" && key != "sMsg")
        continue;
      if (field.value().get_string().get(v) != sj::SUCCESS) return false;
      if (key == "ordId") r.ord_id = v;
      if (key == "clOrdId") r.cl_ord_id = v;
      if (key == "reqId") r.req_id = v;
      if (key == "sCode") r.s_code = parse_code(v);
      if (key == "sMsg") r.s_msg = v;
    }
    break;  // one order per request
  }
  return true;
}

}  // namespace

ParseStatus OkxResponseDecoder::decode(std::string_view json, TradeResponse& r) noexcept {
  r = TradeResponse{};
  if (json == "pong") return ParseStatus::Ignored;
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  bool have_data = false;
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return ParseStatus::Malformed;
    std::string_view v;
    if (key == "data") {
      if (!read_first_order(field.value(), r)) return ParseStatus::Malformed;
      have_data = true;
      continue;
    }
    if (key != "id" && key != "op" && key != "event" && key != "code" && key != "msg") continue;
    if (field.value().get_string().get(v) != sj::SUCCESS) return ParseStatus::Malformed;
    if (key == "id") r.id = v;
    if (key == "op") r.op = v;
    if (key == "event") r.event = v;
    if (key == "code") r.code = parse_code(v);
    if (key == "msg") r.msg = v;
  }
  if (r.code < 0) return ParseStatus::Malformed;
  // A request-level failure has no order in `data`: its code is the order's.
  if (!have_data || r.s_code < 0) r.s_code = r.code;
  return ParseStatus::Ok;
}

}  // namespace fastmm::venues::okx
