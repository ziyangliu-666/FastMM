#include "fastmm/venues/lighter/lighter_private.hpp"

#include "fastmm/core/config_macros.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/error_action.hpp"

#include <fmt/format.h>

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::lighter {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

StatusClass classify_status(std::string_view s) noexcept {
  if (s == "open") return StatusClass::Open;
  if (s == "filled") return StatusClass::Filled;
  if (s == "canceled") return StatusClass::Canceled;
  if (s.starts_with("canceled-")) return StatusClass::Ended;
  return StatusClass::Ignore;
}

RejectReason ended_reason(std::string_view s) noexcept {
  if (s == "canceled-post-only") return RejectReason::PostOnlyWouldCross;
  if (s == "canceled-margin-not-allowed" || s == "canceled-invalid-balance")
    return RejectReason::InsufficientBalance;
  if (s == "canceled-self-trade") return RejectReason::SelfTradePrevention;
  return RejectReason::VenueReject;
}

Notional fee_of(Notional usd, std::int64_t rate) noexcept {
  return Notional::from_raw(
      static_cast<std::int64_t>(static_cast<Int128>(usd.raw) * rate / 1'000'000));
}

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// A number the venue writes either bare or as a string.
template <class V>
bool get_int(V&& v, std::int64_t& out) noexcept {
  if (v.get_int64().get(out) == sj::SUCCESS) return true;
  std::string_view s;
  if (v.get_string().get(s) != sj::SUCCESS) return false;
  const auto r = parse_int64(s);
  if (!r) return false;
  out = *r;
  return true;
}

// One Order object.
[[gnu::noinline]] bool read_order(od::object& o, OrderRow& row) noexcept {
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    std::string_view s;
    if (key == "order_index") {
      if (!get_int(field.value(), row.order_index)) return false;
    } else if (key == "client_order_index") {
      if (!get_int(field.value(), row.client_order_index)) return false;
    } else if (key == "market_index") {
      if (!get_int(field.value(), row.market)) return false;
    } else if (key == "price") {
      if (field.value().get_string().get(s) != sj::SUCCESS) return false;
      const auto p = parse_price(s);
      if (!p) return false;
      row.price = *p;
    } else if (key == "initial_base_amount" || key == "remaining_base_amount" ||
               key == "filled_base_amount") {
      if (field.value().get_string().get(s) != sj::SUCCESS) return false;
      const auto q = parse_rounded<Qty>(s);
      if (!q) return false;
      (key == "initial_base_amount"     ? row.initial
       : key == "remaining_base_amount" ? row.remaining
                                        : row.filled) = *q;
    } else if (key == "is_ask") {
      if (field.value().get_bool().get(row.is_ask) != sj::SUCCESS) return false;
    } else if (key == "status") {
      if (field.value().get_string().get(row.status) != sj::SUCCESS) return false;
    } else if (key == "transaction_time") {
      if (!get_int(field.value(), row.time_us)) row.time_us = 0;
    }
  }
  return row.market >= 0 && !row.status.empty();
}

