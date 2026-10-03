#include "fastmm/venues/gate/gate_rest_decoder.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <charconv>
#include <cmath>

namespace fastmm::venues::gate {

namespace sj = simdjson;
namespace dom = simdjson::dom;

namespace {

// Gate sends most numbers as strings and some (sizes, user ids, times) as JSON numbers; the
// element is read either way, as text.
std::string number_text(const dom::element& e) {
  std::string_view s;
  if (e.get(s) == sj::SUCCESS) return std::string(s);
  std::int64_t i = 0;
  if (e.get(i) == sj::SUCCESS) return std::to_string(i);
  double d = 0.0;
  if (e.get(d) == sj::SUCCESS) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::fixed, 8);
    if (r.ec != std::errc{}) return {};
    std::string t(buf, r.ptr);
    // "1.50000000" -> "1.5", "3.00000000" -> "3"
    if (t.find('.') != std::string::npos) {
      while (!t.empty() && t.back() == '0') t.pop_back();
      if (!t.empty() && t.back() == '.') t.pop_back();
    }
    return t;
  }
  return {};
}

std::string field_text(const dom::element& obj, const char* key) {
  dom::element e;
  if (obj[key].get(e) != sj::SUCCESS) return {};
  return number_text(e);
}

template <class F>
bool fixed_field(const dom::element& obj, const char* key, F& out) {
  const std::string t = field_text(obj, key);
  if (t.empty()) return false;
  const auto v = parse_rounded<F>(t);
  if (!v) return false;
  out = *v;
  return true;
}

bool bool_field(const dom::element& obj, const char* key, bool def = false) {
  bool b = def;
  if (obj[key].get(b) != sj::SUCCESS) return def;
  return b;
}

std::int64_t int_field(const dom::element& obj, const char* key, std::int64_t def = 0) {
  std::int64_t v = def;
  if (obj[key].get(v) == sj::SUCCESS) return v;
  double d = 0.0;
  if (obj[key].get(d) == sj::SUCCESS) return static_cast<std::int64_t>(d);
  std::string_view s;
  if (obj[key].get(s) == sj::SUCCESS) {
    if (const auto p = parse_int64(s)) return *p;
  }
  return def;
}

double double_field(const dom::element& obj, const char* key, double def = 0.0) {
  const std::string t = field_text(obj, key);
  if (t.empty()) return def;
  double d = def;
  const auto r = std::from_chars(t.data(), t.data() + t.size(), d);
  return r.ec == std::errc{} ? d : def;
}

// A time Gate gives in seconds, as a JSON number with a fractional part or as text, in ms.
std::int64_t seconds_to_ms(const dom::element& obj, const char* key) {
  double d = 0.0;
  if (obj[key].get(d) == sj::SUCCESS) return static_cast<std::int64_t>(std::llround(d * 1000.0));
  std::int64_t i = 0;
  if (obj[key].get(i) == sj::SUCCESS) return i * 1000;
  const std::string t = field_text(obj, key);
  if (t.empty()) return 0;
  const auto r = std::from_chars(t.data(), t.data() + t.size(), d);
  return r.ec == std::errc{} ? static_cast<std::int64_t>(std::llround(d * 1000.0)) : 0;
}

std::string read_contract(const dom::element& e, ContractInfo& i) {
  std::string_view s;
  if (e["name"].get(s) != sj::SUCCESS) return "contracts: entry without name";
  i.name = std::string(s);
  if (e["type"].get(s) == sj::SUCCESS) i.type = std::string(s);
  if (e["status"].get(s) == sj::SUCCESS) i.status = std::string(s);
  if (!fixed_field(e, "quanto_multiplier", i.quanto_multiplier))
    return "contracts: bad quanto_multiplier for " + i.name;
  if (!fixed_field(e, "order_price_round", i.tick))
    return "contracts: bad order_price_round for " + i.name;
  static_cast<void>(fixed_field(e, "order_size_min", i.min_size));
  static_cast<void>(fixed_field(e, "order_size_max", i.max_size));
  i.funding_interval_s = int_field(e, "funding_interval");
  i.funding_next_apply_s = int_field(e, "funding_next_apply");
  i.maker_fee_rate = double_field(e, "maker_fee_rate");
  i.taker_fee_rate = double_field(e, "taker_fee_rate");
  i.in_delisting = bool_field(e, "in_delisting");
  return {};
}

}  // namespace

