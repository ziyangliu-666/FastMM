#include "fastmm/venues/okx/okx_private_parser.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/okx/okx_error_map.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <cstring>

namespace fastmm::venues::okx {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct OkxPrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

OkxPrivateParser::OkxPrivateParser(const SymbolTable& symbols,
                                   const InstrumentTable& instruments,
                                   VenueId venue,
                                   std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
OkxPrivateParser::~OkxPrivateParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

template <class M>
void stamp(M& m, Timestamp recv_ts, Cycles t0, std::int64_t exch_ms) noexcept {
  m.hdr.recv_ts = recv_ts;
  m.hdr.t0_cycles = t0;
  m.hdr.exch_ts = ts_from_ms(exch_ms);
}

[[nodiscard]] Qty qty_or_zero(std::string_view s) noexcept {
  if (s.empty()) return Qty{};
  const auto q = parse_qty(s);
  return q ? *q : Qty{};
}

[[nodiscard]] std::int64_t ms_or(std::string_view s, std::int64_t def) noexcept {
  const auto v = parse_int64(s);
  return v ? *v : def;
}

PrivateDecodeResult malformed(PrivateParserStats& stats, PrivateDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  r.len = 0;
  return r;
}

[[nodiscard]] ControlOp event_of(std::string_view e) noexcept {
  if (e == "subscribe") return ControlOp::Subscribe;
  if (e == "unsubscribe") return ControlOp::Unsubscribe;
  if (e == "login") return ControlOp::Login;
  if (e == "error") return ControlOp::Error;
  if (e == "channel-conn-count" || e == "channel-conn-count-error")
    return ControlOp::ChannelConnCount;
  if (e == "notice") return ControlOp::Notice;
  return ControlOp::Other;
}

[[gnu::noinline]] PrivateDecodeResult decode_event(PrivateParserStats& stats,
                                                   od::object& root,
                                                   std::string_view event,
                                                   PrivateDecodeResult r) noexcept {
  ++stats.control;
  r.control = event_of(event);
  root.reset();
  od::object arg;
  if (root["arg"].get_object().get(arg) == sj::SUCCESS) {
    std::string_view ch;
    if (arg["channel"].get_string().get(ch) == sj::SUCCESS) r.channel = ch;
  }
  root.reset();
  std::string_view code;
  if (root["code"].get_string().get(code) == sj::SUCCESS) r.code = parse_code(code);
  root.reset();
  std::string_view msg;
  if (root["msg"].get_string().get(msg) == sj::SUCCESS) r.msg = msg;
  const bool failed = r.control == ControlOp::Error ||
                      (r.control == ControlOp::Login && r.code != 0) ||
                      event == "channel-conn-count-error";
  r.control_success = !failed;
  r.status = failed ? ParseStatus::Error : ParseStatus::Ignored;
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
  // The scratch buffer is reused for every frame: each message is zeroed before it is filled.
  template <class M>
  M* place() noexcept {
    std::byte* p = out.data() + written;
    std::memset(p, 0, sizeof(M));
    return reinterpret_cast<M*>(p);
  }
};

enum class Item : std::uint8_t { Next, Stop, Malformed };

// The fields of one orders-channel item, read in one pass (documented order).
struct OrderItem {
  std::string_view inst_id;
  std::string_view ord_id;
  std::string_view cl_ord_id;
  std::string_view sz;
  std::string_view side;
  std::string_view state;
  std::string_view acc_fill_sz;
  std::string_view fill_px;
  std::string_view fill_sz;
  std::string_view trade_id;
  std::string_view fill_time;
  std::string_view fill_fee;
  std::string_view fill_fee_ccy;
  std::string_view exec_type;
  std::string_view u_time;
  std::string_view req_id;
  std::string_view amend_result;
  std::string_view cancel_source;
  std::string_view code;
  std::string_view msg;
};

[[gnu::noinline]] bool read_order_item(od::object& o, OrderItem& it) noexcept {
  // One pass over the fields: an On-Demand object is read front to back.
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    std::string_view* dst = nullptr;
    switch (key.size()) {
      case 2:
        if (key == "sz") dst = &it.sz;
        break;
      case 4:
        if (key == "side") dst = &it.side;
        if (key == "code") dst = &it.code;
        break;
      case 5:
        if (key == "ordId") dst = &it.ord_id;
        if (key == "state") dst = &it.state;
        if (key == "reqId") dst = &it.req_id;
        if (key == "uTime") dst = &it.u_time;
        break;
      case 6:
        if (key == "instId") dst = &it.inst_id;
        if (key == "fillPx") dst = &it.fill_px;
        if (key == "fillSz") dst = &it.fill_sz;
        break;
      case 7:
        if (key == "clOrdId") dst = &it.cl_ord_id;
        if (key == "tradeId") dst = &it.trade_id;
        if (key == "fillFee") dst = &it.fill_fee;
        break;
      case 3:
        if (key == "msg") dst = &it.msg;
        break;
      case 8:
        if (key == "fillTime") dst = &it.fill_time;
        if (key == "execType") dst = &it.exec_type;
        break;
      case 9:
        if (key == "accFillSz") dst = &it.acc_fill_sz;
        break;
      case 10:
        if (key == "fillFeeCcy") dst = &it.fill_fee_ccy;
        break;
      case 11:
        if (key == "amendResult") dst = &it.amend_result;
        break;
      case 12:
        if (key == "cancelSource") dst = &it.cancel_source;
        break;
      default:
        break;
    }
    if (dst == nullptr) continue;
    if (field.value().get_string().get(*dst) != sj::SUCCESS) return false;
  }
  return true;
}

[[nodiscard]] FeeAsset fee_asset_of(const Instrument& in, std::string_view ccy) noexcept {
  if (ccy.empty() || iequals_symbol(in.quote.view(), ccy)) return FeeAsset::Quote;
  if (iequals_symbol(in.base.view(), ccy)) return FeeAsset::Base;
  return FeeAsset::Other;
}

// cancelSource values that end an order on its own terms rather than by a cancel request.
[[nodiscard]] bool expired_source(std::string_view s) noexcept {
  return s == "13" || s == "14" || s == "31";  // FOK, IOC remainder, post-only would take
}

[[gnu::noinline]] Item decode_order(ItemCtx& c, od::object& o) noexcept {
  OrderItem it;
  if (!read_order_item(o, it)) return Item::Malformed;
  if (it.inst_id.empty() || it.ord_id.empty() || it.state.empty()) return Item::Malformed;
  ++c.stats->orders;
  const InstrumentId inst = c.symbols->find(c.venue, it.inst_id);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return Item::Next;
  }
  ClientOrderId cl{};
  if (const auto d = decode_cl_ord_id(it.cl_ord_id)) {
    cl = *d;
  } else {
    ++c.stats->foreign_ids;
  }
  const Side side = it.side == "sell" ? Side::Sell : Side::Buy;
  const std::int64_t upd = ms_or(it.u_time, 0);
  const Qty acc = qty_or_zero(it.acc_fill_sz);
  // A fill first: the same push may also end the order.
  const Qty fill_sz = qty_or_zero(it.fill_sz);
  if (fill_sz.is_positive() && !it.trade_id.empty()) {
    const auto px = parse_price(it.fill_px);
    if (!px) return Item::Malformed;
    if (!c.room(sizeof(OrderFillMsg))) return Item::Stop;
    auto* m = c.place<OrderFillMsg>();
    init_header(*m, EventType::OrderFill, inst, c.venue);
    m->cl_ord_id = cl;
    m->venue_order_id.assign(it.ord_id);
    m->exec_id.assign(it.trade_id);
    m->price = *px;
    m->qty = fill_sz;
    m->cum_qty = acc;
    const Qty total = qty_or_zero(it.sz);
    m->leaves_qty = total > acc ? total - acc : Qty{};
    // fillFee: negative is charged, positive a rebate; the engine's fee is positive when paid.
    Notional fee{};
    if (!it.fill_fee.empty()) {
      const auto f = parse_notional(it.fill_fee);
      if (!f) return Item::Malformed;
      fee = Notional{} - *f;
    }
    m->fee = fee;
    m->fee_asset =
        fee.is_zero() ? FeeAsset::Quote : fee_asset_of(c.instruments->get(inst), it.fill_fee_ccy);
    m->side = side;
    m->liquidity = it.exec_type == "M" ? Liquidity::Maker : Liquidity::Taker;
    stamp(*m, c.recv_ts, c.t0, ms_or(it.fill_time, upd));
    c.written += sizeof(OrderFillMsg);
    ++c.count;
    ++c.stats->fills;
  }
  // The result of an amend: the id the amend was sent under is in reqId.
  if (!it.amend_result.empty() && !it.req_id.empty()) {
    ++c.stats->amends;
    const auto amended = decode_cl_ord_id(it.req_id);
    if (!amended) {
      ++c.stats->foreign_ids;
      return Item::Next;
    }
    if (it.amend_result == "0") {
      if (!c.room(sizeof(OrderAckMsg))) return Item::Stop;
      auto* m = c.place<OrderAckMsg>();
      init_header(*m, EventType::OrderAck, inst, c.venue);
      m->cl_ord_id = *amended;
      m->venue_order_id.assign(it.ord_id);
      m->flags = OrderAckMsg::kAmendedInPlace;
      stamp(*m, c.recv_ts, c.t0, upd);
      c.written += sizeof(OrderAckMsg);
      ++c.count;
      return Item::Next;
    }
    if (it.amend_result == "-1") {
      if (!c.room(sizeof(OrderRejectMsg))) return Item::Stop;
      auto* m = c.place<OrderRejectMsg>();
      init_header(*m, EventType::OrderReject, inst, c.venue);
      m->cl_ord_id = *amended;
      const int code = parse_code(it.code);
      m->reason = map_error(code, it.msg).reason;
      m->venue_code = code;
      m->text.assign(it.msg);
      stamp(*m, c.recv_ts, c.t0, upd);
      c.written += sizeof(OrderRejectMsg);
      ++c.count;
      return Item::Next;
    }
    // "1": the order was cancelled because the amend failed (cxlOnFail); the state says so below.
  }
  if (it.state == "live" && !fill_sz.is_positive() && it.amend_result.empty()) {
    if (!c.room(sizeof(OrderAckMsg))) return Item::Stop;
    auto* m = c.place<OrderAckMsg>();
    init_header(*m, EventType::OrderAck, inst, c.venue);
    m->cl_ord_id = cl;
    m->venue_order_id.assign(it.ord_id);
    stamp(*m, c.recv_ts, c.t0, upd);
    c.written += sizeof(OrderAckMsg);
    ++c.count;
  } else if (it.state == "canceled" || it.state == "mmp_canceled") {
    if (expired_source(it.cancel_source)) {
      if (!c.room(sizeof(OrderExpiredMsg))) return Item::Stop;
      auto* m = c.place<OrderExpiredMsg>();
      init_header(*m, EventType::OrderExpired, inst, c.venue);
      m->cl_ord_id = cl;
      m->venue_order_id.assign(it.ord_id);
      m->cum_qty = acc;
      stamp(*m, c.recv_ts, c.t0, upd);
      c.written += sizeof(OrderExpiredMsg);
    } else {
      if (!c.room(sizeof(OrderCancelAckMsg))) return Item::Stop;
      auto* m = c.place<OrderCancelAckMsg>();
      init_header(*m, EventType::OrderCancelAck, inst, c.venue);
      m->cl_ord_id = cl;
      m->venue_order_id.assign(it.ord_id);
      m->cum_qty = acc;
      stamp(*m, c.recv_ts, c.t0, upd);
      c.written += sizeof(OrderCancelAckMsg);
    }
    ++c.count;
  } else if (!fill_sz.is_positive()) {
    ++c.stats->ignored;
  }
  return Item::Next;
}

