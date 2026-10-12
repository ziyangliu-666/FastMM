#include "fastmm/venues/hyperliquid/hl_private_parser.hpp"

#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/hyperliquid/hl_error_map.hpp"
#include "fastmm/venues/hyperliquid/hl_order_encoder.hpp"

#include <simdjson.h>

#include <cstdlib>

namespace fastmm::venues::hyperliquid {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct HlPrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

HlPrivateParser::HlPrivateParser(const SymbolTable& symbols,
                                 VenueId venue,
                                 std::string dex,
                                 std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      coins_(symbols, venue, std::move(dex)),
      venue_(venue) {}
HlPrivateParser::~HlPrivateParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

PrivateDecodeResult malformed(PrivateParserStats& stats, PrivateDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  return r;
}

ClientOrderId client_id(std::string_view cloid) noexcept {
  const auto id = cl_ord_id_of_cloid(cloid);
  return id ? *id : ClientOrderId{};
}

struct Ctx {
  PrivateParserStats* stats;
  const CoinLookup* coins;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::span<std::byte> out;
  std::uint32_t written = 0;
  std::uint32_t count = 0;
  bool overflow = false;

  template <class M>
  M* reserve() noexcept {
    if (written + sizeof(M) > out.size()) {
      ++stats->overflow;
      overflow = true;
      return nullptr;
    }
    auto* m = reinterpret_cast<M*>(out.data() + written);
    written += static_cast<std::uint32_t>(sizeof(M));
    ++count;
    return m;
  }
  template <class M>
  void stamp(M& m, std::int64_t venue_ms) const noexcept {
    m.hdr.exch_ts = ts_from_ms(venue_ms);
    m.hdr.recv_ts = recv_ts;
    m.hdr.t0_cycles = t0;
  }
};

// One orderUpdates entry: {"order":{..},"status":..,"statusTimestamp":ms}.
[[gnu::noinline]] bool decode_order_update(Ctx& c, od::object& u) noexcept {
  std::string_view coin;
  std::string_view sz;
  std::string_view orig_sz;
  std::string_view cloid;
  std::uint64_t oid = 0;
  std::string_view status;
  std::int64_t status_ts = 0;
  for (auto field : u) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "order") {
      od::object o;
      if (field.value().get_object().get(o) != sj::SUCCESS) return false;
      for (auto of : o) {
        std::string_view k;
        if (of.unescaped_key().get(k) != sj::SUCCESS) return false;
        if (k == "coin") {
          if (of.value().get_string().get(coin) != sj::SUCCESS) return false;
        } else if (k == "sz") {
          if (of.value().get_string().get(sz) != sj::SUCCESS) return false;
        } else if (k == "origSz") {
          if (of.value().get_string().get(orig_sz) != sj::SUCCESS) return false;
        } else if (k == "oid") {
          if (of.value().get_uint64().get(oid) != sj::SUCCESS) return false;
        } else if (k == "cloid") {
          // null for an order placed without one
          if (of.value().get_string().get(cloid) != sj::SUCCESS) cloid = {};
        }
      }
    } else if (key == "status") {
      if (field.value().get_string().get(status) != sj::SUCCESS) return false;
    } else if (key == "statusTimestamp") {
      if (field.value().get_int64().get(status_ts) != sj::SUCCESS) return false;
    }
  }
  if (coin.empty() || status.empty() || oid == 0) return false;
  ++c.stats->order_updates;
  const StatusClass cls = classify_status(status);
  if (cls == StatusClass::Ignore) return true;
  const InstrumentId inst = c.coins->find(coin);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return true;
  }
  const ClientOrderId id = client_id(cloid);
  const IdText oid_text(static_cast<std::int64_t>(oid));
  Qty cum{};
  if (cls == StatusClass::Canceled || cls == StatusClass::Expired) {
    const auto left = parse_rounded<Qty>(sz);
    const auto orig = parse_rounded<Qty>(orig_sz);
    if (!left || !orig) return false;
    cum = *orig > *left ? *orig - *left : Qty{};
  }
  switch (cls) {
    case StatusClass::Open: {
      auto* m = c.reserve<OrderAckMsg>();
      if (m == nullptr) return true;
      init_header(*m, EventType::OrderAck, inst, c.venue);
      m->cl_ord_id = id;
      static_cast<void>(m->venue_order_id.assign(oid_text.view()));
      m->flags = 0;
      c.stamp(*m, status_ts);
      break;
    }
    case StatusClass::Canceled: {
      auto* m = c.reserve<OrderCancelAckMsg>();
      if (m == nullptr) return true;
      init_header(*m, EventType::OrderCancelAck, inst, c.venue);
      m->cl_ord_id = id;
      static_cast<void>(m->venue_order_id.assign(oid_text.view()));
      m->cum_qty = cum;
      c.stamp(*m, status_ts);
      break;
    }
    case StatusClass::Expired: {
      auto* m = c.reserve<OrderExpiredMsg>();
      if (m == nullptr) return true;
      init_header(*m, EventType::OrderExpired, inst, c.venue);
      m->cl_ord_id = id;
      static_cast<void>(m->venue_order_id.assign(oid_text.view()));
      m->cum_qty = cum;
      c.stamp(*m, status_ts);
      break;
    }
    case StatusClass::Rejected: {
      auto* m = c.reserve<OrderRejectMsg>();
      if (m == nullptr) return true;
      init_header(*m, EventType::OrderReject, inst, c.venue);
      m->cl_ord_id = id;
      m->reason = rejected_reason(status);
      m->venue_code = 0;
      static_cast<void>(m->text.assign(status.substr(0, m->text.kCapacity)));
      c.stamp(*m, status_ts);
      break;
    }
    case StatusClass::Ignore:
      break;
  }
  return true;
}

