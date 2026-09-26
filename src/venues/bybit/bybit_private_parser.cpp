#include "fastmm/venues/bybit/bybit_private_parser.hpp"

#include "fastmm/venues/bybit/bybit_error_map.hpp"
#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <cstring>

namespace fastmm::venues::bybit {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct BybitPrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

BybitPrivateParser::BybitPrivateParser(const SymbolTable& symbols,
                                       const InstrumentTable& instruments,
                                       VenueId venue,
                                       std::size_t capacity,
                                       BybitCategory category)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue),
      category_(category) {}
BybitPrivateParser::~BybitPrivateParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

[[nodiscard]] ControlOp op_of(std::string_view op) noexcept {
  if (op == "subscribe") return ControlOp::Subscribe;
  if (op == "unsubscribe") return ControlOp::Unsubscribe;
  if (op == "ping" || op == "pong") return ControlOp::Pong;
  if (op == "auth") return ControlOp::Auth;
  return ControlOp::Other;
}

template <class M>
void stamp(M& m, Timestamp recv_ts, Cycles t0, std::int64_t exch_ms) noexcept {
  m.hdr.recv_ts = recv_ts;
  m.hdr.t0_cycles = t0;
  m.hdr.exch_ts = ts_from_ms(exch_ms);
}

// Empty strings ("leavesQty":"" in option examples) parse as zero.
[[nodiscard]] Qty qty_or_zero(std::string_view s) noexcept {
  if (s.empty()) return Qty{};
  const auto q = parse_qty(s);
  return q ? *q : Qty{};
}

[[nodiscard]] Notional notional_or_zero(std::string_view s) noexcept {
  if (s.empty()) return Notional{};
  const auto n = parse_notional(s);
  return n ? *n : Notional{};
}

// decode() is split per frame and item kind: one function holding every simdjson
// lookup made gcc's UBSan instrumentation (null, alignment, object-size) take
// about 15 minutes to compile this file at -O1.
MdDecodeResult malformed(PrivateParserStats& stats, MdDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  r.len = 0;
  return r;
}