// A positions item. Net mode only: posSide long / short belong to long/short mode, which the
// connector refuses; they are counted so the venue can say so.
[[gnu::noinline]] Item decode_position(ItemCtx& c, od::object& o) noexcept {
  std::string_view inst_id;
  std::string_view pos_side;
  std::string_view pos;
  std::string_view avg_px;
  std::string_view u_time;
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return Item::Malformed;
    std::string_view* dst = nullptr;
    if (key == "instId") dst = &inst_id;
    if (key == "posSide") dst = &pos_side;
    if (key == "pos") dst = &pos;
    if (key == "avgPx") dst = &avg_px;
    if (key == "uTime") dst = &u_time;
    if (dst == nullptr) continue;
    if (field.value().get_string().get(*dst) != sj::SUCCESS) return Item::Malformed;
  }
  if (inst_id.empty()) return Item::Malformed;
  ++c.stats->positions;
  if (pos_side == "long" || pos_side == "short") {
    ++c.stats->hedge_positions;
    return Item::Next;
  }
  const InstrumentId inst = c.symbols->find(c.venue, inst_id);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return Item::Next;
  }
  const Qty qty = qty_or_zero(pos);
  if (!pos.empty() && !parse_qty(pos)) return Item::Malformed;
  if (!c.room(sizeof(PositionUpdateMsg))) return Item::Stop;
  auto* m = c.place<PositionUpdateMsg>();
  init_header(*m, EventType::PositionUpdate, inst, c.venue);
  m->qty = qty;
  if (!avg_px.empty()) {
    if (const auto px = parse_price(avg_px)) m->avg_px = *px;
  }
  stamp(*m, c.recv_ts, c.t0, ms_or(u_time, 0));
  c.written += sizeof(PositionUpdateMsg);
  ++c.count;
  return Item::Next;
}

