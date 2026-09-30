#include "fastmm/venues/coinbase/advanced_rest.hpp"

#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <simdjson.h>

namespace fastmm::venues::coinbase {

namespace sj = simdjson;
namespace dom = simdjson::dom;

// ---- requests --------------------------------------------------------------------------------

namespace {

bool append(char* buf, std::size_t cap, std::size_t& n, std::string_view s) noexcept {
  if (s.size() > cap - n) return false;
  for (const char c : s) buf[n++] = c;
  return true;
}

void add_list(std::string& p, std::string_view key, std::span<const std::string> values) {
  for (const std::string& v : values) {
    p += p.find('?') == std::string::npos ? '?' : '&';
    p.append(key).append("=").append(v);
  }
}

}  // namespace

bool AdvancedOrderEncoder::encode_new(const OrderCommand& cmd, OrderRequest& out) const noexcept {
  if (cmd.kind != OrderCommandKind::New) return false;
  const std::string_view product = symbols_.venue_symbol(cmd.instrument);
  if (product.empty()) return false;
  out.method = "POST";
  out.path_n = 0;
  if (!append(out.path.data(), out.path.size(), out.path_n, kAdvancedPrefix) ||
      !append(out.path.data(), out.path.size(), out.path_n, "/orders"))
    return false;
  JsonWriter w(std::span<char>(out.body.data(), out.body.size()));
  w.begin_object().key("client_order_id").string(encode_cl_ord_id(cmd.cl_ord_id).view());
  w.key("product_id").string(product);
  w.key("side").string(cmd.side == Side::Buy ? "BUY" : "SELL");
  w.key("order_configuration").begin_object();
  const DecimalText size(cmd.qty);
  if (cmd.type == OrderType::Market) {
    w.key("market_market_ioc").begin_object().key("base_size").string(size.view()).end_object();
  } else {
    const DecimalText price(cmd.price);
    std::string_view config = "limit_limit_gtc";
    if (cmd.type != OrderType::PostOnly && cmd.tif == TimeInForce::Ioc) config = "sor_limit_ioc";
    if (cmd.type != OrderType::PostOnly && cmd.tif == TimeInForce::Fok) config = "limit_limit_fok";
    w.key(config).begin_object();
    w.key("base_size").string(size.view()).key("limit_price").string(price.view());
    if (cmd.type == OrderType::PostOnly) w.key("post_only").boolean(true);
    w.end_object();
  }
  w.end_object().end_object();
  if (!w.ok()) return false;
  out.body_n = w.size();
  return true;
}

std::string AdvancedOrderEncoder::cancel_body(std::span<const std::string> order_ids) {
  std::string b = R"({"order_ids":[)";
  for (std::size_t i = 0; i < order_ids.size(); ++i) {
    if (i != 0) b += ',';
    b += '"';
    b += order_ids[i];
    b += '"';
  }
  b += "]}";
  return b;
}

std::string AdvancedOrderEncoder::open_orders_path(std::span<const std::string> products,
                                                   std::string_view cursor) {
  std::string p = std::string(kAdvancedPrefix) + "/orders/historical/batch?order_status=OPEN";
  add_list(p, "product_ids", products);
  p += "&limit=250";
  if (!cursor.empty()) p.append("&cursor=").append(cursor);
  return p;
}

std::string AdvancedOrderEncoder::fills_path(std::span<const std::string> products,
                                             std::int64_t start_ms,
                                             std::int64_t end_ms,
                                             std::string_view cursor,
                                             int limit) {
  std::string p = std::string(kAdvancedPrefix) + "/orders/historical/fills";
  add_list(p, "product_ids", products);
  p += p.find('?') == std::string::npos ? '?' : '&';
  p += "limit=" + std::to_string(limit);
  if (start_ms > 0) p.append("&start_sequence_timestamp=").append(format_time_ms(start_ms).view());
  if (end_ms > 0) p.append("&end_sequence_timestamp=").append(format_time_ms(end_ms).view());
  if (!cursor.empty()) p.append("&cursor=").append(cursor);
  return p;
}

std::string AdvancedOrderEncoder::order_fills_path(std::string_view order_id) {
  return std::string(kAdvancedPrefix) +
         "/orders/historical/fills?order_ids=" + std::string(order_id) + "&limit=100";
}

std::string AdvancedOrderEncoder::order_path(std::string_view order_id) {
  return std::string(kAdvancedPrefix) + "/orders/historical/" + std::string(order_id);
}

std::string AdvancedOrderEncoder::product_path(std::string_view product) {
  return std::string(kAdvancedPrefix) + "/market/products/" + std::string(product);
}

// ---- replies ---------------------------------------------------------------------------------

namespace {

std::string text(const dom::element& e, const char* key) {
  std::string_view s;
  if (e[key].get(s) != sj::SUCCESS) return {};
  return std::string(s);
}

bool flag(const dom::element& e, const char* key) {
  bool b = false;
  return e[key].get(b) == sj::SUCCESS && b;
}

template <class F>
bool fixed(const dom::element& e, const char* key, F& out) {
  std::string_view s;
  if (e[key].get(s) != sj::SUCCESS || s.empty()) return false;
  const auto v = parse_fixed<F>(s);
  if (!v) return false;
  out = *v;
  return true;
}

// A value the venue computed, which may carry more than 8 decimals: rounded.
bool rounded(const dom::element& e, const char* key, Notional& out) {
  std::string_view s;
  if (e[key].get(s) != sj::SUCCESS || s.empty()) return false;
  const auto v = parse_avg_price(s);
  if (!v) return false;
  out = Notional::from_raw(v->raw);
  return true;
}

std::string not_expected(const dom::element& root, std::string_view what) {
  std::string_view msg;
  if (root.is_object() && root["message"].get(msg) == sj::SUCCESS)
    return std::string(what) + ": " + std::string(msg);
  return std::string(what) + ": unexpected reply";
}

std::string order_of(const dom::element& e, AdvOrderRow& o) {
  o.order_id = text(e, "order_id");
  if (o.order_id.empty()) return "order without order_id";
  o.client_order_id = text(e, "client_order_id");
  o.product_id = text(e, "product_id");
  o.side = text(e, "side");
  o.status = text(e, "status");
  static_cast<void>(fixed(e, "filled_size", o.filled_size));
  dom::object cfg;
  if (e["order_configuration"].get(cfg) == sj::SUCCESS) {
    for (auto [k, v] : cfg) {
      static_cast<void>(k);
      if (!v.is_object()) continue;
      static_cast<void>(fixed(v, "limit_price", o.price));
      static_cast<void>(fixed(v, "base_size", o.size));
      break;  // one configuration an order
    }
  }
  return {};
}

}  // namespace

std::string decode_adv_product(std::string_view json, ProductInfo& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "products: invalid JSON";
  out.id = text(root, "product_id");
  if (out.id.empty()) return not_expected(root, "products");
  out.base = text(root, "base_currency_id");
  out.quote = text(root, "quote_currency_id");
  out.status = text(root, "status");
  if (!fixed(root, "quote_increment", out.tick) || !out.tick.is_positive())
    return "products: bad quote_increment for " + out.id;
  if (!fixed(root, "base_increment", out.lot) || !out.lot.is_positive())
    return "products: bad base_increment for " + out.id;
  static_cast<void>(fixed(root, "base_min_size", out.min_size));
  static_cast<void>(fixed(root, "quote_min_size", out.min_funds));
  out.trading_disabled =
      flag(root, "trading_disabled") || flag(root, "is_disabled") || flag(root, "view_only");
  out.cancel_only = flag(root, "cancel_only");
  out.limit_only = flag(root, "limit_only");
  out.post_only = flag(root, "post_only");
  if (const std::string type = text(root, "product_type"); !type.empty() && type != "SPOT")
    return "products: " + out.id + " is a " + type + " product; only SPOT is supported";
  return {};
}

std::string decode_adv_time(std::string_view json, std::int64_t& epoch_ms) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "time: invalid JSON";
  std::string_view ms;
  if (root["epochMillis"].get(ms) == sj::SUCCESS) {
    const auto v = parse_int64(ms);
    if (v) {
      epoch_ms = *v;
      return {};
    }
  }
  return not_expected(root, "time");
}

