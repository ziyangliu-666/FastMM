#include "fastmm/venues/binance/binance_rest_decoder.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

namespace fastmm::venues::binance {

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

std::string decode_exchange_info(std::string_view json, ExchangeInfo& out) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "exchangeInfo: invalid JSON";
  {
    std::int64_t t = 0;
    if (root["serverTime"].get(t) == sj::SUCCESS) out.server_time_ms = t;
  }
  {
    dom::array rl;
    if (root["rateLimits"].get(rl) == sj::SUCCESS) {
      for (dom::element e : rl) {
        RateLimitRule r;
        std::string_view s;
        std::int64_t n = 0;
        if (e["rateLimitType"].get(s) == sj::SUCCESS) r.type = std::string(s);
        if (e["interval"].get(s) == sj::SUCCESS) r.interval = std::string(s);
        if (e["intervalNum"].get(n) == sj::SUCCESS) r.interval_num = static_cast<int>(n);
        if (e["limit"].get(n) == sj::SUCCESS) r.limit = n;
        out.rate_limits.push_back(std::move(r));
      }
    }
  }
  dom::array symbols;
  if (root["symbols"].get(symbols) != sj::SUCCESS) return "exchangeInfo: missing symbols[]";
  for (dom::element e : symbols) {
    SymbolFilters f;
    std::string_view s;
    if (e["symbol"].get(s) != sj::SUCCESS) return "exchangeInfo: symbol without name";
    f.symbol = std::string(s);
    if (e["status"].get(s) == sj::SUCCESS) f.status = std::string(s);
    if (e["baseAsset"].get(s) == sj::SUCCESS) f.base_asset = std::string(s);
    if (e["quoteAsset"].get(s) == sj::SUCCESS) f.quote_asset = std::string(s);
    dom::array types;
    if (e["orderTypes"].get(types) == sj::SUCCESS) {
      f.post_only_allowed = false;
      for (dom::element t : types) {
        if (t.get(s) == sj::SUCCESS && s == "LIMIT_MAKER") f.post_only_allowed = true;
      }
    }
    dom::array filters;
    if (e["filters"].get(filters) != sj::SUCCESS)
      return "exchangeInfo: " + f.symbol + " without filters";
    bool have_price = false;
    bool have_lot = false;
    for (dom::element flt : filters) {
      std::string_view type;
      if (flt["filterType"].get(type) != sj::SUCCESS) continue;
      if (type == "PRICE_FILTER") {
        if (!fixed_field(flt, "tickSize", f.tick))
          return "exchangeInfo: bad PRICE_FILTER for " + f.symbol;
        have_price = true;
      } else if (type == "LOT_SIZE") {
        if (!fixed_field(flt, "stepSize", f.step) || !fixed_field(flt, "minQty", f.min_qty) ||
            !fixed_field(flt, "maxQty", f.max_qty))
          return "exchangeInfo: bad LOT_SIZE for " + f.symbol;
        have_lot = true;
      } else if (type == "NOTIONAL") {
        if (!fixed_field(flt, "minNotional", f.min_notional))
          return "exchangeInfo: bad NOTIONAL for " + f.symbol;
        static_cast<void>(fixed_field(flt, "maxNotional", f.max_notional));
      } else if (type == "MIN_NOTIONAL") {
        // Older symbols/sims expose MIN_NOTIONAL instead of NOTIONAL (filters.md lists both).
        if (f.min_notional.is_zero() && !fixed_field(flt, "minNotional", f.min_notional))
          return "exchangeInfo: bad MIN_NOTIONAL for " + f.symbol;
      }
    }
    if (!have_price || !have_lot)
      return "exchangeInfo: " + f.symbol + " lacks PRICE_FILTER/LOT_SIZE";
    out.symbols.push_back(std::move(f));
  }
  return {};
}

std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return "time: invalid JSON";
  if (root["serverTime"].get(server_time_ms) != sj::SUCCESS) return "time: missing serverTime";
  return {};
}

bool decode_rest_error(std::string_view json, int& code, std::string& msg) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return false;
  std::int64_t c = 0;
  if (root["code"].get(c) != sj::SUCCESS) return false;
  code = static_cast<int>(c);
  std::string_view m;
  msg = root["msg"].get(m) == sj::SUCCESS ? std::string(m) : std::string{};
  return true;
}

std::string decode_listen_key(std::string_view json, std::string& listen_key) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "listenKey: invalid JSON";
  std::string_view k;
  if (root["listenKey"].get(k) != sj::SUCCESS) return "listenKey: missing field";
  listen_key = std::string(k);
  return {};
}

}  // namespace fastmm::venues::binance