[[gnu::noinline]] PrivateDecodeResult decode_data(ItemCtx& c,
                                                  std::string_view channel,
                                                  od::value data_val,
                                                  PrivateDecodeResult r) noexcept {
  PrivateParserStats& stats = *c.stats;
  const bool orders = channel == "orders";
  const bool positions = channel == "positions";
  const bool bal = channel == "balance_and_position";
  if (!orders && !positions && !bal) {
    ++stats.ignored;
    r.status = ParseStatus::Ignored;
    r.positions_snapshot = false;
    return r;
  }
  if (!positions) r.positions_snapshot = false;
  od::array data;
  if (data_val.get_array().get(data) != sj::SUCCESS) return malformed(stats, r);
  for (auto item : data) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return malformed(stats, r);
    if (bal) {
      std::string_view et;
      if (o["eventType"].get_string().get(et) == sj::SUCCESS && et == "funding_fee") {
        r.funding_event = true;
        ++stats.funding_events;
      }
      continue;
    }
    const Item res = positions ? decode_position(c, o) : decode_order(c, o);
    if (res == Item::Malformed) return malformed(stats, r);
    if (res == Item::Stop) break;
  }
  if (c.count == 0) {
    r.status = ParseStatus::Ignored;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.order_kind = positions ? OrderEventKind::Position : OrderEventKind::Fill;
  r.len = c.written;
  r.count = c.count;
  return r;
}

}  // namespace

