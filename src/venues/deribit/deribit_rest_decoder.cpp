#include "fastmm/venues/deribit/deribit_rest_decoder.hpp"

#include "fastmm/venues/deribit/deribit_json.hpp"

#include <simdjson.h>

namespace fastmm::venues::deribit {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

namespace {

void str_field(od::object& o, std::string_view key, std::string& out) {
  std::string_view s;
  if (o[key].get_string().get(s) == sj::SUCCESS) out.assign(s);
}

template <class F>
bool fixed_field(od::object& o, std::string_view key, F& out) {
  std::string_view tok;
  if (o[key].raw_json_token().get(tok) != sj::SUCCESS) return false;
  const auto v = json_fixed<F>(tok);
  if (!v) return false;
  out = *v;
  return true;
}

}  // namespace

std::string decode_instruments(std::string_view json, std::vector<InstrumentInfo>& out) {
  od::parser parser;
  const sj::padded_string padded(json);
  od::document doc;
  od::object root;
  if (parser.iterate(padded).get(doc) != sj::SUCCESS || doc.get_object().get(root) != sj::SUCCESS)
    return "get_instruments: invalid JSON";
  {
    od::object err;
    if (root["error"].get_object().get(err) == sj::SUCCESS) {
      std::int64_t code = 0;
      std::string msg;
      if (err["code"].get_int64().get(code) != sj::SUCCESS) code = 0;
      str_field(err, "message", msg);
      return "get_instruments: error " + std::to_string(code) + " " + msg;
    }
  }
  root.reset();
  od::array list;
  if (root["result"].get_array().get(list) != sj::SUCCESS) return "get_instruments: missing result";
  for (auto item : list) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return "get_instruments: entry is not an object";
    InstrumentInfo i;
    str_field(o, "instrument_name", i.name);
    if (i.name.empty()) return "get_instruments: entry without instrument_name";
    str_field(o, "kind", i.kind);
    str_field(o, "option_type", i.option_type);
    str_field(o, "instrument_type", i.instrument_type);
    str_field(o, "settlement_period", i.settlement_period);
    str_field(o, "base_currency", i.base_currency);
    str_field(o, "quote_currency", i.quote_currency);
    str_field(o, "counter_currency", i.counter_currency);
    str_field(o, "settlement_currency", i.settlement_currency);
    str_field(o, "state", i.state);
    bool active = false;
    if (o["is_active"].get_bool().get(active) == sj::SUCCESS) i.is_active = active;
    if (!fixed_field(o, "tick_size", i.tick)) return "get_instruments: bad tick_size for " + i.name;
    if (!fixed_field(o, "contract_size", i.contract_size))
      return "get_instruments: bad contract_size for " + i.name;
    if (!fixed_field(o, "min_trade_amount", i.min_trade_amount))
      return "get_instruments: bad min_trade_amount for " + i.name;
    static_cast<void>(fixed_field(o, "strike", i.strike));  // options only
    std::int64_t exp = 0;
    if (o["expiration_timestamp"].get_int64().get(exp) == sj::SUCCESS) i.expiration_ms = exp;
    double c = 0.0;
    if (o["maker_commission"].get_double().get(c) == sj::SUCCESS) i.maker_commission = c;
    if (o["taker_commission"].get_double().get(c) == sj::SUCCESS) i.taker_commission = c;
    od::array steps;
    if (o["tick_size_steps"].get_array().get(steps) == sj::SUCCESS) {
      for (auto st : steps) {
        od::object so;
        if (st.get_object().get(so) != sj::SUCCESS)
          return "get_instruments: bad tick_size_steps for " + i.name;
        TickStep step;
        if (!fixed_field(so, "above_price", step.above_price) ||
            !fixed_field(so, "tick_size", step.tick))
          return "get_instruments: bad tick_size_steps for " + i.name;
        i.tick_steps.push_back(step);
      }
    }
    out.push_back(std::move(i));
  }
  return {};
}

std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms) {
  od::parser parser;
  const sj::padded_string padded(json);
  od::document doc;
  od::object root;
  if (parser.iterate(padded).get(doc) != sj::SUCCESS || doc.get_object().get(root) != sj::SUCCESS)
    return "get_time: invalid JSON";
  std::int64_t ms = 0;
  if (root["result"].get_int64().get(ms) != sj::SUCCESS) return "get_time: missing result";
  server_time_ms = ms;
  return {};
}

bool decode_envelope(std::string_view json, RpcEnvelope& out) {
  out = RpcEnvelope{};
  od::parser parser;
  const sj::padded_string padded(json);
  od::document doc;
  od::object root;
  if (parser.iterate(padded).get(doc) != sj::SUCCESS || doc.get_object().get(root) != sj::SUCCESS)
    return false;
  od::object err;
  if (root["error"].get_object().get(err) == sj::SUCCESS) {
    std::int64_t code = 0;
    if (err["code"].get_int64().get(code) != sj::SUCCESS) return false;
    out.error_code = code;
    str_field(err, "message", out.message);
    od::object data;
    if (err["data"].get_object().get(data) == sj::SUCCESS) str_field(data, "reason", out.reason);
    return true;
  }
  root.reset();
  od::value result;
  if (root["result"].get(result) != sj::SUCCESS) return false;
  out.has_result = true;
  return true;
}

}  // namespace fastmm::venues::deribit