[[gnu::noinline]] MdDecodeResult decode_control(PrivateParserStats& stats,
                                                od::object& root,
                                                MdDecodeResult r) noexcept {
  root.reset();
  std::string_view op;
  if (root["op"].get_string().get(op) != sj::SUCCESS) {
    ++stats.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  root.reset();
  bool success = false;
  std::int64_t ret_code = -1;
  if (root["success"].get_bool().get(success) != sj::SUCCESS) {
    root.reset();
    if (root["retCode"].get_int64().get(ret_code) == sj::SUCCESS) success = ret_code == 0;
  }
  root.reset();
  std::string_view msg;
  if (root["ret_msg"].get_string().get(msg) == sj::SUCCESS) {
    r.ret_msg = msg;
  } else {
    root.reset();
    if (root["retMsg"].get_string().get(msg) == sj::SUCCESS) r.ret_msg = msg;
  }
  root.reset();
  std::string_view req;
  if (root["req_id"].get_string().get(req) == sj::SUCCESS) r.req_id = req;
  ++stats.control;
  r.control = op_of(op);
  // Private pongs are {"req_id":..,"op":"pong","args":[..],"conn_id":..} without `success`
  // (https://bybit-exchange.github.io/docs/v5/ws/connect, "How to Send the Heartbeat Packet").
  r.control_success = success || r.control == ControlOp::Pong;
  r.status = r.control_success ? ParseStatus::Ignored : ParseStatus::Error;
  return r;
}

// Output state shared by the per-item decoders.
struct ItemCtx {
  PrivateParserStats* stats;
  const SymbolTable* symbols;
  const InstrumentTable* instruments;
  VenueId venue;
  BybitCategory category;
  Timestamp recv_ts;
  Cycles t0;
  std::int64_t creation;
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

// Next: go on with the following item; Stop: out of room; Malformed: reject the frame.
enum class Item : std::uint8_t { Next, Stop, Malformed };

[[gnu::noinline]] Item decode_wallet(ItemCtx& c, od::object& o) noexcept {
  ++c.stats->wallets;
  od::array coins;
  if (o["coin"].get_array().get(coins) != sj::SUCCESS) return Item::Malformed;
  for (auto coin_val : coins) {
    od::object co;
    if (coin_val.get_object().get(co) != sj::SUCCESS) return Item::Malformed;
    std::string_view coin;
    std::string_view balance;
    if (co["coin"].get_string().get(coin) != sj::SUCCESS) return Item::Malformed;
    if (co["walletBalance"].get_string().get(balance) != sj::SUCCESS) return Item::Malformed;
    auto bal = parse_qty(balance);
    if (!bal) return Item::Malformed;
    // walletBalance is the gross coin balance: `locked` (open spot orders) is part of it and
    // reported separately, and coin equity is walletBalance - spotBorrow + UPL (wallet page,
    // checked 2026-09-14). The position is the net holding, so spot borrows are deducted.
    std::string_view borrow;
    if (co["spotBorrow"].get_string().get(borrow) == sj::SUCCESS && !borrow.empty()) {
      const auto b = parse_qty(borrow);
      if (!b) return Item::Malformed;
      bal = *bal - *b;
    }
    for (const Instrument& inst : *c.instruments) {
      if (inst.venue != c.venue || !iequals_symbol(inst.base.view(), coin)) continue;
      if (!c.room(sizeof(PositionUpdateMsg))) break;
      auto* m = reinterpret_cast<PositionUpdateMsg*>(c.out.data() + c.written);
      init_header(*m, EventType::PositionUpdate, inst.id, c.venue);
      m->qty = *bal;
      stamp(*m, c.recv_ts, c.t0, c.creation);
      c.written += sizeof(PositionUpdateMsg);
      ++c.count;
    }
  }
  return Item::Next;
}

// Fields common to execution and order items.
struct OrderIds {
  InstrumentId inst;
  std::string_view order_id;
  ClientOrderId cl;
  Side side;
};

[[gnu::noinline]] Item decode_execution(ItemCtx& c, od::object& o, const OrderIds& ids) noexcept {
  std::string_view exec_type;
  if (o["execType"].get_string().get(exec_type) != sj::SUCCESS) return Item::Malformed;
  if (exec_type != "Trade") {
    ++c.stats->ignored;
    return Item::Next;
  }
  std::string_view exec_id;
  std::string_view price_s;
  std::string_view qty_s;
  std::string_view fee_s;
  std::string_view fee_ccy;
  std::string_view fee_rate_s;
  std::string_view order_qty_s;
  std::string_view leaves_s;
  std::string_view time_s;
  bool maker = false;
  if (o["execFee"].get_string().get(fee_s) != sj::SUCCESS) fee_s = {};
  if (o["feeCurrency"].get_string().get(fee_ccy) != sj::SUCCESS) fee_ccy = {};
  if (o["feeRate"].get_string().get(fee_rate_s) != sj::SUCCESS) fee_rate_s = {};
  if (o["execId"].get_string().get(exec_id) != sj::SUCCESS) return Item::Malformed;
  if (o["execPrice"].get_string().get(price_s) != sj::SUCCESS) return Item::Malformed;
  if (o["execQty"].get_string().get(qty_s) != sj::SUCCESS) return Item::Malformed;
  if (o["execTime"].get_string().get(time_s) != sj::SUCCESS) time_s = {};
  if (o["isMaker"].get_bool().get(maker) != sj::SUCCESS) maker = false;
  if (o["orderQty"].get_string().get(order_qty_s) != sj::SUCCESS) return Item::Malformed;
  if (o["leavesQty"].get_string().get(leaves_s) != sj::SUCCESS) return Item::Malformed;
  const auto px = parse_price(price_s);
  const auto qty = parse_qty(qty_s);
  if (!px || !qty) return Item::Malformed;
  if (!c.room(sizeof(OrderFillMsg))) return Item::Stop;
  auto* m = c.place<OrderFillMsg>();
  init_header(*m, EventType::OrderFill, ids.inst, c.venue);
  m->cl_ord_id = ids.cl;
  m->venue_order_id.assign(ids.order_id);
  m->exec_id.assign(exec_id);
  m->price = *px;
  m->qty = *qty;
  m->leaves_qty = qty_or_zero(leaves_s);
  m->cum_qty = qty_or_zero(order_qty_s) - m->leaves_qty;
  m->fee = notional_or_zero(fee_s);
  // A linear fee (a rebate is negative) is in the settlement coin, the instrument's quote.
  // Spot fee currency (enum page, "Spot Fee Currency Instruction"): with a positive fee rate
  // (and always for takers) a buy pays in the base coin and a sell in the quote coin; a maker
  // with a negative rate is the other way round. feeCurrency names it when present; recorded
  // testnet executions omit it, so the rule decides then.
  {
    const Instrument& in = c.instruments->get(ids.inst);
    if (m->fee.is_zero()) {
      m->fee_asset = FeeAsset::Quote;
    } else if (!fee_ccy.empty() || c.category == BybitCategory::Linear) {
      if (fee_ccy.empty()) fee_ccy = in.quote.view();
      m->fee_asset = iequals_symbol(in.quote.view(), fee_ccy)  ? FeeAsset::Quote
                     : iequals_symbol(in.base.view(), fee_ccy) ? FeeAsset::Base
                                                               : FeeAsset::Other;
    } else {
      const bool rebate = maker && !fee_rate_s.empty() && fee_rate_s.front() == '-';
      const bool base = (ids.side == Side::Buy) != rebate;
      m->fee_asset = base ? FeeAsset::Base : FeeAsset::Quote;
    }
  }
  m->side = ids.side;
  m->liquidity = maker ? Liquidity::Maker : Liquidity::Taker;
  const auto t = parse_int64(time_s);
  stamp(*m, c.recv_ts, c.t0, t ? *t : c.creation);
  c.written += sizeof(OrderFillMsg);
  ++c.count;
  ++c.stats->executions;
  return Item::Next;
}

[[gnu::noinline]] Item decode_order(ItemCtx& c, od::object& o, const OrderIds& ids) noexcept {
  std::string_view status;
  std::string_view reject;
  std::string_view cum_s;
  std::string_view updated_s;
  if (o["orderStatus"].get_string().get(status) != sj::SUCCESS) return Item::Malformed;
  if (o["cumExecQty"].get_string().get(cum_s) != sj::SUCCESS) cum_s = {};
  if (o["rejectReason"].get_string().get(reject) != sj::SUCCESS) reject = {};
  if (o["updatedTime"].get_string().get(updated_s) != sj::SUCCESS) updated_s = {};
  const auto upd = parse_int64(updated_s);
  const std::int64_t exch_ms = upd ? *upd : c.creation;
  ++c.stats->orders;
  if (status == "New") {
    if (!c.room(sizeof(OrderAckMsg))) return Item::Stop;
    auto* m = c.place<OrderAckMsg>();
    init_header(*m, EventType::OrderAck, ids.inst, c.venue);
    m->cl_ord_id = ids.cl;
    m->venue_order_id.assign(ids.order_id);
    stamp(*m, c.recv_ts, c.t0, exch_ms);
    c.written += sizeof(OrderAckMsg);
    ++c.count;
  } else if (status == "Rejected") {
    if (!c.room(sizeof(OrderRejectMsg))) return Item::Stop;
    auto* m = c.place<OrderRejectMsg>();
    init_header(*m, EventType::OrderReject, ids.inst, c.venue);
    m->cl_ord_id = ids.cl;
    m->reason = map_reject_reason(reject);
    m->venue_code = 0;
    m->text.assign(reject);
    stamp(*m, c.recv_ts, c.t0, exch_ms);
    c.written += sizeof(OrderRejectMsg);
    ++c.count;
  } else if (status == "Cancelled" || status == "PartiallyFilledCanceled") {
    if (!c.room(sizeof(OrderCancelAckMsg))) return Item::Stop;
    auto* m = c.place<OrderCancelAckMsg>();
    init_header(*m, EventType::OrderCancelAck, ids.inst, c.venue);
    m->cl_ord_id = ids.cl;
    m->venue_order_id.assign(ids.order_id);
    m->cum_qty = qty_or_zero(cum_s);
    stamp(*m, c.recv_ts, c.t0, exch_ms);
    c.written += sizeof(OrderCancelAckMsg);
    ++c.count;
  } else if (status == "Deactivated") {
    if (!c.room(sizeof(OrderExpiredMsg))) return Item::Stop;
    auto* m = c.place<OrderExpiredMsg>();
    init_header(*m, EventType::OrderExpired, ids.inst, c.venue);
    m->cl_ord_id = ids.cl;
    m->venue_order_id.assign(ids.order_id);
    m->cum_qty = qty_or_zero(cum_s);
    stamp(*m, c.recv_ts, c.t0, exch_ms);
    c.written += sizeof(OrderExpiredMsg);
    ++c.count;
  } else {
    ++c.stats->ignored;  // PartiallyFilled, Filled (fills via execution), Untriggered, Triggered
  }
  return Item::Next;
}

// A linear position item. One-way mode only: positionIdx 1 / 2 are the two sides of a hedge-mode
// position, which the connector does not trade; they are counted so the venue can say so.
[[gnu::noinline]] Item decode_position(ItemCtx& c, od::object& o) noexcept {
  std::string_view category;
  if (o["category"].get_string().get(category) != sj::SUCCESS) return Item::Malformed;
  if (category != to_string(c.category)) {
    ++c.stats->ignored;
    return Item::Next;
  }
  std::string_view symbol;
  std::string_view side;
  std::string_view size_s;
  std::string_view entry_s;
  std::string_view updated_s;
  std::int64_t idx = 0;
  if (o["symbol"].get_string().get(symbol) != sj::SUCCESS) return Item::Malformed;
  if (o["side"].get_string().get(side) != sj::SUCCESS) side = {};
  if (o["size"].get_string().get(size_s) != sj::SUCCESS) return Item::Malformed;
  if (o["positionIdx"].get_int64().get(idx) != sj::SUCCESS) return Item::Malformed;
  if (o["entryPrice"].get_string().get(entry_s) != sj::SUCCESS) entry_s = {};
  if (o["updatedTime"].get_string().get(updated_s) != sj::SUCCESS) updated_s = {};
  ++c.stats->positions;
  if (idx != 0) {
    ++c.stats->hedge_positions;
    return Item::Next;
  }
  const InstrumentId inst = c.symbols->find(c.venue, symbol);
  if (!inst.valid()) {
    ++c.stats->unknown_symbol;
    return Item::Next;
  }
  const auto size = parse_qty(size_s);
  if (!size) return Item::Malformed;
  if (!c.room(sizeof(PositionUpdateMsg))) return Item::Stop;
  auto* m = c.place<PositionUpdateMsg>();
  init_header(*m, EventType::PositionUpdate, inst, c.venue);
  m->qty = side == "Sell" ? -*size : *size;
  if (!entry_s.empty()) {
    if (const auto px = parse_price(entry_s)) m->avg_px = *px;
  }
  const auto upd = parse_int64(updated_s);
  stamp(*m, c.recv_ts, c.t0, upd ? *upd : c.creation);
  c.written += sizeof(PositionUpdateMsg);
  ++c.count;
  return Item::Next;
}

// One execution or order item: the shared fields, then the topic's decoder.
[[gnu::noinline]] Item decode_order_item(ItemCtx& c, od::object& o, bool is_exec) noexcept {
  std::string_view category;
  if (o["category"].get_string().get(category) != sj::SUCCESS) return Item::Malformed;
  if (category != to_string(c.category)) {
    ++c.stats->ignored;
    return Item::Next;
  }
  std::string_view symbol;
  std::string_view link_id;
  std::string_view side_s;
  OrderIds ids{};
  if (o["symbol"].get_string().get(symbol) != sj::SUCCESS) return Item::Malformed;
  if (o["orderId"].get_string().get(ids.order_id) != sj::SUCCESS) return Item::Malformed;
  if (o["orderLinkId"].get_string().get(link_id) != sj::SUCCESS) return Item::Malformed;
  if (o["side"].get_string().get(side_s) != sj::SUCCESS) return Item::Malformed;
  ids.inst = c.symbols->find(c.venue, symbol);
  if (!ids.inst.valid()) {
    ++c.stats->unknown_symbol;
    return Item::Next;
  }
  if (const auto d = decode_cl_ord_id(link_id)) {
    ids.cl = *d;
  } else {
    ++c.stats->foreign_ids;
  }
  ids.side = side_s == "Sell" ? Side::Sell : Side::Buy;
  return is_exec ? decode_execution(c, o, ids) : decode_order(c, o, ids);
}

}  // namespace

MdDecodeResult BybitPrivateParser::decode(std::string_view json,
                                          Timestamp recv_ts,
                                          Cycles t0,
                                          std::span<std::byte> out) noexcept {
  ++stats_.frames;
  MdDecodeResult r;
  if (out.size() < kDecoderScratchBytes) {
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed(stats_, r);

  std::string_view topic;
  if (root["topic"].get_string().get(topic) != sj::SUCCESS) return decode_control(stats_, root, r);

  std::int64_t creation = 0;
  if (root["creationTime"].get_int64().get(creation) != sj::SUCCESS) creation = 0;
  const bool is_order = topic == "order" || topic == "order.spot";
  const bool is_exec = topic == "execution" || topic == "execution.spot";
  const bool is_wallet = topic == "wallet";
  const bool is_position = topic == "position" || topic == "position.linear";
  if (!is_order && !is_exec && !is_wallet && !is_position) {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  od::array data;
  if (root["data"].get_array().get(data) != sj::SUCCESS) return malformed(stats_, r);

  ItemCtx c{&stats_, &symbols_, &instruments_, venue_, category_, recv_ts, t0, creation, out, 0, 0};
  for (auto item : data) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return malformed(stats_, r);
    const Item res = is_wallet     ? decode_wallet(c, o)
                     : is_position ? decode_position(c, o)
                                   : decode_order_item(c, o, is_exec);
    if (res == Item::Malformed) return malformed(stats_, r);
    if (res == Item::Stop) break;
  }
  if (c.count == 0) {
    r.status = ParseStatus::Ignored;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.order_kind = is_wallet || is_position ? OrderEventKind::Position
                                          : (is_exec ? OrderEventKind::Fill : OrderEventKind::Ack);
  r.len = c.written;
  r.count = c.count;
  return r;
}

}  // namespace fastmm::venues::bybit