bool parse_signed_size(std::string_view text, Qty& magnitude, bool& positive) noexcept {
  if (text.empty()) return false;
  positive = text.front() != '-';
  const std::string_view mag = positive ? text : text.substr(1);
  const auto q = parse_rounded<Qty>(mag);
  if (!q) return false;
  magnitude = *q;
  return true;
}

std::string decode_contracts(std::string_view json, std::vector<ContractInfo>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "contracts: invalid JSON";
  dom::array arr;
  if (root.get(arr) != sj::SUCCESS) {
    std::string label;
    std::string msg;
    if (decode_error(json, label, msg)) return "contracts: " + label + " " + msg;
    return "contracts: not an array";
  }
  for (dom::element e : arr) {
    ContractInfo i;
    if (const std::string err = read_contract(e, i); !err.empty()) return err;
    out.push_back(std::move(i));
  }
  return {};
}

std::string decode_contract(std::string_view json, ContractInfo& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "contract: invalid JSON";
  std::string label;
  std::string msg;
  if (root.is_object() && root["label"].error() == sj::SUCCESS && decode_error(json, label, msg))
    return "contract: " + label + " " + msg;
  return read_contract(root, out);
}

std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "time: invalid JSON";
  if (root["server_time"].get(server_time_ms) == sj::SUCCESS) return {};
  double current = 0.0;
  if (root["current"].get(current) == sj::SUCCESS && current > 0.0) {
    server_time_ms = static_cast<std::int64_t>(std::llround(current * 1000.0));
    return {};
  }
  std::int64_t cur_i = 0;
  if (root["current"].get(cur_i) == sj::SUCCESS && cur_i > 0) {
    server_time_ms = cur_i * 1000;
    return {};
  }
  return "time: missing current/server_time";
}

std::string decode_account(std::string_view json, AccountInfo& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "accounts: invalid JSON";
  std::string label;
  std::string msg;
  if (root["label"].error() == sj::SUCCESS && decode_error(json, label, msg))
    return "accounts: " + label + " " + msg;
  out.user = field_text(root, "user");
  std::string_view s;
  if (root["currency"].get(s) == sj::SUCCESS) out.currency = std::string(s);
  Notional total{};
  Notional available{};
  Notional order_margin{};
  Notional position_margin{};
  Notional upl{};
  Notional mm{};
  if (!fixed_field(root, "total", total) || !fixed_field(root, "available", available))
    return "accounts: missing total/available";
  static_cast<void>(fixed_field(root, "order_margin", order_margin));
  static_cast<void>(fixed_field(root, "position_margin", position_margin));
  static_cast<void>(fixed_field(root, "unrealised_pnl", upl));
  static_cast<void>(fixed_field(root, "maintenance_margin", mm));
  out.fields.free = available;
  out.fields.locked = order_margin + position_margin;
  out.fields.total = total;
  out.fields.equity = total + upl;
  out.fields.maintenance = mm;
  out.in_dual_mode = bool_field(root, "in_dual_mode");
  return {};
}

std::string decode_positions(std::string_view json, std::vector<PositionRecord>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "positions: invalid JSON";
  dom::array arr;
  if (root.get(arr) != sj::SUCCESS) {
    // GET /positions/{contract} answers one object; an error body is one too.
    std::string label;
    std::string msg;
    if (root["label"].error() == sj::SUCCESS && decode_error(json, label, msg))
      return "positions: " + label + " " + msg;
    PositionRecord p;
    std::string_view s;
    if (root["contract"].get(s) != sj::SUCCESS) return "positions: not an array";
    p.contract = std::string(s);
    if (root["mode"].get(s) == sj::SUCCESS) p.mode = std::string(s);
    Qty mag{};
    bool pos = true;
    if (!parse_signed_size(field_text(root, "size"), mag, pos))
      return "positions: bad size for " + p.contract;
    p.size = pos ? mag : -mag;
    static_cast<void>(fixed_field(root, "entry_price", p.entry_price));
    out.push_back(std::move(p));
    return {};
  }
  for (dom::element e : arr) {
    PositionRecord p;
    std::string_view s;
    if (e["contract"].get(s) != sj::SUCCESS) return "positions: entry without contract";
    p.contract = std::string(s);
    if (e["mode"].get(s) == sj::SUCCESS) p.mode = std::string(s);
    Qty mag{};
    bool pos = true;
    if (!parse_signed_size(field_text(e, "size"), mag, pos))
      return "positions: bad size for " + p.contract;
    p.size = pos ? mag : -mag;
    static_cast<void>(fixed_field(e, "entry_price", p.entry_price));
    out.push_back(std::move(p));
  }
  return {};
}