// One Trade object: the account's side(s) of it.
[[gnu::noinline]] bool read_trade(od::object& o,
                                  std::int64_t account,
                                  std::vector<FillRow>& out) noexcept {
  FillRow f;
  std::int64_t ask_account = -1;
  std::int64_t bid_account = -1;
  std::int64_t ask_id = 0;
  std::int64_t bid_id = 0;
  std::int64_t ask_client = 0;
  std::int64_t bid_client = 0;
  std::int64_t maker_fee = 0;
  std::int64_t taker_fee = 0;
  bool maker_ask = false;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    std::string_view s;
    if (key == "trade_id") {
      if (field.value().get_uint64().get(f.trade_id) != sj::SUCCESS) return false;
    } else if (key == "market_id") {
      if (!get_int(field.value(), f.market)) return false;
    } else if (key == "price") {
      if (field.value().get_string().get(s) != sj::SUCCESS) return false;
      const auto p = parse_price(s);
      if (!p) return false;
      f.price = *p;
    } else if (key == "size") {
      if (field.value().get_string().get(s) != sj::SUCCESS) return false;
      const auto q = parse_rounded<Qty>(s);
      if (!q) return false;
      f.size = *q;
    } else if (key == "usd_amount") {
      if (field.value().get_string().get(s) != sj::SUCCESS) return false;
      const auto n = parse_rounded<Notional>(s);
      if (!n) return false;
      f.usd = *n;
    } else if (key == "ask_account_id") {
      if (!get_int(field.value(), ask_account)) return false;
    } else if (key == "bid_account_id") {
      if (!get_int(field.value(), bid_account)) return false;
    } else if (key == "ask_id") {
      if (!get_int(field.value(), ask_id)) return false;
    } else if (key == "bid_id") {
      if (!get_int(field.value(), bid_id)) return false;
    } else if (key == "ask_client_id") {
      if (!get_int(field.value(), ask_client)) return false;
    } else if (key == "bid_client_id") {
      if (!get_int(field.value(), bid_client)) return false;
    } else if (key == "is_maker_ask") {
      if (field.value().get_bool().get(maker_ask) != sj::SUCCESS) return false;
    } else if (key == "maker_fee") {
      if (!get_int(field.value(), maker_fee)) return false;
    } else if (key == "taker_fee") {
      if (!get_int(field.value(), taker_fee)) return false;
    } else if (key == "timestamp") {
      if (!get_int(field.value(), f.time_ms)) return false;
    }
  }
  if (f.market < 0 || f.trade_id == 0) return false;
  bool first = true;
  if (ask_account == account) {
    FillRow a = f;
    a.side = Side::Sell;
    a.maker = maker_ask;
    a.fee_rate = maker_ask ? maker_fee : taker_fee;
    a.order_index = ask_id;
    a.client_order_index = ask_client;
    out.push_back(a);
    first = false;
  }
  if (bid_account == account) {
    FillRow b = f;
    b.side = Side::Buy;
    b.maker = !maker_ask;
    b.fee_rate = maker_ask ? taker_fee : maker_fee;
    b.order_index = bid_id;
    b.client_order_index = bid_client;
    b.self_trade_other_leg = !first;
    out.push_back(b);
  }
  return true;
}

// {"<market>":[Order..]} or [Order..].
template <class Each>
bool read_grouped(od::value v, Each&& each) noexcept {
  od::json_type t;
  if (v.type().get(t) != sj::SUCCESS) return false;
  auto read_array = [&](od::array arr) {
    for (auto item : arr) {
      od::object o;
      if (item.get_object().get(o) != sj::SUCCESS || !each(o)) return false;
    }
    return true;
  };
  if (t == od::json_type::array) {
    od::array arr;
    return v.get_array().get(arr) == sj::SUCCESS && read_array(arr);
  }
  od::object groups;
  if (v.get_object().get(groups) != sj::SUCCESS) return false;
  for (auto g : groups) {
    od::array arr;
    if (g.value().get_array().get(arr) != sj::SUCCESS || !read_array(arr)) return false;
  }
  return true;
}

// The reply fields of a jsonapi/sendtx answer, at whatever level they come.
bool read_reply_field(std::string_view key, od::value v, TxReply& r) noexcept {
  if (key == "id") {
    if (v.get_string().get(r.id) != sj::SUCCESS) {
      r.id = v.raw_json_token();
    }
  } else if (key == "code") {
    if (!get_int(v, r.code)) return false;
  } else if (key == "message") {
    if (v.get_string().get(r.message) != sj::SUCCESS) r.message = {};
  } else if (key == "volume_quota_remaining") {
    if (!get_int(v, r.quota_remaining)) r.quota_remaining = -1;
  } else if (key == "predicted_execution_time_ms") {
    if (!get_int(v, r.predicted_ms)) r.predicted_ms = -1;
  } else if (key == "error") {
    od::object e;
    if (v.get_object().get(e) != sj::SUCCESS) return false;
    r.refused = true;
    for (auto ef : e) {
      std::string_view k;
      if (ef.unescaped_key().get(k) != sj::SUCCESS) return false;
      if (k == "code") {
        if (!get_int(ef.value(), r.code)) return false;
      } else if (k == "message") {
        if (ef.value().get_string().get(r.message) != sj::SUCCESS) r.message = {};
      }
    }
  }
  return true;
}

}  // namespace

struct LighterPrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

LighterPrivateParser::LighterPrivateParser(std::int64_t account, std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), account_(account) {
  orders_.reserve(1024);
  fills_.reserve(1024);
}
LighterPrivateParser::~LighterPrivateParser() = default;

