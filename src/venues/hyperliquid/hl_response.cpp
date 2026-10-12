#include "fastmm/venues/hyperliquid/hl_response.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <string>

namespace fastmm::venues::hyperliquid {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct HlResponseDecoder::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

HlResponseDecoder::HlResponseDecoder(std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)) {}
HlResponseDecoder::~HlResponseDecoder() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// {"resting":{..}} | {"filled":{..}} | {"error":".."} | "success"
bool read_status(od::value v, OrderStatus& out) noexcept {
  std::string_view word;
  if (v.get_string().get(word) == sj::SUCCESS) {
    out.kind = word == "success" ? OrderStatus::Kind::Success : OrderStatus::Kind::Other;
    if (out.kind == OrderStatus::Kind::Other) out.error = word;
    return true;
  }
  od::object o;
  if (v.get_object().get(o) != sj::SUCCESS) return false;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "error") {
      out.kind = OrderStatus::Kind::Error;
      if (field.value().get_string().get(out.error) != sj::SUCCESS) return false;
      continue;
    }
    if (key != "resting" && key != "filled") continue;
    out.kind = key == "resting" ? OrderStatus::Kind::Resting : OrderStatus::Kind::Filled;
    od::object d;
    if (field.value().get_object().get(d) != sj::SUCCESS) return false;
    for (auto f : d) {
      std::string_view k;
      if (f.unescaped_key().get(k) != sj::SUCCESS) return false;
      if (k == "oid") {
        if (f.value().get_uint64().get(out.oid) != sj::SUCCESS) return false;
      } else if (k == "cloid") {
        if (f.value().get_string().get(out.cloid) != sj::SUCCESS) out.cloid = {};
      } else if (k == "totalSz") {
        std::string_view s;
        if (f.value().get_string().get(s) != sj::SUCCESS) return false;
        const auto q = parse_rounded<Qty>(s);
        if (!q) return false;
        out.total_sz = *q;
      } else if (k == "avgPx") {
        std::string_view s;
        if (f.value().get_string().get(s) != sj::SUCCESS) return false;
        const auto p = parse_avg_price(s);
        if (!p) return false;
        out.avg_px = *p;
      }
    }
  }
  return true;
}

// The action payload {"status":"ok"|"err","response":..}.
ParseStatus read_action(od::object& payload, PostReply& out) noexcept {
  out.kind = PostReply::Kind::Action;
  std::string_view status;
  if (payload["status"].get_string().get(status) != sj::SUCCESS) return ParseStatus::Malformed;
  out.ok = status == "ok";
  od::value resp;
  if (payload["response"].get(resp) != sj::SUCCESS) return ParseStatus::Malformed;
  if (!out.ok) {
    if (resp.get_string().get(out.error) != sj::SUCCESS) out.error = "unreadable error";
    return ParseStatus::Ok;
  }
  od::object r;
  if (resp.get_object().get(r) != sj::SUCCESS) return ParseStatus::Malformed;
  for (auto field : r) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return ParseStatus::Malformed;
    if (key == "type") {
      if (field.value().get_string().get(out.action_type) != sj::SUCCESS)
        return ParseStatus::Malformed;
    } else if (key == "data") {
      od::object data;
      if (field.value().get_object().get(data) != sj::SUCCESS) return ParseStatus::Malformed;
      od::array st;
      if (data["statuses"].get_array().get(st) != sj::SUCCESS) continue;
      for (auto item : st) {
        od::value v;
        if (item.get(v) != sj::SUCCESS) return ParseStatus::Malformed;
        if (out.count >= kMaxBatch) return ParseStatus::Overflow;
        if (!read_status(v, out.statuses[out.count])) return ParseStatus::Malformed;
        ++out.count;
      }
    }
  }
  return ParseStatus::Ok;
}

}  // namespace