std::string decode_create_reply(std::string_view json, CreateReply& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "orders: invalid JSON";
  if (root["success"].get(out.success) != sj::SUCCESS) return not_expected(root, "orders");
  dom::element ok;
  if (root["success_response"].get(ok) == sj::SUCCESS) out.order_id = text(ok, "order_id");
  dom::element err;
  if (root["error_response"].get(err) == sj::SUCCESS) {
    out.failure_reason = text(err, "new_order_failure_reason");
    if (out.failure_reason.empty() || out.failure_reason == "UNKNOWN_FAILURE_REASON") {
      const std::string preview = text(err, "preview_failure_reason");
      const std::string e = text(err, "error");
      if (!e.empty() && e != "UNKNOWN_FAILURE_REASON") {
        out.failure_reason = e;
      } else if (!preview.empty() && preview != "UNKNOWN_PREVIEW_FAILURE_REASON") {
        out.failure_reason = preview;
      }
    }
    out.message = text(err, "message");
    if (const std::string d = text(err, "error_details"); !d.empty()) out.message = d;
  }
  if (out.success && out.order_id.empty()) return "orders: success without order_id";
  return {};
}

std::string decode_cancel_results(std::string_view json, std::vector<CancelResult>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "batch_cancel: invalid JSON";
  dom::array arr;
  if (root["results"].get(arr) != sj::SUCCESS) return not_expected(root, "batch_cancel");
  for (dom::element e : arr) {
    CancelResult c;
    if (e["success"].get(c.success) != sj::SUCCESS) return "batch_cancel: result without success";
    c.order_id = text(e, "order_id");
    c.failure_reason = text(e, "failure_reason");
    out.push_back(std::move(c));
  }
  return {};
}