// One userFills entry.
[[gnu::noinline]] bool decode_fill(Ctx& c, od::object& f) noexcept {
  std::string_view coin;
  std::string_view px;
  std::string_view sz;
  std::string_view side;
  std::string_view fee;
  std::string_view fee_token;
  std::string_view cloid;
  std::int64_t time = 0;
  std::uint64_t oid = 0;
  std::uint64_t tid = 0;
  bool crossed = false;
  for (auto field : f) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "coin") {
      if (field.value().get_string().get(coin) != sj::SUCCESS) return false;
    } else if (key == "px") {
      if (field.value().get_string().get(px) != sj::SUCCESS) return false;
    } else if (key == "sz") {
      if (field.value().get_string().get(sz) != sj::SUCCESS) return false;
    } else if (key == "side") {
      if (field.value().get_string().get(side) != sj::SUCCESS) return false;
    } else if (key == "time") {
      if (field.value().get_int64().get(time) != sj::SUCCESS) return false;
    } else if (key == "oid") {
      if (field.value().get_uint64().get(oid) != sj::SUCCESS) return false;
    } else if (key == "crossed") {
      if (field.value().get_bool().get(crossed) != sj::SUCCESS) return false;
    } else if (key == "fee") {
      if (field.value().get_string().get(fee) != sj::SUCCESS) return false;
    } else if (key == "feeToken") {
      if (field.value().get_string().get(fee_token) != sj::SUCCESS) fee_token = {};
    } else if (key == "tid") {
      if (field.value().get_uint64().get(tid) != sj::SUCCESS) return false;
    } else if (key == "cloid") {
      if (field.value().get_string().get(cloid) != sj::SUCCESS) cloid = {};
    }
  }
  if (coin.empty() || px.empty() || sz.empty() || tid == 0 || (side != "B" && side != "A"))
    return false;
  ++c.stats->fills;
  const InstrumentId inst = c.coins->find(coin);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return true;
  }
  const auto p = parse_price(px);
  const auto q = parse_rounded<Qty>(sz);
  const auto fe = fee.empty() ? Result<Notional, DecimalError>(Notional{}) : parse_fee(fee);
  if (!p || !q || !fe) return false;
  auto* m = c.reserve<OrderFillMsg>();
  if (m == nullptr) return true;
  init_header(*m, EventType::OrderFill, inst, c.venue);
  m->cl_ord_id = client_id(cloid);
  static_cast<void>(m->venue_order_id.assign(IdText(static_cast<std::int64_t>(oid)).view()));
  static_cast<void>(m->exec_id.assign(IdText(static_cast<std::int64_t>(tid)).view()));
  m->price = *p;
  m->qty = *q;
  m->cum_qty = Qty{};
  m->leaves_qty = Qty{};
  m->fee = *fe;
  m->side = side == "B" ? Side::Buy : Side::Sell;
  m->liquidity = crossed ? Liquidity::Taker : Liquidity::Maker;
  // Perpetual fees are USDC, the quote; anything else cannot be converted here.
  m->fee_asset = fee_token.empty() || fee_token == "USDC" ? FeeAsset::Quote : FeeAsset::Other;
  m->flags = 0;
  m->hdr.venue_seq = tid;
  c.stamp(*m, time);
  return true;
}

