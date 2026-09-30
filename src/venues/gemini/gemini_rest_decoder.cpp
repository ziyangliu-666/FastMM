#include "fastmm/venues/gemini/gemini_rest_decoder.hpp"

#include "fastmm/venues/coinbase/coinbase_wire.hpp"  // parse_time_ns (RFC 3339)
#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

namespace fastmm::venues::gemini {

namespace sj = simdjson;
namespace dom = simdjson::dom;

namespace {

std::string parse(dom::parser& parser, std::string_view json, dom::element& root) {
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "invalid JSON";
  return {};
}

// The error envelope as text, or empty when `root` is not one.
std::string error_text(const dom::element& root) {
  if (!root.is_object()) return {};
  std::string_view result;
  if (root["result"].get(result) != sj::SUCCESS || result != "error") return {};
  std::string_view reason;
  std::string_view message;
  if (root["reason"].get(reason) != sj::SUCCESS) reason = "?";
  if (root["message"].get(message) != sj::SUCCESS) message = {};
  return std::string(reason) + ": " + std::string(message);
}

std::string text(const dom::element& e, const char* key) {
  std::string_view s;
  if (e[key].get(s) == sj::SUCCESS) return std::string(s);
  std::int64_t i = 0;
  if (e[key].get(i) == sj::SUCCESS) return std::to_string(i);
  std::uint64_t u = 0;
  if (e[key].get(u) == sj::SUCCESS) return std::to_string(u);
  return {};
}

// A decimal sent as a string or as a JSON number ("tick_size":1e-08).
template <class F>
bool fixed(const dom::element& e, const char* key, F& out) {
  std::string_view s;
  if (e[key].get(s) == sj::SUCCESS) {
    if (s.empty()) return false;
    const auto v = parse_fixed<F>(s);
    if (!v) return false;
    out = *v;
    return true;
  }
  double d = 0;
  if (e[key].get(d) != sj::SUCCESS) return false;
  const auto v = F::from_double_checked(d);
  if (!v) return false;
  out = *v;
  return true;
}

// An amount the venue computed, with more decimals than 8 possibly: rounded to 8.
bool rounded(const dom::element& e, const char* key, Notional& out) {
  std::string_view s;
  if (e[key].get(s) == sj::SUCCESS) {
    const auto v = parse_rounded<Notional>(s);
    if (!v) return false;
    out = *v;
    return true;
  }
  return fixed(e, key, out);
}

bool integer(const dom::element& e, const char* key, std::int64_t& out) {
  if (e[key].get(out) == sj::SUCCESS) return true;
  std::string_view s;
  if (e[key].get(s) != sj::SUCCESS) return false;
  const auto v = parse_int64(s);
  if (!v) return false;
  out = *v;
  return true;
}

// The array a list endpoint returns; an error text when it is the error envelope.
std::string open_array(dom::parser& parser,
                       std::string_view json,
                       std::string_view what,
                       dom::array& out) {
  dom::element root;
  if (std::string err = parse(parser, json, root); !err.empty())
    return std::string(what) + ": " + err;
  if (std::string err = error_text(root); !err.empty()) return std::string(what) + ": " + err;
  if (root.get(out) != sj::SUCCESS) return std::string(what) + ": not an array";
  return {};
}

}  // namespace

bool decode_error(std::string_view json, std::string& reason, std::string& message) {
  dom::parser parser;
  dom::element root;
  if (!parse(parser, json, root).empty() || !root.is_object()) return false;
  std::string_view result;
  if (root["result"].get(result) != sj::SUCCESS || result != "error") return false;
  reason = text(root, "reason");
  message = text(root, "message");
  return true;
}

std::string decode_symbol_details(std::string_view json, SymbolDetails& out) {
  dom::parser parser;
  dom::element root;
  if (std::string err = parse(parser, json, root); !err.empty()) return "symbol details: " + err;
  if (std::string err = error_text(root); !err.empty()) return "symbol details: " + err;
  if (!root.is_object()) return "symbol details: not an object";
  out.symbol = text(root, "symbol");
  out.base = text(root, "base_currency");
  out.quote = text(root, "quote_currency");
  out.collateral = text(root, "contract_price_currency");
  out.product_type = text(root, "product_type");
  out.contract_type = text(root, "contract_type");
  out.status = text(root, "status");
  if (out.symbol.empty()) return "symbol details: no symbol";
  if (!fixed(root, "quote_increment", out.tick)) return "symbol details: bad quote_increment";
  if (!fixed(root, "tick_size", out.lot)) return "symbol details: bad tick_size";
  if (!fixed(root, "min_order_size", out.min_qty)) out.min_qty = out.lot;
  return {};
}

std::string decode_active_orders(std::string_view json, std::vector<ActiveOrder>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_array(parser, json, "orders", data); !err.empty()) return err;
  for (dom::element e : data) {
    ActiveOrder o;
    o.order_id = text(e, "order_id");
    o.client_order_id = text(e, "client_order_id");
    o.symbol = text(e, "symbol");
    o.side = text(e, "side");
    if (o.order_id.empty() || o.symbol.empty()) return "orders: entry without order_id or symbol";
    if (!fixed(e, "price", o.price)) return "orders: bad price for " + o.order_id;
    if (!fixed(e, "original_amount", o.original))
      return "orders: bad original_amount for " + o.order_id;
    static_cast<void>(fixed(e, "executed_amount", o.executed));
    out.push_back(std::move(o));
  }
  return {};
}

