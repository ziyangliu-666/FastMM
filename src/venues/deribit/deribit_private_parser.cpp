#include "fastmm/venues/deribit/deribit_private_parser.hpp"

#include "fastmm/venues/deribit/deribit_json.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace fastmm::venues::deribit {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct DeribitPrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

DeribitPrivateParser::DeribitPrivateParser(const SymbolTable& symbols,
                                           const InstrumentTable& instruments,
                                           VenueId venue,
                                           std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
DeribitPrivateParser::~DeribitPrivateParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

template <class F, class V>
bool fixed_of(V&& v, F& out) noexcept {
  std::string_view tok;
  if (std::forward<V>(v).raw_json_token().get(tok) != sj::SUCCESS) return false;
  const auto f = json_fixed<F>(tok);
  if (!f) return false;
  out = *f;
  return true;
}

void stamp(EventHeader& h, Timestamp recv_ts, Cycles t0, std::int64_t exch_ms) noexcept {
  h.recv_ts = recv_ts;
  h.t0_cycles = t0;
  h.exch_ts = Timestamp{exch_ms * 1'000'000};
}

void read_order_result(od::object& o, OrderResult& out) noexcept {
  out.present = true;
  std::string_view s;
  if (o["order_id"].get_string().get(s) == sj::SUCCESS) out.order_id = s;
  if (o["order_state"].get_string().get(s) == sj::SUCCESS) out.order_state = s;
  if (o["label"].get_string().get(s) == sj::SUCCESS) out.label = s;
  if (o["instrument_name"].get_string().get(s) == sj::SUCCESS) out.instrument_name = s;
  static_cast<void>(fixed_of(o["amount"], out.amount));
  static_cast<void>(fixed_of(o["filled_amount"], out.filled_amount));
}

// decode() is split per frame and item kind: one function holding every simdjson
// lookup made gcc's UBSan instrumentation (null, alignment, object-size) take
// minutes to compile this file at -O1.
PrivateDecodeResult malformed(PrivateParserStats& stats, PrivateDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  r.len = 0;
  return r;
}

// Output state shared by the per-item decoders.
struct ItemCtx {
  PrivateParserStats* stats;
  const SymbolTable* symbols;
  const InstrumentTable* instruments;
  VenueId venue;
  Timestamp recv_ts;
  Cycles t0;
  std::span<std::byte> out;
  std::uint32_t written;
  std::uint32_t count;

  bool room(std::size_t n) noexcept {
    if (written + n <= out.size()) return true;
    ++stats->overflow;
    return false;
  }
  // The output buffer is a scratch buffer the venue reuses for every frame, so each message is
  // zeroed before it is filled: a field a decoder leaves alone must not carry over from the
  // previous decode.
  template <class M>
  M* place() noexcept {
    std::byte* p = out.data() + written;
    std::memset(p, 0, sizeof(M));
    return reinterpret_cast<M*>(p);
  }
};

// Fields common to user.orders and user.trades objects.
struct ItemIds {
  InstrumentId inst;
  Qty csize;
  std::string_view order_id;
  ClientOrderId cl;
};

// The item decoders return false if the object is malformed.
[[gnu::noinline]] bool decode_trade_item(ItemCtx& c, od::object& o, const ItemIds& ids) noexcept {
  std::string_view trade_id;
  std::string_view direction;
  std::string_view liquidity;
  std::string_view fee_ccy;
  std::int64_t ts = 0;
  Price px{};
  Qty amount{};
  Notional fee{};
  if (o["trade_id"].get_string().get(trade_id) != sj::SUCCESS ||
      o["direction"].get_string().get(direction) != sj::SUCCESS || !fixed_of(o["price"], px) ||
      !fixed_of(o["amount"], amount))
    return false;
  if (o["timestamp"].get_int64().get(ts) != sj::SUCCESS) ts = 0;
  if (o["liquidity"].get_string().get(liquidity) != sj::SUCCESS) liquidity = {};
  static_cast<void>(fixed_of(o["fee"], fee));
  if (o["fee_currency"].get_string().get(fee_ccy) != sj::SUCCESS) fee_ccy = {};
  if (!c.room(sizeof(OrderFillMsg))) return true;
  auto* m = c.place<OrderFillMsg>();
  init_header(*m, EventType::OrderFill, ids.inst, c.venue);
  m->cl_ord_id = ids.cl;
  m->venue_order_id.assign(ids.order_id);
  m->exec_id.assign(trade_id);
  m->price = px;
  m->qty = amount_to_contracts(amount, ids.csize);
  m->cum_qty = Qty{};
  m->leaves_qty = Qty{};
  m->fee = fee;
  // Deribit charges the fee in the instrument's settlement currency and names it in
  // fee_currency: the quote coin for linear instruments, the base coin for inverse ones. BTC
  // options are quoted in BTC (base == quote), so their fee is a quote amount; the base-coin fee
  // of an inverse future is neither a quote amount nor a number of contracts, so the engine
  // cannot book it (Other: counted, not converted).
  {
    const Instrument& in = c.instruments->get(ids.inst);
    if (fee_ccy.empty()) fee_ccy = in.inverse() ? in.base.view() : in.quote.view();
    m->fee_asset = fee.is_zero() || iequals_symbol(in.quote.view(), fee_ccy)  ? FeeAsset::Quote
                   : !in.inverse() && iequals_symbol(in.base.view(), fee_ccy) ? FeeAsset::Base
                                                                              : FeeAsset::Other;
  }
  m->side = direction == "sell" ? Side::Sell : Side::Buy;
  m->liquidity = Liquidity::Unknown;
  if (liquidity == "M") m->liquidity = Liquidity::Maker;
  if (liquidity == "T") m->liquidity = Liquidity::Taker;
  stamp(m->hdr, c.recv_ts, c.t0, ts);
  c.written += sizeof(OrderFillMsg);
  ++c.count;
  ++c.stats->trades;
  return true;
}

[[gnu::noinline]] bool decode_order_item(ItemCtx& c, od::object& o, const ItemIds& ids) noexcept {
  std::string_view state;
  if (o["order_state"].get_string().get(state) != sj::SUCCESS) return false;
  Qty filled{};
  static_cast<void>(fixed_of(o["filled_amount"], filled));
  std::int64_t ts = 0;
  if (o["last_update_timestamp"].get_int64().get(ts) != sj::SUCCESS) ts = 0;
  ++c.stats->orders;
  if (state == "open") {
    if (!c.room(sizeof(OrderAckMsg))) return true;
    auto* m = c.place<OrderAckMsg>();
    init_header(*m, EventType::OrderAck, ids.inst, c.venue);
    m->cl_ord_id = ids.cl;
    m->venue_order_id.assign(ids.order_id);
    stamp(m->hdr, c.recv_ts, c.t0, ts);
    c.written += sizeof(OrderAckMsg);
    ++c.count;
  } else if (state == "cancelled") {
    if (!c.room(sizeof(OrderCancelAckMsg))) return true;
    auto* m = c.place<OrderCancelAckMsg>();
    init_header(*m, EventType::OrderCancelAck, ids.inst, c.venue);
    m->cl_ord_id = ids.cl;
    m->venue_order_id.assign(ids.order_id);
    m->cum_qty = amount_to_contracts(filled, ids.csize);
    stamp(m->hdr, c.recv_ts, c.t0, ts);
    c.written += sizeof(OrderCancelAckMsg);
    ++c.count;
  } else if (state == "rejected") {
    if (!c.room(sizeof(OrderRejectMsg))) return true;
    auto* m = c.place<OrderRejectMsg>();
    init_header(*m, EventType::OrderReject, ids.inst, c.venue);
    m->cl_ord_id = ids.cl;
    m->reason = RejectReason::VenueReject;
    m->venue_code = 0;
    m->text.assign("rejected");
    stamp(m->hdr, c.recv_ts, c.t0, ts);
    c.written += sizeof(OrderRejectMsg);
    ++c.count;
  } else {
    ++c.stats->ignored;  // filled (fills via user.trades), untriggered, triggered
  }
  return true;
}

// One order or trade object -> at most one event.
[[gnu::noinline]] bool decode_item(ItemCtx& c, od::object& o, bool is_trades) noexcept {
  std::string_view instrument_name;
  std::string_view label;
  ItemIds ids{};
  if (o["instrument_name"].get_string().get(instrument_name) != sj::SUCCESS ||
      o["order_id"].get_string().get(ids.order_id) != sj::SUCCESS)
    return false;
  if (o["label"].get_string().get(label) != sj::SUCCESS) label = {};
  ids.inst = c.symbols->find(c.venue, instrument_name);
  if (!ids.inst.valid() || !c.instruments->contains(ids.inst)) {
    ++c.stats->unknown_symbol;
    return true;
  }
  ids.csize = c.instruments->get(ids.inst).contract_multiplier;
  if (const auto d = decode_cl_ord_id(label)) {
    ids.cl = *d;
  } else {
    ++c.stats->foreign_ids;
  }
  return is_trades ? decode_trade_item(c, o, ids) : decode_order_item(c, o, ids);
}

[[gnu::noinline]] PrivateDecodeResult decode_notification(ItemCtx& c,
                                                          od::object& root,
                                                          PrivateDecodeResult r) noexcept {
  PrivateParserStats& stats = *c.stats;
  r.frame = FrameKind::Notification;
  root.reset();
  od::object params;
  if (root["params"].get_object().get(params) != sj::SUCCESS) return malformed(stats, r);
  std::string_view channel;
  if (params["channel"].get_string().get(channel) != sj::SUCCESS) return malformed(stats, r);
  const bool is_orders = channel.starts_with("user.orders.");
  const bool is_trades = channel.starts_with("user.trades.");
  if (!is_orders && !is_trades) {
    ++stats.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  od::value data;
  if (params["data"].get(data) != sj::SUCCESS) return malformed(stats, r);

  od::json_type type{};
  if (data.type().get(type) != sj::SUCCESS) return malformed(stats, r);
  if (type == od::json_type::object) {
    od::object o;
    if (data.get_object().get(o) != sj::SUCCESS) return malformed(stats, r);
    if (!decode_item(c, o, is_trades)) return malformed(stats, r);
  } else if (type == od::json_type::array) {
    od::array arr;
    if (data.get_array().get(arr) != sj::SUCCESS) return malformed(stats, r);
    for (auto item : arr) {
      od::object o;
      if (item.get_object().get(o) != sj::SUCCESS) return malformed(stats, r);
      if (!decode_item(c, o, is_trades)) return malformed(stats, r);
    }
  } else {
    return malformed(stats, r);
  }
  if (c.count == 0) {
    r.status = ParseStatus::Ignored;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.order_kind = is_trades ? OrderEventKind::Fill : OrderEventKind::Ack;
  r.len = c.written;
  r.count = c.count;
  return r;
}

[[gnu::noinline]] void read_result_object(od::object& ro, PrivateDecodeResult& r) noexcept {
  od::object order;
  if (ro["order"].get_object().get(order) == sj::SUCCESS) {
    read_order_result(order, r.order);  // private/buy, private/sell, private/edit
    return;
  }
  ro.reset();
  std::string_view s;
  if (ro["order_id"].get_string().get(s) == sj::SUCCESS) {
    ro.reset();
    read_order_result(ro, r.order);  // private/cancel returns the order itself
    return;
  }
  ro.reset();
  if (ro["access_token"].get_string().get(s) == sj::SUCCESS) {
    r.auth.present = true;
    r.auth.access_token = s;
    ro.reset();
    if (ro["refresh_token"].get_string().get(s) == sj::SUCCESS) r.auth.refresh_token = s;
    ro.reset();
    std::int64_t e = 0;
    if (ro["expires_in"].get_int64().get(e) == sj::SUCCESS) r.auth.expires_in = e;
  }
}

// JSON-RPC response.
[[gnu::noinline]] PrivateDecodeResult decode_response(PrivateParserStats& stats,
                                                      od::object& root,
                                                      PrivateDecodeResult r) noexcept {
  root.reset();
  {
    od::value idv;
    if (root["id"].get(idv) != sj::SUCCESS) {
      ++stats.ignored;
      r.status = ParseStatus::Ignored;
      return r;
    }
    std::int64_t num = 0;
    std::string_view text;
    if (idv.get_int64().get(num) == sj::SUCCESS) {
      r.rpc.id = num;
    } else if (idv.get_string().get(text) == sj::SUCCESS) {
      r.rpc.id_text = text;
    }
  }
  r.frame = FrameKind::Response;
  ++stats.responses;
  root.reset();
  {
    od::object err;
    if (root["error"].get_object().get(err) == sj::SUCCESS) {
      r.rpc.is_error = true;
      std::int64_t code = 0;
      if (err["code"].get_int64().get(code) == sj::SUCCESS) r.rpc.error_code = code;
      std::string_view msg;
      if (err["message"].get_string().get(msg) == sj::SUCCESS) r.rpc.error_message = msg;
      od::object data;
      std::string_view reason;
      if (err["data"].get_object().get(data) == sj::SUCCESS &&
          data["reason"].get_string().get(reason) == sj::SUCCESS)
        r.rpc.error_reason = reason;
      r.status = ParseStatus::Error;
      return r;
    }
  }
  root.reset();
  od::value result;
  if (root["result"].get(result) != sj::SUCCESS) {
    r.status = ParseStatus::Ignored;
    return r;
  }
  od::json_type type{};
  if (result.type().get(type) != sj::SUCCESS) return malformed(stats, r);
  switch (type) {
    case od::json_type::object: {
      od::object ro;
      if (result.get_object().get(ro) != sj::SUCCESS) return malformed(stats, r);
      read_result_object(ro, r);
      break;
    }
    case od::json_type::array: {
      od::array arr;
      if (result.get_array().get(arr) != sj::SUCCESS) return malformed(stats, r);
      std::uint32_t n = 0;
      for (auto item : arr) {
        static_cast<void>(item);
        ++n;
      }
      r.result_items = n;
      break;
    }
    case od::json_type::number: {
      std::int64_t v = 0;
      if (result.get_int64().get(v) == sj::SUCCESS) r.result_int = v;
      break;
    }
    default:
      break;
  }
  r.status = ParseStatus::Ignored;
  return r;
}

// False if the order is malformed.
[[gnu::noinline]] bool read_open_order(od::object& o, OpenOrderRecord& rec) noexcept {
  if (o["instrument_name"].get_string().get(rec.instrument_name) != sj::SUCCESS ||
      o["order_id"].get_string().get(rec.order_id) != sj::SUCCESS ||
      o["direction"].get_string().get(rec.direction) != sj::SUCCESS)
    return false;
  if (o["label"].get_string().get(rec.label) != sj::SUCCESS) rec.label = {};
  if (o["order_state"].get_string().get(rec.order_state) != sj::SUCCESS) rec.order_state = {};
  if (!fixed_of(o["price"], rec.price)) rec.price = Price{};  // "market_price" for triggers
  if (!fixed_of(o["amount"], rec.amount)) return false;
  if (!fixed_of(o["filled_amount"], rec.filled_amount)) rec.filled_amount = Qty{};
  return true;
}

}  // namespace

PrivateDecodeResult DeribitPrivateParser::decode(std::string_view json,
                                                 Timestamp recv_ts,
                                                 Cycles t0,
                                                 std::span<std::byte> out) noexcept {
  ++stats_.frames;
  PrivateDecodeResult r;
  if (out.size() < kDecoderScratchBytes) {
    ++stats_.overflow;
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed(stats_, r);

  std::string_view method;
  if (root["method"].get_string().get(method) != sj::SUCCESS)
    return decode_response(stats_, root, r);
  if (method == "heartbeat") {
    root.reset();
    od::object params;
    std::string_view type;
    const bool test = root["params"].get_object().get(params) == sj::SUCCESS &&
                      params["type"].get_string().get(type) == sj::SUCCESS &&
                      type == "test_request";
    ++stats_.heartbeats;
    r.frame = test ? FrameKind::TestRequest : FrameKind::Heartbeat;
    r.status = ParseStatus::Ignored;
    return r;
  }
  if (method != "subscription") {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  ItemCtx c{&stats_, &symbols_, &instruments_, venue_, recv_ts, t0, out, 0, 0};
  return decode_notification(c, root, r);
}

ParseStatus DeribitPrivateParser::decode_open_orders(
    std::string_view json, const std::function<void(const OpenOrderRecord&)>& fn) noexcept {
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return ParseStatus::Malformed;
  {
    od::value err;
    if (root["error"].get(err) == sj::SUCCESS) return ParseStatus::Error;
  }
  root.reset();
  od::array list;
  if (root["result"].get_array().get(list) != sj::SUCCESS) return ParseStatus::Malformed;
  for (auto item : list) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return ParseStatus::Malformed;
    OpenOrderRecord rec;
    if (!read_open_order(o, rec)) return ParseStatus::Malformed;
    fn(rec);
  }
  return ParseStatus::Ok;
}

}  // namespace fastmm::venues::deribit