ParseStatus HlResponseDecoder::decode_frame(std::string_view json, PostReply& out) noexcept {
  out = PostReply{};
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return ParseStatus::Malformed;
  od::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return ParseStatus::Malformed;
  std::string_view channel;
  if (root["channel"].get_string().get(channel) != sj::SUCCESS) return ParseStatus::Malformed;
  if (channel == "error") {
    out.kind = PostReply::Kind::Error;
    if (root["data"].get_string().get(out.error) != sj::SUCCESS) out.error = "unreadable error";
    return ParseStatus::Ok;
  }
  if (channel != "post") return ParseStatus::Ignored;
  od::object data;
  if (root["data"].get_object().get(data) != sj::SUCCESS) return ParseStatus::Malformed;
  if (data["id"].get_uint64().get(out.id) != sj::SUCCESS) return ParseStatus::Malformed;
  od::object response;
  if (data["response"].get_object().get(response) != sj::SUCCESS) return ParseStatus::Malformed;
  std::string_view type;
  if (response["type"].get_string().get(type) != sj::SUCCESS) return ParseStatus::Malformed;
  if (type == "info") {
    out.kind = PostReply::Kind::Info;
    od::object payload;
    if (response["payload"].get_object().get(payload) != sj::SUCCESS) return ParseStatus::Malformed;
    if (payload["type"].get_string().get(out.info_type) != sj::SUCCESS)
      return ParseStatus::Malformed;
    od::value d;
    if (payload["data"].get(d) != sj::SUCCESS || d.raw_json().get(out.info_data) != sj::SUCCESS)
      return ParseStatus::Malformed;
    return ParseStatus::Ok;
  }
  if (type == "error") {  // a post the venue could not take, e.g. too many in flight
    out.kind = PostReply::Kind::Error;
    if (response["payload"].get_string().get(out.error) != sj::SUCCESS)
      out.error = "unreadable error";
    return ParseStatus::Ok;
  }
  od::object payload;
  if (response["payload"].get_object().get(payload) != sj::SUCCESS) return ParseStatus::Malformed;
  return read_action(payload, out);
}

ParseStatus HlResponseDecoder::decode_exchange(std::string_view json, PostReply& out) noexcept {
  out = PostReply{};
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return ParseStatus::Malformed;
  od::object payload;
  if (doc.get_object().get(payload) != sj::SUCCESS) return ParseStatus::Malformed;
  return read_action(payload, out);
}

// ---- info answers (simdjson DOM, control path) --------------------------------------------------

namespace {

namespace dom = simdjson::dom;

template <class F>
Result<F, DecimalError> fixed_of(dom::element e) noexcept {
  std::string_view s;
  if (e.get_string().get(s) != sj::SUCCESS) return fail(DecimalError::Malformed);
  return parse_rounded<F>(s);
}

std::string str_of(dom::element e) {
  std::string_view s;
  if (e.get_string().get(s) != sj::SUCCESS) return {};
  return std::string(s);
}

struct Doc {
  dom::parser parser;
  sj::padded_string text;
  dom::element root;
  std::string error;
  explicit Doc(std::string_view body) : text(body) {
    if (parser.parse(text).get(root) != sj::SUCCESS) error = "unparseable JSON";
  }
};

std::string read_universe(dom::array universe, std::vector<AssetMeta>& out) {
  for (dom::element u : universe) {
    AssetMeta m;
    m.name = str_of(u["name"]);
    std::int64_t v = 0;
    if (u["szDecimals"].get_int64().get(v) != sj::SUCCESS || m.name.empty())
      return "meta: an asset without name or szDecimals";
    m.sz_decimals = static_cast<int>(v);
    if (u["maxLeverage"].get_int64().get(v) == sj::SUCCESS) m.max_leverage = static_cast<int>(v);
    bool b = false;
    if (u["onlyIsolated"].get_bool().get(b) == sj::SUCCESS) m.only_isolated = b;
    std::string_view mode;
    if (u["marginMode"].get_string().get(mode) == sj::SUCCESS)
      m.only_isolated = m.only_isolated || mode == "noCross" || mode == "strictIsolated";
    if (u["isDelisted"].get_bool().get(b) == sj::SUCCESS) m.delisted = b;
    out.push_back(std::move(m));
  }
  return {};
}

}  // namespace

