#include "fastmm/venues/gate/gate_order_encoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/gate/gate_rest_decoder.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <simdjson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fastmm::venues::gate {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

// ---- encoder -------------------------------------------------------------------------------

namespace {

// A signed contract count as a JSON number token: "-3", "1.5".
std::size_t signed_size(Side side, Qty qty, std::span<char> out) noexcept {
  DecimalText q(qty);
  std::size_t n = 0;
  if (side == Side::Sell) {
    if (out.empty()) return 0;
    out[n++] = '-';
  }
  if (n + q.size() > out.size()) return 0;
  std::memcpy(out.data() + n, q.view().data(), q.size());
  return n + q.size();
}

}  // namespace

std::size_t GateOrderEncoder::write_order_ref(const OrderCommand& cmd,
                                              const OrderShadow* shadow,
                                              std::span<char> out) noexcept {
  // The venue's id when a reply named it, else the text id the order was placed with.
  if (cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty()) {
    const std::string_view v = cmd.venue_order_id->view();
    if (v.size() > out.size()) return 0;
    std::memcpy(out.data(), v.data(), v.size());
    return v.size();
  }
  if (shadow != nullptr && !shadow->venue_id.empty()) {
    const std::string_view v = shadow->venue_id.view();
    if (v.size() > out.size()) return 0;
    std::memcpy(out.data(), v.data(), v.size());
    return v.size();
  }
  ClientOrderId link = cmd.kind == OrderCommandKind::Replace ? cmd.orig_cl_ord_id : cmd.cl_ord_id;
  if (shadow != nullptr && shadow->link_id.valid()) link = shadow->link_id;
  const FixedString<24> t = text_of(link);
  if (t.size() > out.size()) return 0;
  std::memcpy(out.data(), t.view().data(), t.size());
  return t.size();
}

std::size_t GateOrderEncoder::write_param(const OrderCommand& cmd,
                                          const OrderShadow* shadow,
                                          std::span<char> out) const noexcept {
  JsonWriter w(out);
  char num[32];
  char ref[48];
  switch (cmd.kind) {
    case OrderCommandKind::New: {
      const std::string_view contract = symbols_.venue_symbol(cmd.instrument);
      if (contract.empty()) return 0;
      w.begin_object().key("contract").string(contract);
      const std::size_t n = signed_size(cmd.side, cmd.qty, num);
      if (n == 0) return 0;
      w.key("size").raw_value(std::string_view(num, n));
      if (cmd.type == OrderType::Market) {
        w.key("price").string("0");  // "0 means market order, with tif set to ioc"
      } else {
        DecimalText px(cmd.price);
        w.key("price").string(px.view());
      }
      w.key("tif").string(tif_text(cmd.type, cmd.tif));
      w.key("text").string(text_of(cmd.cl_ord_id).view());
      if (cmd.reduce_only) w.key("reduce_only").boolean(true);
      w.end_object();
      break;
    }
    case OrderCommandKind::Cancel: {
      const std::size_t n = write_order_ref(cmd, shadow, ref);
      if (n == 0) return 0;
      w.begin_object().key("order_id").string(std::string_view(ref, n)).end_object();
      break;
    }
    case OrderCommandKind::Replace: {
      if (shadow == nullptr) return 0;
      const std::size_t n = write_order_ref(cmd, shadow, ref);
      if (n == 0) return 0;
      // "size: New order size, including filled part", signed like the order's.
      const std::size_t sn = signed_size(shadow->side, cmd.qty, num);
      if (sn == 0) return 0;
      DecimalText px(cmd.price);
      w.begin_object().key("order_id").string(std::string_view(ref, n));
      w.key("size").raw_value(std::string_view(num, sn)).key("price").string(px.view());
      w.end_object();
      break;
    }
  }
  return w.ok() ? w.size() : 0;
}