PrivateDecodeResult OkxPrivateParser::decode(std::string_view json,
                                             Timestamp recv_ts,
                                             Cycles t0,
                                             std::span<std::byte> out) noexcept {
  ++stats_.frames;
  PrivateDecodeResult r;
  if (json == "pong") {
    ++stats_.control;
    r.control = ControlOp::Pong;
    r.control_success = true;
    r.status = ParseStatus::Ignored;
    return r;
  }
  if (out.size() < kDecoderScratchBytes) {
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed(stats_, r);
  // Pushes are {"arg":{..},["eventType":..,"curPage":..,"lastPage":..,]"data":[..]}; events
  // have "event". One pass: the fields before `data` say how to read it.
  std::string_view channel;
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats_, r);
    if (key == "event") {
      std::string_view event;
      if (field.value().get_string().get(event) != sj::SUCCESS) return malformed(stats_, r);
      root.reset();
      return decode_event(stats_, root, event, r);
    }
    if (key == "arg") {
      od::object arg;
      if (field.value().get_object().get(arg) != sj::SUCCESS ||
          arg["channel"].get_string().get(channel) != sj::SUCCESS)
        return malformed(stats_, r);
      continue;
    }
    if (key == "eventType") {
      std::string_view et;
      if (field.value().get_string().get(et) == sj::SUCCESS)
        r.positions_snapshot = et == "snapshot";
      continue;
    }
    if (key != "data") continue;
    ItemCtx c{&stats_, &symbols_, &instruments_, venue_, recv_ts, t0, out, 0, 0};
    return decode_data(c, channel, field.value(), r);
  }
  ++stats_.ignored;
  r.status = ParseStatus::Ignored;
  return r;
}

}  // namespace fastmm::venues::okx
