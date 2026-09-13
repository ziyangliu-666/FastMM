#include "fastmm/venues/bybit/bybit_rest_decoder.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

namespace fastmm::venues::bybit {

namespace sj = simdjson;
namespace dom = simdjson::dom;

namespace {
template <class F>
bool fixed_field(const dom::element& obj, const char* key, F& out) {
  std::string_view s;
  if (obj[key].get(s) != sj::SUCCESS) return false;
  const auto v = parse_fixed<F>(s);
  if (!v) return false;
  out = *v;
  return true;
}
}  // namespace

std::string decode_instruments(std::string_view json, std::vector<InstrumentInfo>& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "instruments-info: invalid JSON";
  std::int64_t code = -1;
  if (root["retCode"].get(code) != sj::SUCCESS) return "instruments-info: missing retCode";
  if (code != 0) {
    std::string_view msg;
    if (root["retMsg"].get(msg) != sj::SUCCESS) msg = {};
    return "instruments-info: retCode " + std::to_string(code) + " " + std::string(msg);
  }
  dom::array list;
  if (root["result"]["list"].get(list) != sj::SUCCESS)
    return "instruments-info: missing result.list";
  for (dom::element e : list) {
    InstrumentInfo i;
    std::string_view s;
    if (e["symbol"].get(s) != sj::SUCCESS) return "instruments-info: entry without symbol";
    i.symbol = std::string(s);
    if (e["baseCoin"].get(s) == sj::SUCCESS) i.base_coin = std::string(s);
    if (e["quoteCoin"].get(s) == sj::SUCCESS) i.quote_coin = std::string(s);
    if (e["status"].get(s) == sj::SUCCESS) i.status = std::string(s);
    dom::element lot;
    dom::element price;
    if (e["lotSizeFilter"].get(lot) != sj::SUCCESS || e["priceFilter"].get(price) != sj::SUCCESS)
      return "instruments-info: " + i.symbol + " lacks lotSizeFilter/priceFilter";
    if (!fixed_field(price, "tickSize", i.tick))
      return "instruments-info: bad tickSize for " + i.symbol;
    if (!fixed_field(lot, "basePrecision", i.base_precision))
      return "instruments-info: bad basePrecision for " + i.symbol;
    static_cast<void>(fixed_field(lot, "minOrderQty", i.min_qty));
    static_cast<void>(fixed_field(lot, "maxOrderQty", i.max_qty));
    static_cast<void>(fixed_field(lot, "minOrderAmt", i.min_amount));
    static_cast<void>(fixed_field(lot, "maxOrderAmt", i.max_amount));
    out.push_back(std::move(i));
  }
  return {};
}

std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "time: invalid JSON";
  std::string_view nano;
  if (root["result"]["timeNano"].get(nano) == sj::SUCCESS) {
    if (const auto v = parse_int64(nano)) {
      server_time_ms = *v / 1'000'000;
      return {};
    }
  }
  if (root["time"].get(server_time_ms) == sj::SUCCESS) return {};
  return "time: missing result.timeNano/time";
}

bool decode_envelope(std::string_view json, int& ret_code, std::string& ret_msg) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return false;
  std::int64_t c = 0;
  if (root["retCode"].get(c) != sj::SUCCESS) return false;
  ret_code = static_cast<int>(c);
  std::string_view m;
  ret_msg = root["retMsg"].get(m) == sj::SUCCESS ? std::string(m) : std::string{};
  return true;
}

}  // namespace fastmm::venues::bybit