std::size_t GateOrderEncoder::encode_ws(const OrderCommand& cmd,
                                        const OrderShadow* shadow,
                                        std::int64_t time_s,
                                        std::span<char> out) const noexcept {
  char param[512];
  const std::size_t n = write_param(cmd, shadow, param);
  if (n == 0) return 0;
  RequestKind kind = RequestKind::New;
  std::string_view channel = "futures.order_place";
  if (cmd.kind == OrderCommandKind::Cancel) {
    kind = RequestKind::Cancel;
    channel = "futures.order_cancel";
  } else if (cmd.kind == OrderCommandKind::Replace) {
    kind = RequestKind::Replace;
    channel = "futures.order_amend";
  }
  const RequestId rid = make_request_id(kind, cmd.cl_ord_id);
  JsonWriter w(out);
  w.begin_object().key("time").integer(time_s).key("channel").string(channel);
  w.key("event").string("api");
  w.key("payload").begin_object().key("req_id").string(rid.view());
  w.key("req_param").raw_value(std::string_view(param, n)).end_object();
  w.end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t GateOrderEncoder::encode_ws_login(std::int64_t time_s,
                                              std::string_view req_id,
                                              std::span<char> out) const noexcept {
  if (!signer_.usable()) return 0;
  const net::HexSha512 sig = signer_.sign_ws_api("futures.login", {}, time_s);
  char ts[24];
  JsonWriter w(out);
  w.begin_object().key("time").integer(time_s).key("channel").string("futures.login");
  w.key("event").string("api");
  w.key("payload").begin_object();
  w.key("api_key").string(signer_.api_key());
  w.key("signature").string(sig.view());
  w.key("timestamp").string(std::string_view(ts, format_int64(time_s, ts)));
  w.key("req_id").string(req_id);
  w.end_object().end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t GateOrderEncoder::encode_ping(std::int64_t time_s, std::span<char> out) noexcept {
  JsonWriter w(out);
  w.begin_object().key("time").integer(time_s).key("channel").string("futures.ping").end_object();
  return w.ok() ? w.size() : 0;
}

std::size_t GateOrderEncoder::encode_subscribe(std::string_view channel,
                                               std::span<const std::string_view> payload,
                                               std::int64_t time_s,
                                               std::span<char> out) const noexcept {
  if (!signer_.usable()) return 0;
  const net::HexSha512 sig = signer_.sign_ws_subscribe(channel, "subscribe", time_s);
  JsonWriter w(out);
  w.begin_object().key("time").integer(time_s).key("channel").string(channel);
  w.key("event").string("subscribe").key("payload").begin_array();
  for (std::string_view p : payload) w.string(p);
  w.end_array();
  w.key("auth").begin_object().key("method").string("api_key");
  w.key("KEY").string(signer_.api_key()).key("SIGN").string(sig.view()).end_object();
  w.end_object();
  return w.ok() ? w.size() : 0;
}

bool GateOrderEncoder::encode_rest(const OrderCommand& cmd,
                                   const OrderShadow* shadow,
                                   RestRequest& out) const {
  char param[512];
  const std::size_t n = write_param(cmd, shadow, param);
  if (n == 0) return false;
  out.query.clear();
  out.body.clear();
  switch (cmd.kind) {
    case OrderCommandKind::New:
      out.method = "POST";
      out.path = prefix() + "/orders";
      out.body.assign(param, n);
      out.is_order = true;
      break;
    case OrderCommandKind::Cancel: {
      char ref[48];
      const std::size_t rn = write_order_ref(cmd, shadow, ref);
      if (rn == 0) return false;
      out.method = "DELETE";
      out.path = prefix() + "/orders/" + std::string(ref, rn);
      out.is_order = false;
      break;
    }
    case OrderCommandKind::Replace: {
      if (shadow == nullptr) return false;
      char ref[48];
      const std::size_t rn = write_order_ref(cmd, shadow, ref);
      if (rn == 0) return false;
      // PUT /orders/{id} takes {"size","price"} only.
      char num[32];
      const std::size_t sn = signed_size(shadow->side, cmd.qty, num);
      if (sn == 0) return false;
      DecimalText px(cmd.price);
      char buf[160];
      JsonWriter w(buf);
      w.begin_object().key("size").raw_value(std::string_view(num, sn));
      w.key("price").string(px.view()).end_object();
      if (!w.ok()) return false;
      out.method = "PUT";
      out.path = prefix() + "/orders/" + std::string(ref, rn);
      out.body.assign(w.view());
      out.is_order = true;
      break;
    }
  }
  return true;
}

bool GateOrderEncoder::encode_rest_cancel_all(std::string_view contract, RestRequest& out) const {
  if (contract.empty()) return false;  // "contract" is required
  out.method = "DELETE";
  out.path = prefix() + "/orders";
  out.query = "contract=" + std::string(contract);
  out.body.clear();
  out.is_order = false;
  return true;
}

void GateOrderEncoder::encode_rest_open_orders(std::string_view contract,
                                               int limit,
                                               int offset,
                                               RestRequest& out) const {
  out.method = "GET";
  out.path = prefix() + "/orders";
  out.query = "status=open";
  if (!contract.empty()) out.query += "&contract=" + std::string(contract);
  out.query += "&limit=" + std::to_string(limit);
  if (offset > 0) out.query += "&offset=" + std::to_string(offset);
  out.body.clear();
  out.is_order = false;
}

void GateOrderEncoder::encode_rest_positions(RestRequest& out) const {
  out.method = "GET";
  out.path = prefix() + "/positions";
  out.query = "holding=true";
  out.body.clear();
  out.is_order = false;
}

void GateOrderEncoder::encode_rest_accounts(RestRequest& out) const {
  out.method = "GET";
  out.path = prefix() + "/accounts";
  out.query.clear();
  out.body.clear();
  out.is_order = false;
}

void GateOrderEncoder::encode_rest_my_trades(std::string_view contract,
                                             std::int64_t from_s,
                                             std::int64_t to_s,
                                             int limit,
                                             int offset,
                                             RestRequest& out) const {
  out.method = "GET";
  out.path = prefix() + "/my_trades_timerange";
  out.query.clear();
  if (!contract.empty()) out.query += "contract=" + std::string(contract) + "&";
  out.query += "from=" + std::to_string(from_s);
  if (to_s > 0) out.query += "&to=" + std::to_string(to_s);
  out.query += "&limit=" + std::to_string(limit);
  if (offset > 0) out.query += "&offset=" + std::to_string(offset);
  out.body.clear();
  out.is_order = false;
}

void GateOrderEncoder::encode_rest_fee(std::string_view contract, RestRequest& out) const {
  out.method = "GET";
  out.path = prefix() + "/fee";
  out.query = contract.empty() ? std::string{} : "contract=" + std::string(contract);
  out.body.clear();
  out.is_order = false;
}

bool GateOrderEncoder::encode_rest_countdown(std::int64_t timeout_s,
                                             std::string_view contract,
                                             RestRequest& out) const {
  char buf[128];
  JsonWriter w(buf);
  w.begin_object().key("timeout").integer(timeout_s);
  if (!contract.empty()) w.key("contract").string(contract);
  w.end_object();
  if (!w.ok()) return false;
  out.method = "POST";
  out.path = prefix() + "/countdown_cancel_all";
  out.query.clear();
  out.body.assign(w.view());
  out.is_order = false;
  return true;
}

// ---- decoder -------------------------------------------------------------------------------

struct GateResponseDecoder::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

GateResponseDecoder::GateResponseDecoder(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}
GateResponseDecoder::~GateResponseDecoder() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// A header field Gate sends as a string ("200") or as a number.
[[nodiscard]] std::int64_t int_or_text(od::value v) noexcept {
  std::int64_t i = 0;
  if (v.get_int64().get(i) == sj::SUCCESS) return i;
  std::string_view s;
  if (v.get_string().get(s) == sj::SUCCESS) {
    if (const auto p = parse_int64(s)) return *p;
  }
  return -1;
}

// A size Gate sends as a number or a string, signed.
bool size_of(od::value v, Qty& out, bool& positive) noexcept {
  std::string_view s;
  if (v.get_string().get(s) == sj::SUCCESS) return parse_signed_size(s, out, positive);
  std::int64_t i = 0;
  if (v.get_int64().get(i) == sj::SUCCESS) {
    positive = i >= 0;
    out = Qty::from_int(positive ? i : -i);
    return true;
  }
  double d = 0.0;
  if (v.get_double().get(d) != sj::SUCCESS) return false;
  positive = d >= 0.0;
  char buf[48];
  const int n = std::snprintf(buf, sizeof buf, "%.8f", positive ? d : -d);
  if (n <= 0) return false;
  return parse_signed_size(std::string_view(buf, static_cast<std::size_t>(n)), out, positive);
}

// The order object's fields (result of a place/cancel/amend, or a REST order reply).
[[gnu::noinline]] bool read_order(od::object& o, ApiResponse& r, char* id_buf) noexcept {
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    od::value v = field.value();
    if (key == "id") {
      std::string_view s;
      if (v.get_string().get(s) == sj::SUCCESS) {
        r.order_id = s;
      } else {
        std::int64_t i = 0;
        if (v.get_int64().get(i) != sj::SUCCESS) return false;
        r.order_id = std::string_view(id_buf, format_int64(i, id_buf));
      }
    } else if (key == "id_string") {
      std::string_view s;
      if (v.get_string().get(s) == sj::SUCCESS) r.order_id = s;
    } else if (key == "text") {
      if (v.get_string().get(r.text) != sj::SUCCESS) return false;
    } else if (key == "status") {
      if (v.get_string().get(r.order_status) != sj::SUCCESS) return false;
    } else if (key == "finish_as") {
      if (v.get_string().get(r.finish_as) != sj::SUCCESS) return false;
    } else if (key == "size") {
      if (!size_of(v, r.size, r.bid)) return false;
    } else if (key == "left") {
      bool pos = true;
      if (!size_of(v, r.left, pos)) return false;
    } else if (key == "price") {
      std::string_view s;
      if (v.get_string().get(s) != sj::SUCCESS) return false;
      if (const auto p = parse_price(s)) r.price = *p;
    } else if (key == "create_time") {
      double d = 0.0;
      if (v.get_double().get(d) == sj::SUCCESS)
        r.create_time_ms = static_cast<std::int64_t>(d * 1000.0);
    } else if (key == "create_time_ms") {
      std::int64_t t = 0;
      if (v.get_int64().get(t) == sj::SUCCESS) r.create_time_ms = t;
    }
  }
  return true;
}

}  // namespace