std::string decode_positions(std::string_view json, std::vector<PositionRow>& out) {
  dom::parser parser;
  dom::element root;
  if (std::string err = parse(parser, json, root); !err.empty()) return "positions: " + err;
  if (std::string err = error_text(root); !err.empty()) return "positions: " + err;
  dom::array rows;
  if (root.is_object()) {
    if (root["openPositions"].get(rows) != sj::SUCCESS) return "positions: no openPositions";
  } else if (root.get(rows) != sj::SUCCESS) {
    return "positions: not an array";
  }
  for (dom::element e : rows) {
    PositionRow p;
    p.symbol = text(e, "symbol");
    p.instrument_type = text(e, "instrument_type");
    if (p.symbol.empty()) return "positions: entry without symbol";
    if (!fixed(e, "quantity", p.qty)) return "positions: bad quantity for " + p.symbol;
    std::string_view avg;
    if (e["average_cost"].get(avg) == sj::SUCCESS && !avg.empty()) {
      const auto v = parse_avg_price(avg);
      if (!v) return "positions: bad average_cost for " + p.symbol;
      p.avg_px = *v;
    }
    out.push_back(std::move(p));
  }
  return {};
}

std::string decode_trades(std::string_view json, std::vector<TradeRow>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_array(parser, json, "mytrades", data); !err.empty()) return err;
  for (dom::element e : data) {
    TradeRow t;
    t.tid = text(e, "tid");
    t.order_id = text(e, "order_id");
    t.client_order_id = text(e, "client_order_id");
    t.symbol = text(e, "symbol");
    t.fee_currency = text(e, "fee_currency");
    t.break_type = text(e, "break");
    t.buy = text(e, "type") == "Buy";
    bool aggressor = false;
    if (e["aggressor"].get(aggressor) == sj::SUCCESS) t.aggressor = aggressor;
    if (t.tid.empty()) return "mytrades: entry without tid";
    if (!fixed(e, "price", t.price)) return "mytrades: bad price for " + t.tid;
    if (!fixed(e, "amount", t.qty)) return "mytrades: bad amount for " + t.tid;
    static_cast<void>(fixed(e, "fee_amount", t.fee));
    if (!integer(e, "timestampms", t.time_ms)) {
      std::int64_t s = 0;
      if (!integer(e, "timestamp", s)) return "mytrades: no timestamp for " + t.tid;
      t.time_ms = s * 1000;
    }
    out.push_back(std::move(t));
  }
  return {};
}

std::string decode_funding(std::string_view json, std::vector<FundingRow>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_array(parser, json, "fundingPayment", data); !err.empty()) return err;
  for (dom::element e : data) {
    dom::element h;
    if (e["hourlyFundingTransfer"].get(h) != sj::SUCCESS) continue;  // another event type
    FundingRow f;
    f.symbol = text(h, "instrumentSymbol");
    f.asset = text(h, "assetCode");
    if (!integer(h, "timestamp", f.time_ms)) return "fundingPayment: entry without timestamp";
    dom::element q;
    if (h["quantity"].get(q) != sj::SUCCESS || !fixed(q, "value", f.amount))
      return "fundingPayment: bad quantity";
    if (f.asset.empty()) f.asset = text(q, "currency");
    const std::string action = text(h, "action");
    if (action == "Debit") {
      f.amount = Notional{} - f.amount;
    } else if (action != "Credit") {
      return "fundingPayment: unknown action " + action;
    }
    out.push_back(std::move(f));
  }
  return {};
}

std::string decode_balances(std::string_view json, std::vector<BalanceRow>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_array(parser, json, "balances", data); !err.empty()) return err;
  for (dom::element e : data) {
    BalanceRow b;
    b.currency = text(e, "currency");
    if (b.currency.empty()) return "balances: entry without currency";
    if (!rounded(e, "amount", b.amount)) return "balances: bad amount for " + b.currency;
    if (!rounded(e, "available", b.available)) return "balances: bad available for " + b.currency;
    std::string_view ts;
    if (e["_timestamp"].get(ts) == sj::SUCCESS) {
      const std::int64_t ns = coinbase::parse_time_ns(ts);
      if (ns > 0) b.time_ms = ns / 1'000'000;
    }
    out.push_back(std::move(b));
  }
  return {};
}

std::string decode_margin(std::string_view json, MarginRow& out) {
  dom::parser parser;
  dom::element root;
  if (std::string err = parse(parser, json, root); !err.empty()) return "margin: " + err;
  if (std::string err = error_text(root); !err.empty()) return "margin: " + err;
  if (!root.is_object()) return "margin: not an object";
  if (!rounded(root, "margin_assets_value", out.assets_value) ||
      !rounded(root, "initial_margin", out.initial) ||
      !rounded(root, "available_margin", out.available))
    return "margin: bad margin_assets_value, initial_margin or available_margin";
  static_cast<void>(rounded(root, "margin_maintenance_limit", out.maintenance));
  return {};
}

std::string decode_cancel_result(std::string_view json,
                                 std::size_t& cancelled,
                                 std::vector<std::string>& rejects) {
  dom::parser parser;
  dom::element root;
  if (std::string err = parse(parser, json, root); !err.empty()) return "cancel: " + err;
  if (std::string err = error_text(root); !err.empty()) return "cancel: " + err;
  std::string_view result;
  if (root["result"].get(result) != sj::SUCCESS || result != "ok") return "cancel: result not ok";
  dom::element details;
  if (root["details"].get(details) != sj::SUCCESS) return {};
  dom::array a;
  if (details["cancelledOrders"].get(a) == sj::SUCCESS) cancelled = a.size();
  if (details["cancelRejects"].get(a) == sj::SUCCESS) {
    for (dom::element id : a) {
      std::int64_t v = 0;
      std::string_view s;
      if (id.get(v) == sj::SUCCESS) {
        rejects.push_back(std::to_string(v));
      } else if (id.get(s) == sj::SUCCESS) {
        rejects.emplace_back(s);
      }
    }
  }
  return {};
}

}  // namespace fastmm::venues::gemini
