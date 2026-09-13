#include "fastmm/venues/bybit/bybit_private_parser.hpp"

#include "fastmm/venues/bybit/bybit_error_map.hpp"
#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>

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
                                       std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
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

}  // namespace

MdDecodeResult BybitPrivateParser::decode(std::string_view json,
                                          Timestamp recv_ts,
                                          Cycles t0,
                                          std::span<std::byte> out) noexcept {
  ++stats_.frames;
  MdDecodeResult r;
  auto malformed = [&]() {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    r.count = 0;
    r.len = 0;
    return r;
  };
  if (out.size() < kDecoderScratchBytes) {
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed();

  std::string_view topic;
  if (root["topic"].get_string().get(topic) != sj::SUCCESS) {
    root.reset();
    std::string_view op;
    if (root["op"].get_string().get(op) != sj::SUCCESS) {
      ++stats_.ignored;
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
    ++stats_.control;
    r.control = op_of(op);
    // Private pongs are {"req_id":..,"op":"pong","args":[..],"conn_id":..} without `success`
    // (https://bybit-exchange.github.io/docs/v5/ws/connect, "How to Send the Heartbeat Packet").
    r.control_success = success || r.control == ControlOp::Pong;
    r.status = r.control_success ? ParseStatus::Ignored : ParseStatus::Error;
    return r;
  }

  std::int64_t creation = 0;
  if (root["creationTime"].get_int64().get(creation) != sj::SUCCESS) creation = 0;
  const bool is_order = topic == "order" || topic == "order.spot";
  const bool is_exec = topic == "execution" || topic == "execution.spot";
  const bool is_wallet = topic == "wallet";
  if (!is_order && !is_exec && !is_wallet) {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  od::array data;
  if (root["data"].get_array().get(data) != sj::SUCCESS) return malformed();

  std::uint32_t written = 0;
  std::uint32_t count = 0;
  auto room = [&](std::size_t n) {
    if (written + n <= out.size()) return true;
    ++stats_.overflow;
    return false;
  };

  for (auto item : data) {
    od::object o;
    if (item.get_object().get(o) != sj::SUCCESS) return malformed();

    if (is_wallet) {
      ++stats_.wallets;
      od::array coins;
      if (o["coin"].get_array().get(coins) != sj::SUCCESS) return malformed();
      for (auto c : coins) {
        od::object co;
        if (c.get_object().get(co) != sj::SUCCESS) return malformed();
        std::string_view coin;
        std::string_view balance;
        if (co["coin"].get_string().get(coin) != sj::SUCCESS) return malformed();
        if (co["walletBalance"].get_string().get(balance) != sj::SUCCESS) return malformed();
        const auto bal = parse_qty(balance);
        if (!bal) return malformed();
        for (const Instrument& inst : instruments_) {
          if (inst.venue != venue_ || !iequals_symbol(inst.base.view(), coin)) continue;
          if (!room(sizeof(PositionUpdateMsg))) break;
          auto* m = reinterpret_cast<PositionUpdateMsg*>(out.data() + written);
          init_header(*m, EventType::PositionUpdate, inst.id, venue_);
          // VERIFY: whether walletBalance already includes `locked` for spot holdings.
          m->qty = *bal;
          stamp(*m, recv_ts, t0, creation);
          written += sizeof(PositionUpdateMsg);
          ++count;
        }
      }
      continue;
    }

    std::string_view category;
    if (o["category"].get_string().get(category) != sj::SUCCESS) return malformed();
    if (category != "spot") {
      ++stats_.ignored;
      continue;
    }
    std::string_view symbol;
    std::string_view order_id;
    std::string_view link_id;
    std::string_view side_s;
    if (o["symbol"].get_string().get(symbol) != sj::SUCCESS) return malformed();
    if (o["orderId"].get_string().get(order_id) != sj::SUCCESS) return malformed();
    if (o["orderLinkId"].get_string().get(link_id) != sj::SUCCESS) return malformed();
    if (o["side"].get_string().get(side_s) != sj::SUCCESS) return malformed();
    const InstrumentId inst = symbols_.find(venue_, symbol);
    if (!inst.valid()) {
      ++stats_.unknown_symbol;
      continue;
    }
    ClientOrderId cl{};
    if (const auto d = decode_cl_ord_id(link_id)) {
      cl = *d;
    } else {
      ++stats_.foreign_ids;
    }
    const Side side = side_s == "Sell" ? Side::Sell : Side::Buy;

    if (is_exec) {
      std::string_view exec_type;
      if (o["execType"].get_string().get(exec_type) != sj::SUCCESS) return malformed();
      if (exec_type != "Trade") {
        ++stats_.ignored;
        continue;
      }
      std::string_view exec_id;
      std::string_view price_s;
      std::string_view qty_s;
      std::string_view fee_s;
      std::string_view order_qty_s;
      std::string_view leaves_s;
      std::string_view time_s;
      bool maker = false;
      if (o["execFee"].get_string().get(fee_s) != sj::SUCCESS) fee_s = {};
      if (o["execId"].get_string().get(exec_id) != sj::SUCCESS) return malformed();
      if (o["execPrice"].get_string().get(price_s) != sj::SUCCESS) return malformed();
      if (o["execQty"].get_string().get(qty_s) != sj::SUCCESS) return malformed();
      if (o["execTime"].get_string().get(time_s) != sj::SUCCESS) time_s = {};
      if (o["isMaker"].get_bool().get(maker) != sj::SUCCESS) maker = false;
      if (o["orderQty"].get_string().get(order_qty_s) != sj::SUCCESS) return malformed();
      if (o["leavesQty"].get_string().get(leaves_s) != sj::SUCCESS) return malformed();
      const auto px = parse_price(price_s);
      const auto qty = parse_qty(qty_s);
      if (!px || !qty) return malformed();
      if (!room(sizeof(OrderFillMsg))) break;
      auto* m = reinterpret_cast<OrderFillMsg*>(out.data() + written);
      init_header(*m, EventType::OrderFill, inst, venue_);
      m->cl_ord_id = cl;
      m->venue_order_id.assign(order_id);
      m->exec_id.assign(exec_id);
      m->price = *px;
      m->qty = *qty;
      m->leaves_qty = qty_or_zero(leaves_s);
      m->cum_qty = qty_or_zero(order_qty_s) - m->leaves_qty;
      if (const auto fee = parse_notional(fee_s)) m->fee = *fee;
      m->side = side;
      m->liquidity = maker ? Liquidity::Maker : Liquidity::Taker;
      const auto t = parse_int64(time_s);
      stamp(*m, recv_ts, t0, t ? *t : creation);
      written += sizeof(OrderFillMsg);
      ++count;
      ++stats_.executions;
      continue;
    }

    // order topic
    std::string_view status;
    std::string_view reject;
    std::string_view cum_s;
    std::string_view updated_s;
    if (o["orderStatus"].get_string().get(status) != sj::SUCCESS) return malformed();
    if (o["cumExecQty"].get_string().get(cum_s) != sj::SUCCESS) cum_s = {};
    if (o["rejectReason"].get_string().get(reject) != sj::SUCCESS) reject = {};
    if (o["updatedTime"].get_string().get(updated_s) != sj::SUCCESS) updated_s = {};
    const auto upd = parse_int64(updated_s);
    const std::int64_t exch_ms = upd ? *upd : creation;
    ++stats_.orders;
    if (status == "New") {
      if (!room(sizeof(OrderAckMsg))) break;
      auto* m = reinterpret_cast<OrderAckMsg*>(out.data() + written);
      init_header(*m, EventType::OrderAck, inst, venue_);
      m->cl_ord_id = cl;
      m->venue_order_id.assign(order_id);
      stamp(*m, recv_ts, t0, exch_ms);
      written += sizeof(OrderAckMsg);
      ++count;
    } else if (status == "Rejected") {
      if (!room(sizeof(OrderRejectMsg))) break;
      auto* m = reinterpret_cast<OrderRejectMsg*>(out.data() + written);
      init_header(*m, EventType::OrderReject, inst, venue_);
      m->cl_ord_id = cl;
      m->reason = map_reject_reason(reject);
      m->venue_code = 0;
      m->text.assign(reject);
      stamp(*m, recv_ts, t0, exch_ms);
      written += sizeof(OrderRejectMsg);
      ++count;
    } else if (status == "Cancelled" || status == "PartiallyFilledCanceled") {
      if (!room(sizeof(OrderCancelAckMsg))) break;
      auto* m = reinterpret_cast<OrderCancelAckMsg*>(out.data() + written);
      init_header(*m, EventType::OrderCancelAck, inst, venue_);
      m->cl_ord_id = cl;
      m->venue_order_id.assign(order_id);
      m->cum_qty = qty_or_zero(cum_s);
      stamp(*m, recv_ts, t0, exch_ms);
      written += sizeof(OrderCancelAckMsg);
      ++count;
    } else if (status == "Deactivated") {
      if (!room(sizeof(OrderExpiredMsg))) break;
      auto* m = reinterpret_cast<OrderExpiredMsg*>(out.data() + written);
      init_header(*m, EventType::OrderExpired, inst, venue_);
      m->cl_ord_id = cl;
      m->venue_order_id.assign(order_id);
      m->cum_qty = qty_or_zero(cum_s);
      stamp(*m, recv_ts, t0, exch_ms);
      written += sizeof(OrderExpiredMsg);
      ++count;
    } else {
      ++stats_.ignored;  // PartiallyFilled, Filled (fills via execution), Untriggered, Triggered
    }
  }
  if (count == 0) {
    r.status = ParseStatus::Ignored;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.order_kind =
      is_wallet ? OrderEventKind::Position : (is_exec ? OrderEventKind::Fill : OrderEventKind::Ack);
  r.len = written;
  r.count = count;
  return r;
}

}  // namespace fastmm::venues::bybit