ParseStatus GateResponseDecoder::decode_ws(std::string_view json, ApiResponse& r) noexcept {
  r = ApiResponse{};
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  std::string_view s;
  if (root["request_id"].get_string().get(s) != sj::SUCCESS) return ParseStatus::Ignored;
  r.request_id = s;
  root.reset();
  bool ack = false;
  if (root["ack"].get_bool().get(ack) == sj::SUCCESS) r.ack = ack;
  root.reset();
  {
    od::object header;
    if (root["header"].get_object().get(header) == sj::SUCCESS) {
      for (auto field : header) {
        std::string_view key;
        if (field.unescaped_key().get(key) != sj::SUCCESS) return ParseStatus::Malformed;
        od::value v = field.value();
        if (key == "status") {
          r.status = static_cast<int>(int_or_text(v));
        } else if (key == "channel") {
          if (v.get_string().get(r.channel) != sj::SUCCESS) return ParseStatus::Malformed;
        } else if (key == "response_time") {
          r.response_time_ms = int_or_text(v);
        } else if (key == "x_gate_ratelimit_limit") {
          r.limit = int_or_text(v);
        } else if (key == "x_gate_ratelimit_requests_remain") {
          r.remain = int_or_text(v);
        } else if (key == "x_gat_ratelimit_reset_timestamp" ||
                   key == "x_gate_ratelimit_reset_timestamp") {
          r.reset_ms = int_or_text(v);
        }
      }
    }
  }
  root.reset();
  od::object data;
  if (root["data"].get_object().get(data) != sj::SUCCESS) {
    r.success = r.ack;
    return ParseStatus::Ok;
  }
  {
    od::object errs;
    if (data["errs"].get_object().get(errs) == sj::SUCCESS) {
      if (errs["label"].get_string().get(r.label) != sj::SUCCESS) r.label = "UNKNOWN";
      errs.reset();
      if (errs["message"].get_string().get(r.message) != sj::SUCCESS) r.message = {};
      r.success = false;
      return ParseStatus::Ok;
    }
  }
  data.reset();
  od::object result;
  if (data["result"].get_object().get(result) == sj::SUCCESS) {
    if (r.ack) {
      r.success = true;  // the echo of our request
      return ParseStatus::Ok;
    }
    std::string_view uid;
    if (result["uid"].get_string().get(uid) == sj::SUCCESS) {
      r.uid = uid;  // a login reply
    } else {
      result.reset();
      if (!read_order(result, r, id_buf_)) return ParseStatus::Malformed;
    }
    r.success = r.status == 0 || (r.status >= 200 && r.status < 300);
    return ParseStatus::Ok;
  }
  // A result that is not an object (cancel_cp returns a list): success by status.
  r.success = r.status == 0 || (r.status >= 200 && r.status < 300);
  return ParseStatus::Ok;
}

ParseStatus GateResponseDecoder::decode_rest(std::string_view json,
                                             int http_status,
                                             ApiResponse& r) noexcept {
  r = ApiResponse{};
  r.status = http_status;
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  std::string_view s;
  if (root["label"].get_string().get(s) == sj::SUCCESS) {
    r.label = s;
    root.reset();
    if (root["message"].get_string().get(s) == sj::SUCCESS) r.message = s;
    r.success = false;
    return ParseStatus::Ok;
  }
  root.reset();
  if (!read_order(root, r, id_buf_)) return ParseStatus::Malformed;
  r.success = http_status >= 200 && http_status < 300;
  return ParseStatus::Ok;
}

}  // namespace fastmm::venues::gate
