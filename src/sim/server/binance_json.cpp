#include "fastmm/sim/server/binance_json.hpp"

#include <array>
#include <charconv>

namespace fastmm::sim::server {

std::string_view to_text(BinanceOrderType t) noexcept {
  switch (t) {
    case BinanceOrderType::Limit:
      return "LIMIT";
    case BinanceOrderType::LimitMaker:
      return "LIMIT_MAKER";
    case BinanceOrderType::Market:
      return "MARKET";
  }
  return "LIMIT";
}

std::string_view to_text(BinanceOrderStatus s) noexcept {
  switch (s) {
    case BinanceOrderStatus::New:
      return "NEW";
    case BinanceOrderStatus::PartiallyFilled:
      return "PARTIALLY_FILLED";
    case BinanceOrderStatus::Filled:
      return "FILLED";
    case BinanceOrderStatus::Canceled:
      return "CANCELED";
    case BinanceOrderStatus::Rejected:
      return "REJECTED";
    case BinanceOrderStatus::Expired:
      return "EXPIRED";
    case BinanceOrderStatus::ExpiredInMatch:
      return "EXPIRED_IN_MATCH";
  }
  return "NEW";
}

std::string_view to_text(ExecType x) noexcept {
  switch (x) {
    case ExecType::New:
      return "NEW";
    case ExecType::Canceled:
      return "CANCELED";
    case ExecType::Replaced:
      return "REPLACED";
    case ExecType::Rejected:
      return "REJECTED";
    case ExecType::Trade:
      return "TRADE";
    case ExecType::Expired:
      return "EXPIRED";
    case ExecType::TradePrevention:
      return "TRADE_PREVENTION";
  }
  return "NEW";
}

std::string_view side_text(Side s) noexcept {
  return s == Side::Buy ? "BUY" : "SELL";
}

std::string_view tif_text(TimeInForce t) noexcept {
  switch (t) {
    case TimeInForce::Ioc:
      return "IOC";
    case TimeInForce::Fok:
      return "FOK";
    case TimeInForce::Gtc:
    case TimeInForce::Day:
      return "GTC";
  }
  return "GTC";
}

bool is_terminal(BinanceOrderStatus s) noexcept {
  return s != BinanceOrderStatus::New && s != BinanceOrderStatus::PartiallyFilled;
}

// ---- primitives -----------------------------------------------------------------------------

void append_int(std::string& out, std::int64_t v) {
  std::array<char, 24> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  out.append(buf.data(), static_cast<std::size_t>(res.ptr - buf.data()));
}

void append_uint(std::string& out, std::uint64_t v) {
  std::array<char, 24> buf{};
  const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  out.append(buf.data(), static_cast<std::size_t>(res.ptr - buf.data()));
}

void append_decimal_raw(std::string& out, std::int64_t raw) {
  std::uint64_t mag = 0;
  if (raw < 0) {
    out.push_back('-');
    mag = 0 - static_cast<std::uint64_t>(raw);
  } else {
    mag = static_cast<std::uint64_t>(raw);
  }
  const auto scale = static_cast<std::uint64_t>(kFixedScale);
  append_uint(out, mag / scale);
  out.push_back('.');
  std::uint64_t frac = mag % scale;
  std::array<char, 8> digits{};
  for (std::size_t i = digits.size(); i-- > 0;) {
    digits[i] = static_cast<char>('0' + static_cast<int>(frac % 10));
    frac /= 10;
  }
  out.append(digits.data(), digits.size());
}

void append_json_string(std::string& out, std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default: {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(uc >> 4U) & 0x0FU]);
          out.push_back(kHex[uc & 0x0FU]);
        } else {
          out.push_back(c);
        }
      }
    }
  }
  out.push_back('"');
}

