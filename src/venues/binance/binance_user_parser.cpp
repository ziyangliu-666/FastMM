#include "fastmm/venues/binance/binance_user_parser.hpp"

#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstring>

namespace fastmm::venues::binance {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct BinanceUserParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

BinanceUserParser::BinanceUserParser(const SymbolTable& symbols,
                                     const InstrumentTable& instruments,
                                     VenueId venue,
                                     std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)),
      symbols_(symbols),
      instruments_(instruments),
      venue_(venue) {}
BinanceUserParser::~BinanceUserParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

// Every executionReport field we consume, in wire order.
struct ExecReport {
  std::int64_t event_time = 0;   // E
  std::string_view symbol;       // s
  std::string_view client_id;    // c
  std::string_view side;         // S
  std::string_view orig_client;  // C
  std::string_view exec_type;    // x
  std::string_view status;       // X
  std::string_view reject;       // r
  std::int64_t order_id = 0;     // i
  std::string_view last_qty;     // l
  std::string_view cum_qty;      // z
  std::string_view last_px;      // L
  std::string_view fee;          // n
  std::string_view fee_asset;    // N (may be null)
  std::int64_t tx_time = 0;      // T
  std::int64_t trade_id = -1;    // t
  bool maker = false;            // m
  std::string_view order_qty;    // q
  std::string_view order_px;     // p
};

bool read_exec_report(od::object& ev, ExecReport& r) noexcept {
  // Unordered lookups are used on purpose: the payload order is documented but the fields we
  // skip (P, F, g, ...) sit between the ones we need, and ondemand rewinds transparently.
  if (ev["E"].get_int64().get(r.event_time) != sj::SUCCESS) return false;
  if (ev["s"].get_string().get(r.symbol) != sj::SUCCESS) return false;
  if (ev["c"].get_string().get(r.client_id) != sj::SUCCESS) return false;
  if (ev["S"].get_string().get(r.side) != sj::SUCCESS) return false;
  if (ev["q"].get_string().get(r.order_qty) != sj::SUCCESS) return false;
  if (ev["p"].get_string().get(r.order_px) != sj::SUCCESS) return false;
  if (ev["C"].get_string().get(r.orig_client) != sj::SUCCESS) r.orig_client = {};
  if (ev["x"].get_string().get(r.exec_type) != sj::SUCCESS) return false;
  if (ev["X"].get_string().get(r.status) != sj::SUCCESS) return false;
  if (ev["r"].get_string().get(r.reject) != sj::SUCCESS) r.reject = {};
  if (ev["i"].get_int64().get(r.order_id) != sj::SUCCESS) return false;
  if (ev["l"].get_string().get(r.last_qty) != sj::SUCCESS) return false;
  if (ev["z"].get_string().get(r.cum_qty) != sj::SUCCESS) return false;
  if (ev["L"].get_string().get(r.last_px) != sj::SUCCESS) return false;
  if (ev["n"].get_string().get(r.fee) != sj::SUCCESS) r.fee = {};
  {
    od::value n;
    if (ev["N"].get(n) == sj::SUCCESS) {
      if (n.get_string().get(r.fee_asset) != sj::SUCCESS) r.fee_asset = {};
    }
  }
  if (ev["T"].get_int64().get(r.tx_time) != sj::SUCCESS) return false;
  if (ev["t"].get_int64().get(r.trade_id) != sj::SUCCESS) r.trade_id = -1;
  if (ev["m"].get_bool().get(r.maker) != sj::SUCCESS) r.maker = false;
  return true;
}

void set_venue_order_id(VenueOrderId& out, std::int64_t order_id) noexcept {
  char buf[24];
  const std::size_t n = format_int64(order_id, buf);
  out.assign(std::string_view(buf, n));
}

template <class M>
void stamp(M& m, Timestamp recv_ts, Cycles t0, std::int64_t exch_ms) noexcept {
  m.hdr.recv_ts = recv_ts;
  m.hdr.t0_cycles = t0;
  m.hdr.exch_ts = ts_from_ms(exch_ms);
}

}  // namespace