std::string decode_open_orders(std::string_view json, std::vector<OpenOrderRecord>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "orders: invalid JSON";
  dom::array arr;
  if (root.get(arr) != sj::SUCCESS) {
    std::string label;
    std::string msg;
    if (decode_error(json, label, msg)) return "orders: " + label + " " + msg;
    return "orders: not an array";
  }
  for (dom::element e : arr) {
    OpenOrderRecord o;
    std::string_view s;
    if (e["contract"].get(s) != sj::SUCCESS) return "orders: entry without contract";
    o.contract = std::string(s);
    o.id = field_text(e, "id");
    if (o.id.empty()) return "orders: " + o.contract + " entry without id";
    if (e["text"].get(s) == sj::SUCCESS) o.text = std::string(s);
    if (e["status"].get(s) == sj::SUCCESS) o.status = std::string(s);
    Qty mag{};
    bool pos = true;
    if (!parse_signed_size(field_text(e, "size"), mag, pos)) return "orders: bad size";
    o.size = pos ? mag : -mag;
    if (!parse_signed_size(field_text(e, "left"), mag, pos)) return "orders: bad left";
    o.left = pos ? mag : -mag;
    if (!fixed_field(e, "price", o.price)) return "orders: bad price";
    o.create_time_ms = seconds_to_ms(e, "create_time");
    out.push_back(std::move(o));
  }
  return {};
}

std::string decode_my_trades(std::string_view json, std::vector<TradeRecord>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "my_trades: invalid JSON";
  dom::array arr;
  if (root.get(arr) != sj::SUCCESS) {
    std::string label;
    std::string msg;
    if (decode_error(json, label, msg)) return "my_trades: " + label + " " + msg;
    return "my_trades: not an array";
  }
  for (dom::element e : arr) {
    TradeRecord t;
    std::string_view s;
    if (e["contract"].get(s) != sj::SUCCESS) return "my_trades: entry without contract";
    t.contract = std::string(s);
    t.id = field_text(e, "id");
    t.order_id = field_text(e, "order_id");
    if (t.id.empty() || t.order_id.empty()) return "my_trades: entry without id/order_id";
    if (e["text"].get(s) == sj::SUCCESS) t.text = std::string(s);
    if (e["role"].get(s) == sj::SUCCESS) t.maker = s == "maker";
    Qty mag{};
    bool pos = true;
    if (!parse_signed_size(field_text(e, "size"), mag, pos)) return "my_trades: bad size";
    t.size = pos ? mag : -mag;
    if (!fixed_field(e, "price", t.price)) return "my_trades: bad price";
    static_cast<void>(fixed_field(e, "fee", t.fee));
    t.time_ms = seconds_to_ms(e, "create_time");
    out.push_back(std::move(t));
  }
  return {};
}

std::string decode_fees(std::string_view json, std::vector<std::pair<std::string, FeeRates>>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "fee: invalid JSON";
  dom::object obj;
  if (root.get(obj) != sj::SUCCESS) return "fee: not an object";
  std::string label;
  std::string msg;
  if (root["label"].error() == sj::SUCCESS && decode_error(json, label, msg))
    return "fee: " + label + " " + msg;
  for (dom::key_value_pair kv : obj) {
    FeeRates f;
    f.maker = double_field(kv.value, "maker_fee");
    f.taker = double_field(kv.value, "taker_fee");
    out.emplace_back(std::string(kv.key), f);
  }
  return {};
}

bool decode_error(std::string_view json, std::string& label, std::string& message) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return false;
  std::string_view s;
  if (root["label"].get(s) != sj::SUCCESS) return false;
  label = std::string(s);
  message = root["message"].get(s) == sj::SUCCESS ? std::string(s) : std::string{};
  return true;
}

}  // namespace fastmm::venues::gate