PrivateFrame LighterPrivateParser::decode(std::string_view json) noexcept {
  orders_.clear();
  fills_.clear();
  PrivateFrame f;
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS) {
    f.kind = FrameKind::Malformed;
    return f;
  }
  bool have_orders = false;
  bool have_trades = false;
  bool reply_fields = false;
  for (auto field : root) {
    std::string_view key;
    od::value v;
    if (field.unescaped_key().get(key) != sj::SUCCESS || field.value().get(v) != sj::SUCCESS) {
      f.kind = FrameKind::Malformed;
      return f;
    }
    bool ok = true;
    if (key == "type") {
      ok = v.get_string().get(f.type) == sj::SUCCESS;
    } else if (key == "channel") {
      ok = v.get_string().get(f.channel) == sj::SUCCESS;
    } else if (key == "orders") {
      have_orders = true;
      ok = read_grouped(v, [&](od::object& o) {
        OrderRow row;
        if (!read_order(o, row)) return false;
        orders_.push_back(row);
        return true;
      });
    } else if (key == "trades") {
      have_trades = true;
      ok = read_grouped(v, [&](od::object& o) { return read_trade(o, account_, fills_); });
    } else if (key == "data") {
      od::object d;
      if (v.get_object().get(d) == sj::SUCCESS) {
        for (auto df : d) {
          std::string_view k;
          od::value dv;
          if (df.unescaped_key().get(k) != sj::SUCCESS || df.value().get(dv) != sj::SUCCESS) {
            ok = false;
            break;
          }
          if (!read_reply_field(k, dv, f.reply)) {
            ok = false;
            break;
          }
          reply_fields = reply_fields || k == "id" || k == "tx_hash" || k == "code";
        }
      }
    } else if (key == "id" || key == "code" || key == "message" || key == "error" ||
               key == "volume_quota_remaining" || key == "predicted_execution_time_ms") {
      ok = read_reply_field(key, v, f.reply);
      reply_fields = true;
    } else if (key == "tx_hash") {
      reply_fields = true;
    }
    if (!ok) {
      orders_.clear();
      fills_.clear();
      f.kind = FrameKind::Malformed;
      return f;
    }
  }
  if (f.type.starts_with("subscribed/") || f.type == "connected" || f.type == "pong") {
    orders_.clear();
    fills_.clear();
    f.kind = FrameKind::Ignored;
    return f;
  }
  if (have_orders) {
    f.kind = FrameKind::Orders;
    return f;
  }
  if (have_trades) {
    f.kind = FrameKind::Trades;
    return f;
  }
  if (reply_fields) {
    if (f.reply.code != 0 && f.reply.code != 200) f.reply.refused = true;
    f.kind = f.reply.id.empty() && f.reply.refused ? FrameKind::Error : FrameKind::TxReply;
    return f;
  }
  return f;
}

// ---- REST
// -----------------------------------------------------------------------------------------

namespace {

namespace dom = simdjson::dom;

// The body as an object, or what is wrong: not JSON, or a venue error {"code":!=200,"message"}.
std::string open_body(dom::parser& p, std::string_view body, dom::object& out) {
  dom::element doc;
  if (p.parse(body.data(), body.size()).get(doc) != sj::SUCCESS)
    return fmt::format("unreadable reply: {}", body.substr(0, 160));
  if (doc.get_object().get(out) != sj::SUCCESS) return "reply is not an object";
  std::int64_t code = 200;
  if (out["code"].get_int64().get(code) == sj::SUCCESS && code != 200 && code != 0) {
    std::string_view msg;
    if (out["message"].get_string().get(msg) != sj::SUCCESS) msg = {};
    return fmt::format("code {}: {}", code, msg);
  }
  return {};
}

template <class T>
bool dom_decimal(dom::object o, std::string_view key, T& out) {
  std::string_view s;
  if (o[key].get_string().get(s) != sj::SUCCESS) return false;
  const auto v = parse_rounded<T>(s);
  if (!v) return false;
  out = *v;
  return true;
}

bool dom_int(dom::object o, std::string_view key, std::int64_t& out) {
  dom::element e;
  if (o[key].get(e) != sj::SUCCESS) return false;
  if (e.get_int64().get(out) == sj::SUCCESS) return true;
  std::uint64_t u = 0;
  if (e.get_uint64().get(u) == sj::SUCCESS) {
    out = static_cast<std::int64_t>(u);
    return true;
  }
  std::string_view s;
  if (e.get_string().get(s) != sj::SUCCESS) return false;
  const auto r = parse_int64(s);
  if (!r) return false;
  out = *r;
  return true;
}

// The REST Order and Trade objects share the stream's shape: reuse its readers on their text.
template <class Read>
std::string each_raw(dom::array arr, Read&& read) {
  od::parser p;
  for (dom::element e : arr) {
    const std::string text = sj::minify(e);
    const sj::padded_string padded_text(text);
    od::document d;
    od::object o;
    if (p.iterate(padded_text).get(d) != sj::SUCCESS || d.get_object().get(o) != sj::SUCCESS ||
        !read(o))
      return fmt::format("unreadable row: {}", std::string_view(text).substr(0, 160));
  }
  return {};
}

}  // namespace