UserDecodeResult BinanceUserParser::decode(std::string_view json,
                                           Timestamp recv_ts,
                                           Cycles t0,
                                           std::span<std::byte> out) noexcept {
  ++stats_.frames;
  UserDecodeResult r;
  auto ignored = [&]() {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  };
  auto malformed = [&]() {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
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

  // Unwrap: "event" (WS API), "data" (combined stream), or the object itself.
  od::object ev;
  {
    od::value v;
    if (root["event"].get(v) == sj::SUCCESS) {
      if (v.get_object().get(ev) != sj::SUCCESS) return malformed();
    } else {
      root.reset();
      if (root["data"].get(v) == sj::SUCCESS) {
        if (v.get_object().get(ev) != sj::SUCCESS) return malformed();
      } else {
        root.reset();
        ev = root;
      }
    }
  }
  std::string_view type;
  {
    od::value e;
    if (ev["e"].get(e) != sj::SUCCESS || e.get_string().get(type) != sj::SUCCESS)
      return ignored();  // responses ({"id":..,"status":..}) and unknown envelopes
  }

  if (type == "executionReport") {
    ExecReport x;
    if (!read_exec_report(ev, x)) return malformed();
    const InstrumentId inst = symbols_.find(venue_, x.symbol);
    if (!inst.valid()) {
      ++stats_.unknown_symbol;
      r.status = ParseStatus::UnknownSymbol;
      return r;
    }
    // CANCELED reports name the cancelled order in C; every other report in c.
    std::string_view id_text = x.client_id;
    if (x.exec_type == "CANCELED" && !x.orig_client.empty()) id_text = x.orig_client;
    ClientOrderId cl_ord_id{};
    if (const auto decoded = decode_cl_ord_id(id_text)) {
      cl_ord_id = *decoded;
    } else {
      ++stats_.foreign_ids;
    }
    const Side side = x.side == "SELL" ? Side::Sell : Side::Buy;
    const auto cum = parse_qty(x.cum_qty);
    if (!cum) return malformed();
    ++stats_.exec_reports;
    r.count = 1;
    r.status = ParseStatus::Ok;

    if (x.exec_type == "NEW" || x.exec_type == "REPLACED") {
      auto* m = reinterpret_cast<OrderAckMsg*>(out.data());
      init_header(*m, EventType::OrderAck, inst, venue_);
      m->cl_ord_id = cl_ord_id;
      set_venue_order_id(m->venue_order_id, x.order_id);
      stamp(*m, recv_ts, t0, x.tx_time);
      r.order_kind = OrderEventKind::Ack;
      r.len = sizeof(OrderAckMsg);
      return r;
    }
    if (x.exec_type == "REJECTED") {
      auto* m = reinterpret_cast<OrderRejectMsg*>(out.data());
      init_header(*m, EventType::OrderReject, inst, venue_);
      m->cl_ord_id = cl_ord_id;
      m->reason = x.reject == "INSUFFICIENT_BALANCES" ? RejectReason::InsufficientBalance
                                                      : RejectReason::VenueReject;
      m->venue_code = 0;
      m->text.assign(x.reject);
      stamp(*m, recv_ts, t0, x.tx_time);
      r.order_kind = OrderEventKind::Reject;
      r.len = sizeof(OrderRejectMsg);
      return r;
    }
    if (x.exec_type == "CANCELED") {
      auto* m = reinterpret_cast<OrderCancelAckMsg*>(out.data());
      init_header(*m, EventType::OrderCancelAck, inst, venue_);
      m->cl_ord_id = cl_ord_id;
      set_venue_order_id(m->venue_order_id, x.order_id);
      m->cum_qty = *cum;
      stamp(*m, recv_ts, t0, x.tx_time);
      r.order_kind = OrderEventKind::CancelAck;
      r.len = sizeof(OrderCancelAckMsg);
      return r;
    }
    if (x.exec_type == "TRADE") {
      const auto last_qty = parse_qty(x.last_qty);
      const auto last_px = parse_price(x.last_px);
      const auto order_qty = parse_qty(x.order_qty);
      if (!last_qty || !last_px || !order_qty) return malformed();
      auto* m = reinterpret_cast<OrderFillMsg*>(out.data());
      init_header(*m, EventType::OrderFill, inst, venue_);
      m->cl_ord_id = cl_ord_id;
      set_venue_order_id(m->venue_order_id, x.order_id);
      {
        char buf[24];
        const std::size_t n = format_int64(x.trade_id, buf);
        m->exec_id.assign(std::string_view(buf, n));
      }
      m->price = *last_px;
      m->qty = *last_qty;
      m->cum_qty = *cum;
      m->leaves_qty = *order_qty - *cum;
      // Commission is quoted in `N` (the commission asset); it is stored as-is - the fee
      // asset can be base, quote or BNB, and the engine only aggregates it for reporting.
      if (const auto fee = parse_notional(x.fee)) {
        m->fee = *fee;
      } else {
        m->fee = Notional{};
      }
      m->side = side;
      m->liquidity = x.maker ? Liquidity::Maker : Liquidity::Taker;
      stamp(*m, recv_ts, t0, x.tx_time);
      r.order_kind = OrderEventKind::Fill;
      r.len = sizeof(OrderFillMsg);
      return r;
    }
    if (x.exec_type == "EXPIRED" || x.exec_type == "TRADE_PREVENTION") {
      auto* m = reinterpret_cast<OrderExpiredMsg*>(out.data());
      init_header(*m, EventType::OrderExpired, inst, venue_);
      m->cl_ord_id = cl_ord_id;
      set_venue_order_id(m->venue_order_id, x.order_id);
      m->cum_qty = *cum;
      stamp(*m, recv_ts, t0, x.tx_time);
      r.order_kind = OrderEventKind::Expired;
      r.len = sizeof(OrderExpiredMsg);
      return r;
    }
    r.count = 0;
    return ignored();
  }

  if (type == "outboundAccountPosition") {
    std::int64_t ev_time = 0;
    if (ev["E"].get_int64().get(ev_time) != sj::SUCCESS) return malformed();
    od::array balances;
    if (ev["B"].get_array().get(balances) != sj::SUCCESS) return malformed();
    std::uint32_t written = 0;
    std::uint32_t count = 0;
    for (auto bal_res : balances) {
      od::object bal;
      if (bal_res.get_object().get(bal) != sj::SUCCESS) return malformed();
      std::string_view asset;
      std::string_view free_s;
      std::string_view locked_s;
      if (bal["a"].get_string().get(asset) != sj::SUCCESS) return malformed();
      if (bal["f"].get_string().get(free_s) != sj::SUCCESS) return malformed();
      if (bal["l"].get_string().get(locked_s) != sj::SUCCESS) return malformed();
      const auto f = parse_qty(free_s);
      const auto l = parse_qty(locked_s);
      if (!f || !l) return malformed();
      for (const Instrument& inst : instruments_) {
        if (inst.venue != venue_ || !iequals_symbol(inst.base.view(), asset)) continue;
        if (written + sizeof(PositionUpdateMsg) > out.size()) break;
        auto* m = reinterpret_cast<PositionUpdateMsg*>(out.data() + written);
        init_header(*m, EventType::PositionUpdate, inst.id, venue_);
        m->qty = *f + *l;
        m->avg_px = Price{};
        stamp(*m, recv_ts, t0, ev_time);
        written += sizeof(PositionUpdateMsg);
        ++count;
      }
    }
    ++stats_.positions;
    r.status = count > 0 ? ParseStatus::Ok : ParseStatus::Ignored;
    r.order_kind = OrderEventKind::Position;
    r.len = written;
    r.count = count;
    return r;
  }
  return ignored();  // balanceUpdate, listStatus, eventStreamTerminated, externalLockUpdate
}

}  // namespace fastmm::venues::binance
