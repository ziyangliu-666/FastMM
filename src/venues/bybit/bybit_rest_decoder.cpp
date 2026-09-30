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
    if (e["settleCoin"].get(s) == sj::SUCCESS) i.settle_coin = std::string(s);
    if (e["contractType"].get(s) == sj::SUCCESS) i.contract_type = std::string(s);
    if (e["fundingInterval"].get(i.funding_interval_min) != sj::SUCCESS) i.funding_interval_min = 0;
    dom::element lot;
    dom::element price;
    if (e["lotSizeFilter"].get(lot) != sj::SUCCESS || e["priceFilter"].get(price) != sj::SUCCESS)
      return "instruments-info: " + i.symbol + " lacks lotSizeFilter/priceFilter";
    if (!fixed_field(price, "tickSize", i.tick))
      return "instruments-info: bad tickSize for " + i.symbol;
    // Spot states the quantity step as basePrecision, derivatives as qtyStep.
    if (!fixed_field(lot, "basePrecision", i.base_precision) &&
        !fixed_field(lot, "qtyStep", i.base_precision))
      return "instruments-info: bad basePrecision/qtyStep for " + i.symbol;
    static_cast<void>(fixed_field(lot, "minOrderQty", i.min_qty));
    static_cast<void>(fixed_field(lot, "maxOrderQty", i.max_qty));
    if (!fixed_field(lot, "minOrderAmt", i.min_amount))
      static_cast<void>(fixed_field(lot, "minNotionalValue", i.min_amount));
    static_cast<void>(fixed_field(lot, "maxOrderAmt", i.max_amount));
    out.push_back(std::move(i));
  }
  return {};
}

std::string decode_positions(std::string_view json,
                             std::vector<PositionRecord>& out,
                             std::string& next_cursor) {
  next_cursor.clear();
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "position/list: invalid JSON";
  std::int64_t code = -1;
  if (root["retCode"].get(code) != sj::SUCCESS) return "position/list: missing retCode";
  if (code != 0) {
    std::string_view msg;
    if (root["retMsg"].get(msg) != sj::SUCCESS) msg = {};
    return "position/list: retCode " + std::to_string(code) + " " + std::string(msg);
  }
  dom::array list;
  if (root["result"]["list"].get(list) != sj::SUCCESS) return "position/list: missing result.list";
  for (dom::element e : list) {
    PositionRecord p;
    std::string_view s;
    if (e["symbol"].get(s) != sj::SUCCESS) return "position/list: entry without symbol";
    p.symbol = std::string(s);
    std::int64_t idx = 0;
    if (e["positionIdx"].get(idx) != sj::SUCCESS)
      return "position/list: " + p.symbol + " without positionIdx";
    p.position_idx = static_cast<int>(idx);
    Qty size{};
    if (!fixed_field(e, "size", size)) return "position/list: bad size for " + p.symbol;
    std::string_view side;
    if (e["side"].get(side) != sj::SUCCESS) side = {};
    p.qty = side == "Sell" ? -size : size;
    // avgPrice is "0" (or "") for an empty position.
    std::string_view avg;
    if (e["avgPrice"].get(avg) == sj::SUCCESS) {
      if (const auto px = parse_avg_price(avg)) p.avg_px = *px;  // an average: may pass 8 decimals
    }
    out.push_back(std::move(p));
  }
  std::string_view cursor;
  if (root["result"]["nextPageCursor"].get(cursor) == sj::SUCCESS) next_cursor = cursor;
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

namespace {
// result of a reply with retCode 0; an error text otherwise.
std::string open_result(dom::parser& parser,
                        std::string_view json,
                        std::string_view what,
                        dom::element& root,
                        dom::element& result) {
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return std::string(what) + ": invalid JSON";
  std::int64_t code = -1;
  if (root["retCode"].get(code) != sj::SUCCESS) return std::string(what) + ": missing retCode";
  if (code != 0) {
    std::string_view msg;
    if (root["retMsg"].get(msg) != sj::SUCCESS) msg = {};
    return std::string(what) + ": retCode " + std::to_string(code) + " " + std::string(msg);
  }
  if (root["result"].get(result) != sj::SUCCESS) return std::string(what) + ": missing result";
  return {};
}

std::string_view view_of(const dom::element& e, const char* key) {
  std::string_view s;
  return e[key].get(s) == sj::SUCCESS ? s : std::string_view{};
}
}  // namespace

std::string decode_wallet_balance(std::string_view json, WalletBalance& out) {
  dom::parser parser;
  dom::element root;
  dom::element result;
  if (std::string err = open_result(parser, json, "wallet-balance", root, result); !err.empty())
    return err;
  if (root["time"].get(out.time_ms) != sj::SUCCESS) out.time_ms = 0;
  dom::array list;
  if (result["list"].get(list) != sj::SUCCESS) return "wallet-balance: missing result.list";
  for (dom::element e : list) {
    const BybitAccountFields a{view_of(e, "totalEquity"),
                               view_of(e, "totalWalletBalance"),
                               view_of(e, "totalAvailableBalance"),
                               view_of(e, "totalInitialMargin"),
                               view_of(e, "totalMaintenanceMargin")};
    if (!bybit_account_balance(a, out.account)) return "wallet-balance: bad account-level amount";
    dom::array coins;
    if (e["coin"].get(coins) != sj::SUCCESS) return "wallet-balance: missing coin";
    for (dom::element c : coins) {
      const BybitCoinFields f{view_of(c, "coin"),
                              view_of(c, "walletBalance"),
                              view_of(c, "locked"),
                              view_of(c, "equity"),
                              view_of(c, "totalOrderIM"),
                              view_of(c, "totalPositionIM"),
                              view_of(c, "totalPositionMM"),
                              view_of(c, "bonus"),
                              view_of(c, "spotBorrow")};
      if (f.coin.empty()) return "wallet-balance: entry without coin";
      WalletCoin w;
      w.coin = std::string(f.coin);
      if (!bybit_coin_balance(f, w.fields)) return "wallet-balance: bad amount for " + w.coin;
      out.coins.push_back(std::move(w));
    }
    return {};  // accountType UNIFIED: one account
  }
  return "wallet-balance: empty list";
}

std::string decode_margin_mode(std::string_view json, std::string& margin_mode) {
  dom::parser parser;
  dom::element root;
  dom::element result;
  if (std::string err = open_result(parser, json, "account info", root, result); !err.empty())
    return err;
  margin_mode = std::string(view_of(result, "marginMode"));
  if (margin_mode.empty()) return "account info: missing marginMode";
  return {};
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