namespace {

void append_levels(std::string& out, std::span<const Level> levels) {
  out.push_back('[');
  bool first = true;
  for (const Level& l : levels) {
    if (!first) out.push_back(',');
    first = false;
    out += "[\"";
    append_decimal(out, l.price);
    out += "\",\"";
    append_decimal(out, l.qty);
    out += "\"]";
  }
  out.push_back(']');
}

void order_core(JsonObjectWriter& w, const OrderView& o) {
  w.str("symbol", o.symbol).num("orderId", o.order_id).num("orderListId", -1);
}

void order_prices(JsonObjectWriter& w, const OrderView& o) {
  w.dec("price", o.type == BinanceOrderType::Market ? Price{} : o.price)
      .dec("origQty", o.orig_qty)
      .dec("executedQty", o.executed_qty);
}

}  // namespace

// ---- orders -----------------------------------------------------------------------------------

void append_order_object(std::string& out, const OrderView& o) {
  JsonObjectWriter w(out);
  order_core(w, o);
  w.str("clientOrderId", o.client_order_id);
  order_prices(w, o);
  w.dec("cummulativeQuoteQty", o.cum_quote)
      .str("status", to_text(o.status))
      .str("timeInForce", tif_text(o.tif))
      .str("type", to_text(o.type))
      .str("side", side_text(o.side))
      .dec("stopPrice", Price{})
      .dec("icebergQty", Qty{})
      .num("time", o.time_ms)
      .num("updateTime", o.update_ms)
      .boolean("isWorking", true)
      .num("workingTime", o.time_ms)
      .dec("origQuoteOrderQty", Notional{})
      .str("selfTradePreventionMode", "NONE");
  w.close();
}

void append_ack_result(std::string& out, const OrderView& o, std::int64_t transact_ms) {
  JsonObjectWriter w(out);
  order_core(w, o);
  w.str("clientOrderId", o.client_order_id).num("transactTime", transact_ms);
  w.close();
}

void append_result_result(std::string& out,
                          const OrderView& o,
                          std::int64_t transact_ms,
                          std::span<const FillView> fills,
                          bool full) {
  JsonObjectWriter w(out);
  order_core(w, o);
  w.str("clientOrderId", o.client_order_id).num("transactTime", transact_ms);
  order_prices(w, o);
  w.dec("origQuoteOrderQty", Notional{})
      .dec("cummulativeQuoteQty", o.cum_quote)
      .str("status", to_text(o.status))
      .str("timeInForce", tif_text(o.tif))
      .str("type", to_text(o.type))
      .str("side", side_text(o.side))
      .num("workingTime", transact_ms)
      .str("selfTradePreventionMode", "NONE");
  if (full) {
    std::string arr = "[";
    bool first = true;
    for (const FillView& f : fills) {
      if (!first) arr.push_back(',');
      first = false;
      JsonObjectWriter fw(arr);
      fw.dec("price", f.price)
          .dec("qty", f.qty)
          .dec("commission", f.commission)
          .str("commissionAsset", f.commission_asset)
          .unum("tradeId", f.trade_id);
      fw.close();
    }
    arr.push_back(']');
    w.raw("fills", arr);
  }
  w.close();
}

void append_cancel_result(std::string& out,
                          const OrderView& o,
                          std::string_view cancel_client_order_id,
                          std::int64_t transact_ms) {
  JsonObjectWriter w(out);
  w.str("symbol", o.symbol)
      .str("origClientOrderId", o.client_order_id)
      .num("orderId", o.order_id)
      .num("orderListId", -1)
      .str("clientOrderId", cancel_client_order_id)
      .num("transactTime", transact_ms);
  order_prices(w, o);
  w.dec("origQuoteOrderQty", Notional{})
      .dec("cummulativeQuoteQty", o.cum_quote)
      .str("status", to_text(o.status))
      .str("timeInForce", tif_text(o.tif))
      .str("type", to_text(o.type))
      .str("side", side_text(o.side))
      .str("selfTradePreventionMode", "NONE");
  w.close();
}