std::string decode_market_details(std::string_view body, std::vector<MarketDetail>& out) {
  dom::parser p;
  dom::object root;
  if (std::string err = open_body(p, body, root); !err.empty()) return err;
  dom::array arr;
  if (root["order_book_details"].get_array().get(arr) != sj::SUCCESS)
    return "no order_book_details";
  for (dom::element e : arr) {
    dom::object o;
    if (e.get_object().get(o) != sj::SUCCESS) return "a market is not an object";
    MarketDetail m;
    std::int64_t sd = 0;
    std::int64_t pd = 0;
    std::string_view s;
    if (!dom_int(o, "market_id", m.market_id) || !dom_int(o, "supported_size_decimals", sd) ||
        !dom_int(o, "supported_price_decimals", pd) ||
        o["symbol"].get_string().get(s) != sj::SUCCESS)
      return "a market lacks market_id, symbol or its decimals";
    m.symbol = std::string(s);
    m.size_decimals = static_cast<int>(sd);
    m.price_decimals = static_cast<int>(pd);
    if (o["market_type"].get_string().get(s) == sj::SUCCESS) m.market_type = std::string(s);
    if (o["status"].get_string().get(s) == sj::SUCCESS) m.active = s == "active";
    static_cast<void>(dom_decimal(o, "min_base_amount", m.min_base));
    static_cast<void>(dom_decimal(o, "min_quote_amount", m.min_quote));
    out.push_back(std::move(m));
  }
  return {};
}

std::string decode_next_nonce(std::string_view body, std::int64_t& nonce) {
  dom::parser p;
  dom::object root;
  if (std::string err = open_body(p, body, root); !err.empty()) return err;
  if (!dom_int(root, "nonce", nonce)) return "no nonce";
  return {};
}

std::string decode_account(std::string_view body, AccountState& out) {
  dom::parser p;
  dom::object root;
  if (std::string err = open_body(p, body, root); !err.empty()) return err;
  dom::array accounts;
  if (root["accounts"].get_array().get(accounts) != sj::SUCCESS) return "no accounts";
  dom::object a;
  bool found = false;
  for (dom::element e : accounts) {
    if (e.get_object().get(a) == sj::SUCCESS) {
      found = true;
      break;
    }
  }
  if (!found) return "account not found";
  static_cast<void>(dom_int(a, "index", out.index));
  if (!dom_decimal(a, "collateral", out.collateral)) return "no collateral";
  static_cast<void>(dom_decimal(a, "available_balance", out.available));
  static_cast<void>(dom_decimal(a, "total_asset_value", out.equity));
  dom::array positions;
  if (a["positions"].get_array().get(positions) == sj::SUCCESS) {
    for (dom::element e : positions) {
      dom::object o;
      if (e.get_object().get(o) != sj::SUCCESS) return "a position is not an object";
      PositionRow r;
      std::int64_t sign = 1;
      if (!dom_int(o, "market_id", r.market) || !dom_decimal(o, "position", r.qty))
        return "a position lacks market_id or position";
      static_cast<void>(dom_int(o, "sign", sign));
      if (sign < 0) r.qty = Qty::from_raw(-r.qty.raw);
      static_cast<void>(dom_decimal(o, "avg_entry_price", r.avg_entry));
      out.positions.push_back(r);
    }
  }
  return {};
}

