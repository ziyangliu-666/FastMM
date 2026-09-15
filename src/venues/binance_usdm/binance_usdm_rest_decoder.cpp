#include "fastmm/venues/binance_usdm/binance_usdm_rest_decoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/symbology.hpp"

#include <simdjson.h>

namespace fastmm::venues::binance_usdm {

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

std::string string_field(const dom::element& obj, const char* key) {
  std::string_view s;
  return obj[key].get(s) == sj::SUCCESS ? std::string(s) : std::string{};
}

bool is_wanted(std::string_view symbol, std::span<const std::string> wanted) noexcept {
  if (wanted.empty()) return true;
  for (const std::string& w : wanted) {
    if (iequals_symbol(w, symbol)) return true;
  }
  return false;
}

// One symbols[] entry. Empty string on success.
std::string decode_symbol(const dom::element& e, SymbolInfo& f) {
  f.status = string_field(e, "status");
  f.contract_type = string_field(e, "contractType");
  f.base_asset = string_field(e, "baseAsset");
  f.quote_asset = string_field(e, "quoteAsset");
  f.margin_asset = string_field(e, "marginAsset");
  dom::array tifs;
  if (e["timeInForce"].get(tifs) == sj::SUCCESS) {
    f.gtx_allowed = false;
    for (dom::element t : tifs) {
      std::string_view s;
      if (t.get(s) == sj::SUCCESS && s == "GTX") f.gtx_allowed = true;
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
    } else if (type == "MIN_NOTIONAL") {
      // USDⓈ-M names the field `notional` (Spot: `minNotional`).
      if (!fixed_field(flt, "notional", f.min_notional))
        return "exchangeInfo: bad MIN_NOTIONAL for " + f.symbol;
    }
  }
  if (!have_price || !have_lot) return "exchangeInfo: " + f.symbol + " lacks PRICE_FILTER/LOT_SIZE";
  return {};
}

}  // namespace

std::string decode_exchange_info(std::string_view json,
                                 ExchangeInfo& out,
                                 std::span<const std::string> wanted) {
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
        binance::RateLimitRule r;
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
    std::string_view name;
    if (e["symbol"].get(name) != sj::SUCCESS) return "exchangeInfo: symbol without name";
    if (!is_wanted(name, wanted)) continue;
    SymbolInfo f;
    f.symbol = std::string(name);
    if (std::string err = decode_symbol(e, f); !err.empty()) return err;
    out.symbols.push_back(std::move(f));
  }
  return {};
}

std::string decode_position_risk(std::string_view json, std::vector<PositionRecord>& out) {
  dom::parser parser;
  dom::array arr;
  if (parser.parse(sj::padded_string(json)).get(arr) != sj::SUCCESS)
    return "positionRisk: expected an array";
  for (dom::element e : arr) {
    PositionRecord p;
    p.symbol = string_field(e, "symbol");
    p.position_side = string_field(e, "positionSide");
    if (p.symbol.empty() || !fixed_field(e, "positionAmt", p.qty))
      return "positionRisk: entry without symbol or positionAmt";
    static_cast<void>(fixed_field(e, "entryPrice", p.entry_price));
    out.push_back(std::move(p));
  }
  return {};
}

std::string decode_position_mode(std::string_view json, bool& dual_side_position) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "positionSide/dual: invalid JSON";
  if (root["dualSidePosition"].get(dual_side_position) != sj::SUCCESS)
    return "positionSide/dual: missing dualSidePosition";
  return {};
}

std::string decode_symbol_config(std::string_view json, std::vector<SymbolConfig>& out) {
  dom::parser parser;
  dom::array arr;
  if (parser.parse(sj::padded_string(json)).get(arr) != sj::SUCCESS)
    return "symbolConfig: expected an array";
  for (dom::element e : arr) {
    SymbolConfig c;
    c.symbol = string_field(e, "symbol");
    c.margin_type = string_field(e, "marginType");
    std::int64_t lev = 0;
    if (e["leverage"].get(lev) == sj::SUCCESS) c.leverage = lev;
    out.push_back(std::move(c));
  }
  return {};
}

std::string decode_balance(std::string_view json, std::vector<BalanceRecord>& out) {
  dom::parser parser;
  dom::array arr;
  if (parser.parse(sj::padded_string(json)).get(arr) != sj::SUCCESS)
    return "balance: expected an array";
  for (dom::element e : arr) {
    BalanceRecord b;
    b.asset = string_field(e, "asset");
    static_cast<void>(fixed_field(e, "balance", b.balance));
    static_cast<void>(fixed_field(e, "availableBalance", b.available));
    out.push_back(std::move(b));
  }
  return {};
}

}  // namespace fastmm::venues::binance_usdm
