#include "fastmm/venues/coinbase/coinbase_rest_decoder.hpp"

#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cmath>

namespace fastmm::venues::coinbase {

namespace sj = simdjson;
namespace dom = simdjson::dom;

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

// An error body where a value was expected.
std::string not_expected(const dom::element& root, std::string_view what) {
  std::string_view msg;
  if (root.is_object() && root["message"].get(msg) == sj::SUCCESS)
    return std::string(what) + ": " + std::string(msg);
  return std::string(what) + ": unexpected reply";
}

std::string order_of(const dom::element& e, OrderRow& o) {
  o.id = text(e, "id");
  if (o.id.empty()) return "order without id";
  o.client_oid = text(e, "client_oid");
  o.product_id = text(e, "product_id");
  o.side = text(e, "side");
  o.type = text(e, "type");
  o.status = text(e, "status");
  o.reject_reason = text(e, "reject_reason");
  static_cast<void>(fixed(e, "price", o.price));
  static_cast<void>(fixed(e, "size", o.size));
  static_cast<void>(fixed(e, "filled_size", o.filled_size));
  return {};
}

}  // namespace

std::string decode_product(std::string_view json, ProductInfo& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "products: invalid JSON";
  out.id = text(root, "id");
  if (out.id.empty()) return not_expected(root, "products");
  out.base = text(root, "base_currency");
  out.quote = text(root, "quote_currency");
  out.status = text(root, "status");
  if (!fixed(root, "quote_increment", out.tick) || !out.tick.is_positive())
    return "products: bad quote_increment for " + out.id;
  if (!fixed(root, "base_increment", out.lot) || !out.lot.is_positive())
    return "products: bad base_increment for " + out.id;
  static_cast<void>(fixed(root, "base_min_size", out.min_size));
  static_cast<void>(fixed(root, "min_market_funds", out.min_funds));
  out.trading_disabled = flag(root, "trading_disabled");
  out.cancel_only = flag(root, "cancel_only");
  out.limit_only = flag(root, "limit_only");
  out.post_only = flag(root, "post_only");
  return {};
}

std::string decode_server_time(std::string_view json, std::int64_t& epoch_ms) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "time: invalid JSON";
  double epoch = 0;
  if (root["epoch"].get(epoch) == sj::SUCCESS && epoch > 0) {
    epoch_ms = static_cast<std::int64_t>(std::llround(epoch * 1000.0));
    return {};
  }
  std::string_view iso;
  if (root["iso"].get(iso) == sj::SUCCESS) {
    const std::int64_t ns = parse_time_ns(iso);
    if (ns > 0) {
      epoch_ms = ns / 1'000'000;
      return {};
    }
  }
  return not_expected(root, "time");
}

std::string decode_orders(std::string_view json, std::vector<OrderRow>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "orders: invalid JSON";
  dom::array arr;
  if (root.get(arr) != sj::SUCCESS) return not_expected(root, "orders");
  for (dom::element e : arr) {
    OrderRow o;
    if (std::string err = order_of(e, o); !err.empty()) return "orders: " + err;
    out.push_back(std::move(o));
  }
  return {};
}

std::string decode_order(std::string_view json, OrderRow& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "order: invalid JSON";
  if (!root.is_object() || root["id"].error() != sj::SUCCESS) return not_expected(root, "order");
  if (std::string err = order_of(root, out); !err.empty()) return "order: " + err;
  return {};
}

std::string decode_fills(std::string_view json, std::vector<FillRow>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "fills: invalid JSON";
  dom::array arr;
  if (root.get(arr) != sj::SUCCESS) return not_expected(root, "fills");
  for (dom::element e : arr) {
    FillRow f;
    if (e["trade_id"].get(f.trade_id) != sj::SUCCESS) return "fills: row without trade_id";
    f.order_id = text(e, "order_id");
    f.product_id = text(e, "product_id");
    f.side = text(e, "side");
    f.liquidity = text(e, "liquidity");
    if (!fixed(e, "price", f.price) || !fixed(e, "size", f.size))
      return "fills: bad price or size in trade " + std::to_string(f.trade_id);
    // The fee comes with 16 decimals ("0.2433492642000000"): rounded to 8.
    std::string_view fee;
    if (e["fee"].get(fee) == sj::SUCCESS && !fee.empty()) {
      const auto v = parse_avg_price(fee);
      if (!v) return "fills: bad fee in trade " + std::to_string(f.trade_id);
      f.fee = Notional::from_raw(v->raw);
    }
    std::string_view created;
    if (e["created_at"].get(created) == sj::SUCCESS) {
      const std::int64_t ns = parse_time_ns(created);
      if (ns > 0) f.time_ms = ns / 1'000'000;
    }
    out.push_back(std::move(f));
  }
  return {};
}

std::string decode_ids(std::string_view json, std::vector<std::string>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "ids: invalid JSON";
  std::string_view one;
  if (root.get(one) == sj::SUCCESS) {
    out.emplace_back(one);
    return {};
  }
  dom::array arr;
  if (root.get(arr) != sj::SUCCESS) return not_expected(root, "ids");
  for (dom::element e : arr) {
    std::string_view id;
    if (e.get(id) != sj::SUCCESS) return "ids: not a string";
    out.emplace_back(id);
  }
  return {};
}

std::string decode_accounts(std::string_view json, std::vector<AccountRow>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "accounts: invalid JSON";
  dom::array arr;
  if (root.get(arr) != sj::SUCCESS) return not_expected(root, "accounts");
  for (dom::element e : arr) {
    AccountRow a;
    a.id = text(e, "id");
    a.currency = text(e, "currency");
    if (a.id.empty() || a.currency.empty()) return "accounts: account without id or currency";
    std::string_view s;
    auto amount = [&](const char* key, Notional& v) {
      if (e[key].get(s) != sj::SUCCESS) return false;
      const auto n = parse_balance(s);
      if (n) v = *n;
      return n.has_value();
    };
    if (!amount("available", a.available) || !amount("hold", a.hold))
      return "accounts: bad available or hold for " + a.currency;
    static_cast<void>(amount("balance", a.balance));
    a.trading_enabled = flag(e, "trading_enabled");
    out.push_back(std::move(a));
  }
  return {};
}

std::string error_message(std::string_view json) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS || !root.is_object())
    return {};
  return text(root, "message");
}

}  // namespace fastmm::venues::coinbase