std::string decode_perp_dexs(std::string_view body, std::vector<std::string>& out) {
  Doc d(body);
  if (!d.error.empty()) return "perpDexs: " + d.error;
  dom::array arr;
  if (d.root.get_array().get(arr) != sj::SUCCESS) return "perpDexs: not an array";
  for (dom::element e : arr) {
    if (e.is_null()) {
      out.emplace_back();
      continue;
    }
    out.push_back(str_of(e["name"]));
  }
  return {};
}

std::string decode_meta(std::string_view body, std::vector<AssetMeta>& out) {
  Doc d(body);
  if (!d.error.empty()) return "meta: " + d.error;
  dom::array universe;
  dom::array pair;
  if (d.root.get_array().get(pair) == sj::SUCCESS) {  // metaAndAssetCtxs
    dom::element meta;
    dom::array ctxs;
    if (pair.at(0).get(meta) != sj::SUCCESS ||
        meta["universe"].get_array().get(universe) != sj::SUCCESS ||
        pair.at(1).get_array().get(ctxs) != sj::SUCCESS)
      return "metaAndAssetCtxs: expected [meta, contexts]";
    if (std::string err = read_universe(universe, out); !err.empty()) return err;
    std::size_t i = 0;
    for (dom::element c : ctxs) {
      if (i >= out.size()) break;
      if (const auto p = fixed_of<Price>(c["markPx"])) out[i].mark = *p;
      ++i;
    }
    return {};
  }
  if (d.root["universe"].get_array().get(universe) != sj::SUCCESS) return "meta: no universe";
  return read_universe(universe, out);
}

std::string decode_clearinghouse(std::string_view body, ClearinghouseState& out) {
  Doc d(body);
  if (!d.error.empty()) return "clearinghouseState: " + d.error;
  dom::object summary;
  if (d.root["marginSummary"].get_object().get(summary) != sj::SUCCESS)
    return "clearinghouseState: no marginSummary";
  if (const auto v = fixed_of<Notional>(summary["accountValue"])) out.account_value = *v;
  if (const auto v = fixed_of<Notional>(summary["totalMarginUsed"])) out.margin_used = *v;
  if (const auto v = fixed_of<Notional>(d.root["withdrawable"])) out.withdrawable = *v;
  std::int64_t t = 0;
  if (d.root["time"].get_int64().get(t) == sj::SUCCESS) out.time_ms = t;
  dom::array positions;
  if (d.root["assetPositions"].get_array().get(positions) != sj::SUCCESS)
    return "clearinghouseState: no assetPositions";
  for (dom::element e : positions) {
    dom::element p;
    if (e["position"].get(p) != sj::SUCCESS) return "clearinghouseState: an entry without position";
    PositionRow row;
    row.coin = str_of(p["coin"]);
    const auto szi = fixed_of<Qty>(p["szi"]);
    if (row.coin.empty() || !szi) return "clearinghouseState: a position without coin or szi";
    row.szi = *szi;
    if (const auto entry = fixed_of<Price>(p["entryPx"])) row.entry = *entry;
    out.positions.push_back(std::move(row));
  }
  return {};
}

std::string decode_spot_balances(std::string_view body, std::vector<SpotBalanceRow>& out) {
  Doc d(body);
  if (!d.error.empty()) return "spotClearinghouseState: " + d.error;
  dom::array arr;
  if (d.root["balances"].get_array().get(arr) != sj::SUCCESS)
    return "spotClearinghouseState: no balances";
  for (dom::element e : arr) {
    SpotBalanceRow row;
    row.coin = str_of(e["coin"]);
    const auto total = fixed_of<Notional>(e["total"]);
    if (row.coin.empty() || !total)
      return "spotClearinghouseState: a balance without coin or total";
    row.total = *total;
    if (const auto hold = fixed_of<Notional>(e["hold"])) row.hold = *hold;
    out.push_back(std::move(row));
  }
  return {};
}