std::string decode_api_keys(std::string_view body, std::vector<ApiKeyRow>& out) {
  dom::parser p;
  dom::object root;
  if (std::string err = open_body(p, body, root); !err.empty()) return err;
  dom::array keys;
  if (root["api_keys"].get_array().get(keys) != sj::SUCCESS) return "no api_keys";
  for (dom::element e : keys) {
    dom::object o;
    if (e.get_object().get(o) != sj::SUCCESS) return "a key is not an object";
    ApiKeyRow r;
    std::string_view pk;
    static_cast<void>(dom_int(o, "api_key_index", r.api_key_index));
    static_cast<void>(dom_int(o, "nonce", r.nonce));
    if (o["public_key"].get_string().get(pk) == sj::SUCCESS) r.public_key = std::string(pk);
    out.push_back(std::move(r));
  }
  return {};
}

std::string decode_active_orders(std::string_view body, std::vector<OrderRow>& out) {
  dom::parser p;
  dom::object root;
  if (std::string err = open_body(p, body, root); !err.empty()) return err;
  dom::array orders;
  if (root["orders"].get_array().get(orders) != sj::SUCCESS) return "no orders";
  return each_raw(orders, [&](od::object& o) {
    OrderRow row;
    if (!read_order(o, row)) return false;
    row.status = {};  // a view into a buffer that is about to go
    out.push_back(row);
    return true;
  });
}

std::string decode_trades(std::string_view body,
                          std::int64_t account,
                          std::vector<FillRow>& out,
                          std::string& next_cursor) {
  dom::parser p;
  dom::object root;
  if (std::string err = open_body(p, body, root); !err.empty()) return err;
  std::string_view cursor;
  if (root["next_cursor"].get_string().get(cursor) == sj::SUCCESS) next_cursor = cursor;
  dom::array trades;
  if (root["trades"].get_array().get(trades) != sj::SUCCESS) return "no trades";
  return each_raw(trades, [&](od::object& o) { return read_trade(o, account, out); });
}

std::string decode_send_tx(std::string_view body, TxReply& out) {
  dom::parser p;
  dom::element doc;
  dom::object root;
  if (p.parse(body.data(), body.size()).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return fmt::format("unreadable reply: {}", body.substr(0, 160));
  static_cast<void>(dom_int(root, "code", out.code));
  std::string_view msg;
  if (root["message"].get_string().get(msg) == sj::SUCCESS) out.message = msg;
  static_cast<void>(dom_int(root, "volume_quota_remaining", out.quota_remaining));
  static_cast<void>(dom_int(root, "predicted_execution_time_ms", out.predicted_ms));
  out.refused = out.code != 200;
  return {};
}

TxErrorMapping map_tx_error(std::int64_t code, std::string_view m) noexcept {
  TxErrorMapping r;
  if (code == 429 || code == 23000 || contains_ci(m, "too many") || contains_ci(m, "rate limit") ||
      contains_ci(m, "quota")) {
    r.reason = RejectReason::VenueRateLimit;
    r.rate_limited = true;
    return r;
  }
  if (contains_ci(m, "nonce")) {
    r.nonce = true;
    return r;
  }
  if (code == 21100 || code == 20013 || contains_ci(m, "account not found") ||
      contains_ci(m, "signature") || contains_ci(m, "api key") || contains_ci(m, "pubkey") ||
      contains_ci(m, "invalid auth")) {
    r.fatal = true;
    return r;
  }
  if (contains_ci(m, "post only") || contains_ci(m, "post-only")) {
    r.reason = RejectReason::PostOnlyWouldCross;
    return r;
  }
  if (contains_ci(m, "margin") || contains_ci(m, "balance") || contains_ci(m, "collateral")) {
    r.reason = RejectReason::InsufficientBalance;
    return r;
  }
  if (contains_ci(m, "not found") || contains_ci(m, "does not exist") ||
      contains_ci(m, "order is not") || contains_ci(m, "no order")) {
    r.reason = RejectReason::VenueUnknownOrder;
    r.unknown_order = true;
    return r;
  }
  if (contains_ci(m, "min") && (contains_ci(m, "amount") || contains_ci(m, "quote")))
    r.reason = RejectReason::BelowMinNotional;
  return r;
}

}  // namespace fastmm::venues::lighter