void append_amend_result(std::string& out,
                         const OrderView& o,
                         std::string_view orig_client_order_id,
                         std::int64_t transact_ms,
                         std::int64_t execution_id) {
  std::string amended;
  {
    JsonObjectWriter a(amended);
    order_core(a, o);
    a.str("origClientOrderId", orig_client_order_id)
        .str("clientOrderId", o.client_order_id)
        .dec("price", o.price)
        .dec("qty", o.orig_qty)
        .dec("executedQty", o.executed_qty)
        .dec("preventedQty", Qty{})
        .dec("quoteOrderQty", Notional{})
        .dec("cumulativeQuoteQty", o.cum_quote)
        .str("status", to_text(o.status))
        .str("timeInForce", tif_text(o.tif))
        .str("type", to_text(o.type))
        .str("side", side_text(o.side))
        .num("workingTime", o.time_ms)
        .str("selfTradePreventionMode", "NONE");
    a.close();
  }
  JsonObjectWriter w(out);
  w.num("transactTime", transact_ms).num("executionId", execution_id).raw("amendedOrder", amended);
  w.close();
}

void append_error(std::string& out, int code, std::string_view msg) {
  JsonObjectWriter w(out);
  w.num("code", code).str("msg", msg);
  w.close();
}

void append_error_with_data(std::string& out,
                            int code,
                            std::string_view msg,
                            std::string_view data_json) {
  JsonObjectWriter w(out);
  w.num("code", code).str("msg", msg).raw("data", data_json);
  w.close();
}

// ---- user data events ---------------------------------------------------------------------------

void append_execution_report(std::string& out, const ExecReportView& r) {
  const OrderView& o = r.order;
  JsonObjectWriter w(out);
  w.str("e", "executionReport")
      .num("E", r.event_ms)
      .str("s", o.symbol)
      .str("c", o.client_order_id)
      .str("S", side_text(o.side))
      .str("o", to_text(o.type))
      .str("f", tif_text(o.tif))
      .dec("q", o.orig_qty)
      .dec("p", o.type == BinanceOrderType::Market ? Price{} : o.price)
      .dec("P", Price{})
      .dec("F", Qty{})
      .num("g", -1)
      .str("C", r.orig_client_order_id)
      .str("x", to_text(r.exec_type))
      .str("X", to_text(o.status))
      .str("r", r.reject_reason)
      .num("i", o.order_id)
      .dec("l", r.last_qty)
      .dec("z", o.executed_qty)
      .dec("L", r.last_price)
      .dec("n", r.commission);
  if (r.commission_asset.empty()) {
    w.null("N");
  } else {
    w.str("N", r.commission_asset);
  }
  w.num("T", r.transact_ms)
      .num("t", r.trade_id)
      .num("I", r.execution_id)
      .boolean("w", r.working)
      .boolean("m", r.maker)
      .boolean("M", r.trade_id >= 0)
      .num("O", o.time_ms)
      .dec("Z", o.cum_quote)
      .dec("Y", mul(r.last_price, r.last_qty))
      .dec("Q", Notional{})
      .num("W", o.time_ms)
      .str("V", "NONE");
  w.close();
}

void append_account_position(std::string& out,
                             std::int64_t event_ms,
                             std::int64_t update_ms,
                             std::span<const BalanceView> balances) {
  std::string arr = "[";
  bool first = true;
  for (const BalanceView& b : balances) {
    if (!first) arr.push_back(',');
    first = false;
    JsonObjectWriter bw(arr);
    bw.str("a", b.asset).dec("f", b.free).dec("l", b.locked);
    bw.close();
  }
  arr.push_back(']');
  JsonObjectWriter w(out);
  w.str("e", "outboundAccountPosition").num("E", event_ms).num("u", update_ms).raw("B", arr);
  w.close();
}

void append_account_info(std::string& out,
                         std::int64_t update_ms,
                         std::int64_t maker_bps,
                         std::int64_t taker_bps,
                         std::span<const BalanceView> balances) {
  std::string arr = "[";
  bool first = true;
  for (const BalanceView& b : balances) {
    if (!first) arr.push_back(',');
    first = false;
    JsonObjectWriter bw(arr);
    bw.str("asset", b.asset).dec("free", b.free).dec("locked", b.locked);
    bw.close();
  }
  arr.push_back(']');
  JsonObjectWriter w(out);
  w.num("makerCommission", maker_bps)
      .num("takerCommission", taker_bps)
      .num("buyerCommission", 0)
      .num("sellerCommission", 0)
      .boolean("canTrade", true)
      .boolean("canWithdraw", false)
      .boolean("canDeposit", false)
      .boolean("brokered", false)
      .boolean("requireSelfTradePrevention", false)
      .boolean("preventSor", false)
      .num("updateTime", update_ms)
      .str("accountType", "SPOT")
      .raw("balances", arr)
      .raw("permissions", "[\"SPOT\"]")
      .num("uid", 1);
  w.close();
}