std::string decode_adv_orders(std::string_view json,
                              std::vector<AdvOrderRow>& out,
                              std::string& cursor,
                              bool& has_next) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "orders: invalid JSON";
  dom::array arr;
  if (root["orders"].get(arr) != sj::SUCCESS) return not_expected(root, "orders");
  for (dom::element e : arr) {
    AdvOrderRow o;
    if (std::string err = order_of(e, o); !err.empty()) return "orders: " + err;
    out.push_back(std::move(o));
  }
  cursor = text(root, "cursor");
  has_next = flag(root, "has_next");
  return {};
}

std::string decode_adv_order(std::string_view json, AdvOrderRow& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "order: invalid JSON";
  dom::element o;
  if (root["order"].get(o) != sj::SUCCESS) return not_expected(root, "order");
  if (std::string err = order_of(o, out); !err.empty()) return "order: " + err;
  return {};
}

std::string decode_adv_fills(std::string_view json,
                             std::vector<AdvFillRow>& out,
                             std::string& cursor) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "fills: invalid JSON";
  dom::array arr;
  if (root["fills"].get(arr) != sj::SUCCESS) return not_expected(root, "fills");
  for (dom::element e : arr) {
    AdvFillRow f;
    f.trade_id = text(e, "trade_id");
    if (f.trade_id.empty()) return "fills: row without trade_id";
    f.order_id = text(e, "order_id");
    f.product_id = text(e, "product_id");
    f.side = text(e, "side");
    f.liquidity = text(e, "liquidity_indicator");
    if (!fixed(e, "price", f.price) || !fixed(e, "size", f.size))
      return "fills: bad price or size in trade " + f.trade_id;
    if (!rounded(e, "commission", f.commission))
      return "fills: bad commission in trade " + f.trade_id;
    std::string_view t;
    if (e["trade_time"].get(t) == sj::SUCCESS) {
      const std::int64_t ns = parse_time_ns(t);
      if (ns > 0) f.time_ms = ns / 1'000'000;
    }
    out.push_back(std::move(f));
  }
  cursor = text(root, "cursor");
  return {};
}

std::string decode_adv_accounts(std::string_view json, std::size_t& accounts, bool& has_next) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "accounts: invalid JSON";
  dom::array arr;
  if (root["accounts"].get(arr) != sj::SUCCESS) return not_expected(root, "accounts");
  accounts = arr.size();
  has_next = flag(root, "has_next");
  return {};
}

std::string adv_error_message(std::string_view json) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS || !root.is_object())
    return {};
  std::string m = text(root, "message");
  if (m.empty()) m = text(root, "error");
  return m;
}

}  // namespace fastmm::venues::coinbase