std::string decode_open_orders(std::string_view body, std::vector<OpenOrderRow>& out) {
  Doc d(body);
  if (!d.error.empty()) return "openOrders: " + d.error;
  dom::array arr;
  if (d.root.get_array().get(arr) != sj::SUCCESS) return "openOrders: not an array";
  for (dom::element e : arr) {
    OpenOrderRow row;
    row.coin = str_of(e["coin"]);
    const std::string side = str_of(e["side"]);
    const auto px = fixed_of<Price>(e["limitPx"]);
    const auto sz = fixed_of<Qty>(e["sz"]);
    if (row.coin.empty() || (side != "B" && side != "A") || !px || !sz ||
        e["oid"].get_uint64().get(row.oid) != sj::SUCCESS)
      return "openOrders: an order without coin, side, limitPx, sz or oid";
    row.buy = side == "B";
    row.px = *px;
    row.sz = *sz;
    row.orig_sz = *sz;
    if (const auto o = fixed_of<Qty>(e["origSz"])) row.orig_sz = *o;
    row.cloid = str_of(e["cloid"]);
    std::int64_t t = 0;
    if (e["timestamp"].get_int64().get(t) == sj::SUCCESS) row.time_ms = t;
    out.push_back(std::move(row));
  }
  return {};
}

std::string decode_user_fills(std::string_view body, std::vector<FillRow>& out) {
  Doc d(body);
  if (!d.error.empty()) return "userFills: " + d.error;
  dom::array arr;
  if (d.root.get_array().get(arr) != sj::SUCCESS) return "userFills: not an array";
  for (dom::element e : arr) {
    FillRow row;
    row.coin = str_of(e["coin"]);
    const std::string side = str_of(e["side"]);
    const auto px = fixed_of<Price>(e["px"]);
    const auto sz = fixed_of<Qty>(e["sz"]);
    if (row.coin.empty() || (side != "B" && side != "A") || !px || !sz ||
        e["tid"].get_uint64().get(row.tid) != sj::SUCCESS ||
        e["time"].get_int64().get(row.time_ms) != sj::SUCCESS)
      return "userFills: a fill without coin, side, px, sz, tid or time";
    row.buy = side == "B";
    row.px = *px;
    row.sz = *sz;
    if (e["oid"].get_uint64().get(row.oid) != sj::SUCCESS) row.oid = 0;
    if (e["crossed"].get_bool().get(row.crossed) != sj::SUCCESS) row.crossed = false;
    if (const auto f = fixed_of<Notional>(e["fee"])) row.fee = *f;
    row.fee_token = str_of(e["feeToken"]);
    row.cloid = str_of(e["cloid"]);
    out.push_back(std::move(row));
  }
  return {};
}

std::string decode_rate_limit(std::string_view body, RateLimitState& out) {
  Doc d(body);
  if (!d.error.empty()) return "userRateLimit: " + d.error;
  std::string_view vlm;
  if (d.root["cumVlm"].get_string().get(vlm) == sj::SUCCESS)
    out.cum_volume = std::strtod(std::string(vlm).c_str(), nullptr);
  if (d.root["nRequestsUsed"].get_int64().get(out.used) != sj::SUCCESS ||
      d.root["nRequestsCap"].get_int64().get(out.cap) != sj::SUCCESS)
    return "userRateLimit: no nRequestsUsed / nRequestsCap";
  if (d.root["nRequestsSurplus"].get_int64().get(out.surplus) != sj::SUCCESS) out.surplus = 0;
  return {};
}

std::string decode_order_status(std::string_view body, OrderStatusRow& out) {
  Doc d(body);
  if (!d.error.empty()) return "orderStatus: " + d.error;
  std::string_view status;
  if (d.root["status"].get_string().get(status) != sj::SUCCESS) return "orderStatus: no status";
  if (status != "order") {
    out.status = std::string(status);  // unknownOid
    return {};
  }
  dom::element order;
  if (d.root["order"].get(order) != sj::SUCCESS) return "orderStatus: no order";
  out.status = str_of(order["status"]);
  if (order["statusTimestamp"].get_int64().get(out.status_ms) != sj::SUCCESS) out.status_ms = 0;
  if (order["order"]["timestamp"].get_int64().get(out.placed_ms) != sj::SUCCESS) out.placed_ms = 0;
  return {};
}

}  // namespace fastmm::venues::hyperliquid