void append_listen_key_expired(std::string& out, std::int64_t event_ms, std::string_view key) {
  JsonObjectWriter w(out);
  w.str("e", "listenKeyExpired").num("E", event_ms).str("listenKey", key);
  w.close();
}

// ---- market data ------------------------------------------------------------------------------

void append_depth_update(std::string& out,
                         std::int64_t event_ms,
                         std::string_view symbol,
                         std::uint64_t first_update_id,
                         std::uint64_t last_update_id,
                         std::span<const Level> bids,
                         std::span<const Level> asks) {
  out += "{\"e\":\"depthUpdate\",\"E\":";
  append_int(out, event_ms);
  out += ",\"s\":";
  append_json_string(out, symbol);
  out += ",\"U\":";
  append_uint(out, first_update_id);
  out += ",\"u\":";
  append_uint(out, last_update_id);
  out += ",\"b\":";
  append_levels(out, bids);
  out += ",\"a\":";
  append_levels(out, asks);
  out.push_back('}');
}

void append_book_ticker(
    std::string& out, std::uint64_t update_id, std::string_view symbol, Level bid, Level ask) {
  JsonObjectWriter w(out);
  w.unum("u", update_id)
      .str("s", symbol)
      .dec("b", bid.price)
      .dec("B", bid.qty)
      .dec("a", ask.price)
      .dec("A", ask.qty);
  w.close();
}

void append_trade(std::string& out,
                  std::int64_t event_ms,
                  std::string_view symbol,
                  std::uint64_t trade_id,
                  Price price,
                  Qty qty,
                  std::int64_t trade_ms,
                  bool buyer_is_maker) {
  JsonObjectWriter w(out);
  w.str("e", "trade")
      .num("E", event_ms)
      .str("s", symbol)
      .unum("t", trade_id)
      .dec("p", price)
      .dec("q", qty)
      .num("T", trade_ms)
      .boolean("m", buyer_is_maker)
      .boolean("M", true);
  w.close();
}

void append_depth_snapshot(std::string& out,
                           std::uint64_t last_update_id,
                           std::span<const Level> bids,
                           std::span<const Level> asks) {
  out += "{\"lastUpdateId\":";
  append_uint(out, last_update_id);
  out += ",\"bids\":";
  append_levels(out, bids);
  out += ",\"asks\":";
  append_levels(out, asks);
  out.push_back('}');
}

void append_rest_book_ticker(std::string& out, std::string_view symbol, Level bid, Level ask) {
  JsonObjectWriter w(out);
  w.str("symbol", symbol)
      .dec("bidPrice", bid.price)
      .dec("bidQty", bid.qty)
      .dec("askPrice", ask.price)
      .dec("askQty", ask.qty);
  w.close();
}

// ---- reference data -----------------------------------------------------------------------------

void append_rate_limits(std::string& out, std::span<const RateLimitView> limits) {
  out.push_back('[');
  bool first = true;
  for (const RateLimitView& l : limits) {
    if (!first) out.push_back(',');
    first = false;
    JsonObjectWriter w(out);
    w.str("rateLimitType", l.type)
        .str("interval", l.interval)
        .num("intervalNum", l.interval_num)
        .num("limit", l.limit);
    if (l.count >= 0) w.num("count", l.count);
    w.close();
  }
  out.push_back(']');
}

