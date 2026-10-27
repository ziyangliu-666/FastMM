#include "fastmm/venues/alpaca/alpaca_rest.hpp"

#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/json_writer.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::alpaca {

namespace sj = simdjson;
namespace dom = simdjson::dom;
namespace od = simdjson::ondemand;

// ---- requests --------------------------------------------------------------------------------

namespace {

bool append(char* buf, std::size_t cap, std::size_t& n, std::string_view s) noexcept {
  if (s.size() > cap - n) return false;
  for (const char c : s) buf[n++] = c;
  return true;
}

std::string_view tif_text(TimeInForce t, Tif resting) noexcept {
  switch (t) {
    case TimeInForce::Ioc:
      return "ioc";
    case TimeInForce::Fok:
      return "fok";
    case TimeInForce::Gtc:
    case TimeInForce::Day:
      break;
  }
  return resting == Tif::Gtc ? "gtc" : "day";
}

}  // namespace

bool AlpacaOrderEncoder::encode_new(const OrderCommand& cmd, OrderRequest& out) const noexcept {
  if (cmd.kind != OrderCommandKind::New) return false;
  const std::string_view symbol = symbols_.venue_symbol(cmd.instrument);
  if (symbol.empty()) return false;
  out.path_n = 0;
  if (!append(out.path.data(), out.path.size(), out.path_n, "/v2/orders")) return false;
  JsonWriter w(std::span<char>(out.body.data(), out.body.size()));
  w.begin_object().key("symbol").string(symbol);
  w.key("qty").string(DecimalText(cmd.qty).view());
  w.key("side").string(cmd.side == Side::Buy ? "buy" : "sell");
  const bool market = cmd.type == OrderType::Market;
  w.key("type").string(market ? "market" : "limit");
  const std::string_view tif = market && cmd.tif != TimeInForce::Ioc && cmd.tif != TimeInForce::Fok
                                   ? std::string_view("day")
                                   : tif_text(cmd.tif, opts_.resting_tif);
  w.key("time_in_force").string(tif);
  if (!market) w.key("limit_price").string(DecimalText(cmd.price).view());
  w.key("client_order_id").string(encode_cl_ord_id(cmd.cl_ord_id).view());
  // Alpaca takes extended hours for limit orders that rest for the day or until cancelled.
  if (opts_.extended_hours && !market && (tif == "day" || tif == "gtc"))
    w.key("extended_hours").boolean(true);
  w.end_object();
  if (!w.ok()) return false;
  out.body_n = w.size();
  return true;
}

bool AlpacaOrderEncoder::encode_replace(const OrderCommand& cmd,
                                        std::string_view venue_id,
                                        OrderRequest& out) noexcept {
  if (cmd.kind != OrderCommandKind::Replace || venue_id.empty()) return false;
  out.path_n = 0;
  if (!append(out.path.data(), out.path.size(), out.path_n, "/v2/orders/") ||
      !append(out.path.data(), out.path.size(), out.path_n, venue_id))
    return false;
  JsonWriter w(std::span<char>(out.body.data(), out.body.size()));
  w.begin_object().key("qty").string(DecimalText(cmd.qty).view());
  w.key("limit_price").string(DecimalText(cmd.price).view());
  w.key("client_order_id").string(encode_cl_ord_id(cmd.cl_ord_id).view());
  w.end_object();
  if (!w.ok()) return false;
  out.body_n = w.size();
  return true;
}

std::string AlpacaOrderEncoder::order_path(std::string_view venue_id) {
  return std::string(kPrefix) + "/orders/" + std::string(venue_id);
}

std::string AlpacaOrderEncoder::open_orders_path(std::span<const std::string> symbols) {
  std::string p = std::string(kPrefix) + "/orders?status=open&limit=500&direction=asc";
  if (!symbols.empty()) {
    p += "&symbols=";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
      if (i != 0) p += "%2C";
      p += symbols[i];
    }
  }
  return p;
}

std::string AlpacaOrderEncoder::fills_path(std::int64_t after_ms,
                                           std::int64_t until_ms,
                                           std::string_view page_token,
                                           int page_size) {
  std::string p = std::string(kPrefix) +
                  "/account/activities/FILL?direction=desc&page_size=" + std::to_string(page_size);
  // "after" is exclusive: one millisecond earlier keeps the start inclusive.
  if (after_ms > 0) p.append("&after=").append(coinbase::format_time_ms(after_ms - 1).view());
  if (until_ms > 0) p.append("&until=").append(coinbase::format_time_ms(until_ms).view());
  if (!page_token.empty()) p.append("&page_token=").append(page_token);
  return p;
}

// ---- REST replies ------------------------------------------------------------------------------