// One userFundings entry: {time, coin, usdc, szi, fundingRate}.
[[gnu::noinline]] bool decode_funding(Ctx& c, od::object& f) noexcept {
  std::string_view coin;
  std::string_view usdc;
  std::int64_t time = 0;
  for (auto field : f) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "coin") {
      if (field.value().get_string().get(coin) != sj::SUCCESS) return false;
    } else if (key == "usdc") {
      if (field.value().get_string().get(usdc) != sj::SUCCESS) return false;
    } else if (key == "time") {
      if (field.value().get_int64().get(time) != sj::SUCCESS) return false;
    }
  }
  if (coin.empty() || usdc.empty() || time <= 0) return false;
  ++c.stats->fundings;
  const InstrumentId inst = c.coins->find(coin);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return true;
  }
  const auto amount = parse_rounded<Notional>(usdc);
  if (!amount) return false;
  auto* m = c.reserve<FundingMsg>();
  if (m == nullptr) return true;
  init_header(*m, EventType::Funding, inst, c.venue);
  m->amount = *amount;
  ExecId id;
  id.append(IdText(time).view());
  id.push_back(':');
  id.append(coin.substr(0, id.kCapacity - id.size()));
  m->funding_id = id;
  static_cast<void>(m->asset.assign("USDC"));
  m->flags = 0;
  c.stamp(*m, time);
  return true;
}

// {"isSnapshot":true?,"user":..,"<list>":[..]}: true when the list was read or skipped.
template <class F>
bool decode_user_list(
    Ctx& c, od::object& data, std::string_view list, bool& snapshot, F&& each) noexcept {
  snapshot = false;
  for (auto field : data) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    if (key == "isSnapshot") {
      if (field.value().get_bool().get(snapshot) != sj::SUCCESS) return false;
    } else if (key == list) {
      if (snapshot) return true;  // isSnapshot comes first: skip the replay of history
      od::array arr;
      if (field.value().get_array().get(arr) != sj::SUCCESS) return false;
      for (auto item : arr) {
        od::object o;
        if (item.get_object().get(o) != sj::SUCCESS || !each(c, o)) return false;
        if (c.overflow) return true;
      }
    }
  }
  return true;
}

}  // namespace

PrivateDecodeResult HlPrivateParser::decode(std::string_view json,
                                            Timestamp recv_ts,
                                            Cycles t0,
                                            std::span<std::byte> out) noexcept {
  ++stats_.frames;
  PrivateDecodeResult r;
  od::document doc;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS) return malformed(stats_, r);
  od::object root;
  if (doc.get_object().get(root) != sj::SUCCESS) return malformed(stats_, r);
  std::string_view channel;
  if (root["channel"].get_string().get(channel) != sj::SUCCESS) return malformed(stats_, r);
  r.channel = channel;
  Ctx c{&stats_, &coins_, venue_, recv_ts, t0, out};
  bool ok = true;
  if (channel == "orderUpdates") {
    od::array data;
    if (root["data"].get_array().get(data) != sj::SUCCESS) return malformed(stats_, r);
    for (auto item : data) {
      od::object u;
      if (item.get_object().get(u) != sj::SUCCESS || !decode_order_update(c, u)) {
        ok = false;
        break;
      }
      if (c.overflow) break;
    }
  } else if (channel == "userFills" || channel == "userFundings") {
    od::object data;
    if (root["data"].get_object().get(data) != sj::SUCCESS) return malformed(stats_, r);
    bool snap = false;
    ok = channel == "userFills" ? decode_user_list(c, data, "fills", snap, decode_fill)
                                : decode_user_list(c, data, "fundings", snap, decode_funding);
    if (ok && snap) {
      ++stats_.snapshots_skipped;
      r.snapshot = true;
      r.status = ParseStatus::Ignored;
      return r;
    }
  } else {
    ++stats_.control;
    r.status = ParseStatus::Ignored;
    if (channel == "pong") {
      r.control = ControlOp::Pong;
    } else if (channel == "subscriptionResponse") {
      r.control = ControlOp::Subscribe;
    } else if (channel == "error") {
      r.control = ControlOp::Error;
      std::string_view text;
      if (root["data"].get_string().get(text) == sj::SUCCESS) r.error = text;
      r.status = ParseStatus::Error;
    } else {
      r.control = ControlOp::Other;
      ++stats_.ignored;
    }
    return r;
  }
  if (!ok) return malformed(stats_, r);
  r.count = c.count;
  r.len = c.written;
  if (c.overflow) {
    r.status = ParseStatus::Overflow;
  } else {
    r.status = c.count > 0 ? ParseStatus::Ok : ParseStatus::Ignored;
  }
  return r;
}

}  // namespace fastmm::venues::hyperliquid