void append_exchange_info(std::string& out,
                          std::int64_t server_ms,
                          std::span<const RateLimitView> limits,
                          std::span<const SymbolInfoView> symbols) {
  std::string rl;
  append_rate_limits(rl, limits);
  std::string arr = "[";
  bool first = true;
  for (const SymbolInfoView& s : symbols) {
    if (!first) arr.push_back(',');
    first = false;
    std::string filters = "[";
    {
      JsonObjectWriter f(filters);
      f.str("filterType", "PRICE_FILTER")
          .dec("minPrice", s.tick)
          .dec("maxPrice", Price::from_int(1'000'000))
          .dec("tickSize", s.tick);
      f.close();
    }
    filters.push_back(',');
    {
      JsonObjectWriter f(filters);
      f.str("filterType", "LOT_SIZE")
          .dec("minQty", s.min_qty)
          .dec("maxQty", s.max_qty.is_positive() ? s.max_qty : Qty::from_int(9000))
          .dec("stepSize", s.lot);
      f.close();
    }
    filters.push_back(',');
    {
      JsonObjectWriter f(filters);
      f.str("filterType", "NOTIONAL")
          .dec("minNotional", s.min_notional)
          .boolean("applyMinToMarket", true)
          .dec("maxNotional",
               s.max_notional.is_positive() ? s.max_notional : Notional::from_int(9'000'000))
          .boolean("applyMaxToMarket", false)
          .num("avgPriceMins", 5);
      f.close();
    }
    filters.push_back(',');
    {
      JsonObjectWriter f(filters);
      f.str("filterType", "MAX_NUM_ORDERS").num("maxNumOrders", 200);
      f.close();
    }
    filters.push_back(']');
    JsonObjectWriter w(arr);
    w.str("symbol", s.symbol)
        .str("status", "TRADING")
        .str("baseAsset", s.base_asset)
        .num("baseAssetPrecision", 8)
        .str("quoteAsset", s.quote_asset)
        .num("quotePrecision", 8)
        .num("quoteAssetPrecision", 8)
        .num("baseCommissionPrecision", 8)
        .num("quoteCommissionPrecision", 8)
        .raw("orderTypes", R"(["LIMIT","LIMIT_MAKER","MARKET"])")
        .boolean("icebergAllowed", false)
        .boolean("ocoAllowed", false)
        .boolean("otoAllowed", false)
        .boolean("quoteOrderQtyMarketAllowed", false)
        .boolean("allowTrailingStop", false)
        .boolean("cancelReplaceAllowed", true)
        .boolean("amendAllowed", true)
        .boolean("isSpotTradingAllowed", true)
        .boolean("isMarginTradingAllowed", false)
        .raw("filters", filters)
        .raw("permissions", "[]")
        .raw("permissionSets", R"([["SPOT"]])")
        .str("defaultSelfTradePreventionMode", "EXPIRE_MAKER")
        .raw("allowedSelfTradePreventionModes", R"(["EXPIRE_MAKER"])");
    w.close();
  }
  arr.push_back(']');
  JsonObjectWriter w(out);
  w.str("timezone", "UTC")
      .num("serverTime", server_ms)
      .raw("rateLimits", rl)
      .raw("exchangeFilters", "[]")
      .raw("symbols", arr);
  w.close();
}

// ---- envelopes ---------------------------------------------------------------------------------

void append_ws_api_response(std::string& out,
                            std::string_view id_json,
                            int status,
                            bool is_error,
                            std::string_view body_json,
                            std::string_view rate_limits_json) {
  JsonObjectWriter w(out);
  w.raw("id", id_json.empty() ? std::string_view("null") : id_json)
      .num("status", status)
      .raw(is_error ? "error" : "result", body_json);
  if (!rate_limits_json.empty()) w.raw("rateLimits", rate_limits_json);
  w.close();
}

void append_stream_message(std::string& out, std::string_view stream, std::string_view data_json) {
  JsonObjectWriter w(out);
  w.str("stream", stream).raw("data", data_json);
  w.close();
}

void append_user_event(std::string& out,
                       std::int64_t subscription_id,
                       std::string_view event_json) {
  // No string is this long; the bound lets GCC 13 (-O3, LTO) see that the append below cannot
  // exceed the maximum object size, which it otherwise reports as -Wstringop-overflow in
  // publish_user_event.
  if (event_json.size() > out.max_size() / 2) return;
  JsonObjectWriter w(out);
  w.num("subscriptionId", subscription_id).raw("event", event_json);
  w.close();
}

}  // namespace fastmm::sim::server