namespace {

thread_local dom::parser t_parser;

std::string_view str(dom::object o, std::string_view key) {
  std::string_view v;
  if (o[key].get_string().get(v) != sj::SUCCESS) return {};
  return v;
}

template <class F>
bool fixed(dom::object o, std::string_view key, F& out) {
  const std::string_view s = str(o, key);
  if (s.empty()) return true;  // null or absent: zero
  const auto v = parse_rounded<F>(s);
  if (!v) return false;
  out = *v;
  return true;
}

std::string read_order(dom::object o, OrderRow& out) {
  out.id = std::string(str(o, "id"));
  out.client_order_id = std::string(str(o, "client_order_id"));
  out.symbol = std::string(str(o, "symbol"));
  out.side = std::string(str(o, "side"));
  out.status = std::string(str(o, "status"));
  out.replaced_by = std::string(str(o, "replaced_by"));
  if (out.id.empty() || out.symbol.empty()) return "order without id or symbol";
  if (!fixed(o, "qty", out.qty) || !fixed(o, "filled_qty", out.filled_qty) ||
      !fixed(o, "limit_price", out.limit_price))
    return "order " + out.id + ": unreadable qty or price";
  return {};
}

}  // namespace

std::string decode_order(std::string_view json, OrderRow& out) {
  dom::object o;
  if (t_parser.parse(json.data(), json.size()).get_object().get(o) != sj::SUCCESS)
    return "order: not a JSON object";
  return read_order(o, out);
}

std::string decode_orders(std::string_view json, std::vector<OrderRow>& out) {
  dom::array a;
  if (t_parser.parse(json.data(), json.size()).get_array().get(a) != sj::SUCCESS)
    return "orders: not a JSON array";
  for (dom::element e : a) {
    dom::object o;
    if (e.get_object().get(o) != sj::SUCCESS) return "orders: not an object";
    OrderRow row;
    if (std::string err = read_order(o, row); !err.empty()) return err;
    out.push_back(std::move(row));
  }
  return {};
}

std::string decode_positions(std::string_view json, std::vector<PositionRow>& out) {
  dom::array a;
  if (t_parser.parse(json.data(), json.size()).get_array().get(a) != sj::SUCCESS)
    return "positions: not a JSON array";
  for (dom::element e : a) {
    dom::object o;
    if (e.get_object().get(o) != sj::SUCCESS) return "positions: not an object";
    PositionRow p;
    p.symbol = std::string(str(o, "symbol"));
    if (p.symbol.empty() || !fixed(o, "qty", p.qty) || !fixed(o, "avg_entry_price", p.avg_px))
      return "positions: unreadable row";
    // qty is negative for a short; the side says so too.
    if (str(o, "side") == "short" && p.qty.is_positive()) p.qty = -p.qty;
    out.push_back(std::move(p));
  }
  return {};
}

std::string decode_fills(std::string_view json, std::vector<FillRow>& out) {
  dom::array a;
  if (t_parser.parse(json.data(), json.size()).get_array().get(a) != sj::SUCCESS)
    return "activities: not a JSON array";
  for (dom::element e : a) {
    dom::object o;
    if (e.get_object().get(o) != sj::SUCCESS) return "activities: not an object";
    FillRow f;
    f.page_id = std::string(str(o, "id"));
    const std::size_t sep = f.page_id.find("::");
    f.exec_id = sep == std::string::npos ? f.page_id : f.page_id.substr(sep + 2);
    f.order_id = std::string(str(o, "order_id"));
    f.symbol = std::string(str(o, "symbol"));
    const std::string_view side = str(o, "side");
    f.side = side == "buy" ? Side::Buy : Side::Sell;
    const std::int64_t t = coinbase::parse_time_ns(str(o, "transaction_time"));
    if (f.exec_id.empty() || f.symbol.empty() || side.empty() || t < 0 ||
        !fixed(o, "price", f.price) || !fixed(o, "qty", f.qty))
      return "activities: unreadable fill " + f.page_id;
    f.time_ms = t / 1'000'000;
    out.push_back(std::move(f));
  }
  return {};
}

std::string decode_asset(std::string_view json, AssetInfo& out) {
  dom::object o;
  if (t_parser.parse(json.data(), json.size()).get_object().get(o) != sj::SUCCESS)
    return "asset: not a JSON object";
  out.status = std::string(str(o, "status"));
  bool b = false;
  out.tradable = o["tradable"].get_bool().get(b) == sj::SUCCESS && b;
  out.shortable = o["shortable"].get_bool().get(b) == sj::SUCCESS && b;
  out.fractionable = o["fractionable"].get_bool().get(b) == sj::SUCCESS && b;
  if (out.status.empty()) return "asset: no status";
  return {};
}

std::string decode_clock(std::string_view json, std::int64_t& epoch_ms, bool& is_open) {
  dom::object o;
  if (t_parser.parse(json.data(), json.size()).get_object().get(o) != sj::SUCCESS)
    return "clock: not a JSON object";
  // "2026-10-27T02:28:45.264685368-04:00": New York time with its offset.
  const std::string_view ts = str(o, "timestamp");
  if (ts.size() < 26) return "clock: no timestamp";
  const std::string_view off = ts.substr(ts.size() - 6);
  if ((off[0] != '+' && off[0] != '-') || off[3] != ':') return "clock: no offset";
  const int oh = (off[1] - '0') * 10 + (off[2] - '0');
  const int om = (off[4] - '0') * 10 + (off[5] - '0');
  std::string utc(ts.substr(0, ts.size() - 6));
  utc += 'Z';
  const std::int64_t local = coinbase::parse_time_ns(utc);
  if (local < 0) return "clock: unreadable timestamp";
  const std::int64_t shift = (std::int64_t{oh} * 3600 + std::int64_t{om} * 60) * 1'000'000'000;
  epoch_ms = (off[0] == '-' ? local + shift : local - shift) / 1'000'000;
  bool b = false;
  is_open = o["is_open"].get_bool().get(b) == sj::SUCCESS && b;
  return {};
}

ApiError decode_error(std::string_view json) {
  ApiError e;
  dom::object o;
  if (t_parser.parse(json.data(), json.size()).get_object().get(o) != sj::SUCCESS) return e;
  std::int64_t code = 0;
  if (o["code"].get_int64().get(code) == sj::SUCCESS) e.code = code;
  e.message = std::string(str(o, "message"));
  e.reject_reason = std::string(str(o, "reject_reason"));
  return e;
}

// ---- trade_updates ---------------------------------------------------------------------------

struct TradeUpdateParser::Impl {
  od::parser parser;
  Impl() {
    if (parser.allocate(1U << 16) != sj::SUCCESS) std::abort();
  }
};

TradeUpdateParser::TradeUpdateParser() : impl_(std::make_unique<Impl>()) {}
TradeUpdateParser::~TradeUpdateParser() = default;

namespace {

bool view_of(od::value v, std::string_view& out) noexcept {
  bool is_null = false;
  if (v.is_null().get(is_null) == sj::SUCCESS && is_null) {
    out = {};
    return true;
  }
  return v.get_string().get(out) == sj::SUCCESS;
}

template <class F>
bool fixed_of(od::value v, F& out) noexcept {
  std::string_view s;
  if (!view_of(v, s)) return false;
  if (s.empty()) return true;
  const auto x = parse_rounded<F>(s);
  if (!x) return false;
  out = *x;
  return true;
}

[[gnu::noinline]] bool read_order_view(od::object o, OrderView& out) noexcept {
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    od::value v;
    if (field.value().get(v) != sj::SUCCESS) return false;
    bool ok = true;
    if (key == "id") {
      ok = view_of(v, out.id);
    } else if (key == "client_order_id") {
      ok = view_of(v, out.client_order_id);
    } else if (key == "symbol") {
      ok = view_of(v, out.symbol);
    } else if (key == "side") {
      ok = view_of(v, out.side);
    } else if (key == "status") {
      ok = view_of(v, out.status);
    } else if (key == "replaced_by") {
      ok = view_of(v, out.replaced_by);
    } else if (key == "qty") {
      ok = fixed_of(v, out.qty);
    } else if (key == "filled_qty") {
      ok = fixed_of(v, out.filled_qty);
    } else if (key == "limit_price") {
      ok = fixed_of(v, out.limit_price);
    }
    if (!ok) return false;
  }
  return !out.id.empty();
}

}  // namespace

bool TradeUpdateParser::parse(std::string_view json, TradeUpdate& out) noexcept {
  out = TradeUpdate{};
  od::document doc;
  const sj::padded_string_view p(json.data(), json.size(), json.size() + sj::SIMDJSON_PADDING);
  if (impl_->parser.iterate(p).get(doc) != sj::SUCCESS) return false;
  od::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return false;
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "stream") {
      std::string_view s;
      if (field.value().get_string().get(s) != sj::SUCCESS) return false;
      out.stream = s == "trade_updates"   ? TradeUpdate::Stream::TradeUpdates
                   : s == "authorization" ? TradeUpdate::Stream::Authorization
                   : s == "listening"     ? TradeUpdate::Stream::Listening
                                          : TradeUpdate::Stream::Other;
    } else if (key == "data") {
      od::object data;
      if (field.value().get_object().get(data) != sj::SUCCESS) return false;
      for (auto f : data) {
        std::string_view k;
        if (f.unescaped_key().get(k) != sj::SUCCESS) return false;
        od::value v;
        if (f.value().get(v) != sj::SUCCESS) return false;
        bool ok = true;
        if (k == "event") {
          ok = view_of(v, out.event);
        } else if (k == "status") {
          ok = view_of(v, out.status);
        } else if (k == "execution_id") {
          ok = view_of(v, out.execution_id);
        } else if (k == "timestamp") {
          std::string_view t;
          ok = view_of(v, t);
          if (ok && !t.empty()) out.time_ns = coinbase::parse_time_ns(t);
        } else if (k == "price") {
          ok = fixed_of(v, out.price);
        } else if (k == "qty") {
          ok = fixed_of(v, out.qty);
        } else if (k == "order") {
          od::object o;
          ok = v.get_object().get(o) == sj::SUCCESS && read_order_view(o, out.order);
        }
        if (!ok) return false;
      }
    }
  }
  return out.stream != TradeUpdate::Stream::TradeUpdates || !out.event.empty();
}

}  // namespace fastmm::venues::alpaca
