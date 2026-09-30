#include "fastmm/venues/gemini/gemini_private_parser.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/gemini/gemini_error_map.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <cstring>

namespace fastmm::venues::gemini {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct GeminiPrivateParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

GeminiPrivateParser::GeminiPrivateParser(const SymbolTable& symbols,
                                         VenueId venue,
                                         std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
GeminiPrivateParser::~GeminiPrivateParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

PrivateDecodeResult malformed(PrivateParserStats& stats, PrivateDecodeResult r) noexcept {
  ++stats.malformed;
  r.status = ParseStatus::Malformed;
  r.count = 0;
  r.len = 0;
  return r;
}

// A string, or a number's digits (ids are numbers in events, strings in some replies).
std::string_view scalar_text(od::value v) noexcept {
  od::json_type t{};
  if (v.type().get(t) != sj::SUCCESS) return {};
  if (t == od::json_type::string) {
    std::string_view s;
    if (v.get_string().get(s) != sj::SUCCESS) return {};
    return s;
  }
  if (t != od::json_type::number) return {};
  std::string_view raw = v.raw_json_token();
  std::size_t n = 0;
  while (n < raw.size() && ((raw[n] >= '0' && raw[n] <= '9') || raw[n] == '-')) ++n;
  return raw.substr(0, n);
}

[[nodiscard]] Qty qty_or_zero(std::string_view s) noexcept {
  if (s.empty()) return Qty{};
  const auto q = parse_qty(s);
  return q ? *q : Qty{};
}

struct OrderEvent {
  std::string_view symbol;
  std::string_view order_id;
  std::string_view client_id;
  std::string_view side;
  std::string_view status;
  std::string_view orig_qty;
  std::string_view remaining;
  std::string_view executed;
  std::string_view last_px;
  std::string_view trade_id;
  std::string_view fee;
  std::string_view reason;
  bool maker = false;
  std::int64_t event_ns = 0;
  std::int64_t update_ns = 0;
};

template <class M>
M* place(std::span<std::byte> out) noexcept {
  std::memset(out.data(), 0, sizeof(M));
  return reinterpret_cast<M*>(out.data());
}

}  // namespace

PrivateDecodeResult GeminiPrivateParser::decode(std::string_view json,
                                                Timestamp recv_ts,
                                                Cycles t0,
                                                std::span<std::byte> out) noexcept {
  ++stats_.frames;
  PrivateDecodeResult r;
  if (out.size() < kDecoderScratchBytes) {
    r.status = ParseStatus::Overflow;
    return r;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return malformed(stats_, r);

  std::string_view event;
  OrderEvent ev;
  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return malformed(stats_, r);
    if (key.size() != 1) {
      if (key == "id") {
        r.control.present = true;
        od::value idv = field.value();
        od::json_type t{};
        if (idv.type().get(t) == sj::SUCCESS && t == od::json_type::string) {
          r.control.id = scalar_text(idv);
        } else {
          std::int64_t n = 0;
          if (idv.get_int64().get(n) == sj::SUCCESS) r.control.id_number = n;
        }
      } else if (key == "status") {
        std::int64_t st = 0;
        if (field.value().get_int64().get(st) != sj::SUCCESS) return malformed(stats_, r);
        r.control.present = true;
        r.control.status = static_cast<int>(st);
      } else if (key == "error") {
        od::object e;
        if (field.value().get_object().get(e) != sj::SUCCESS) return malformed(stats_, r);
        for (auto ef : e) {
          std::string_view ek;
          if (ef.unescaped_key().get(ek) != sj::SUCCESS) return malformed(stats_, r);
          if (ek == "code") {
            std::int64_t c = 0;
            if (ef.value().get_int64().get(c) == sj::SUCCESS)
              r.control.error_code = static_cast<int>(c);
          } else if (ek == "msg") {
            std::string_view msg;
            if (ef.value().get_string().get(msg) == sj::SUCCESS) r.control.msg = msg;
          }
        }
      } else if (key == "result") {
        od::object res;
        if (field.value().get_object().get(res) == sj::SUCCESS) {
          for (auto rf : res) {
            std::string_view rk;
            if (rf.unescaped_key().get(rk) != sj::SUCCESS) break;
            if (rk == "orderId" || rk == "order_id") r.control.order_id = scalar_text(rf.value());
          }
        }
      }
      continue;
    }
    od::value v = field.value();
    switch (key[0]) {
      case 'e':
        if (v.get_string().get(event) != sj::SUCCESS) return malformed(stats_, r);
        break;
      case 'E':
        if (v.get_int64().get(ev.event_ns) != sj::SUCCESS) ev.event_ns = 0;
        break;
      case 'T':
        if (v.get_int64().get(ev.update_ns) != sj::SUCCESS) ev.update_ns = 0;
        break;
      case 's':
        ev.symbol = scalar_text(v);
        break;
      case 'i':
        ev.order_id = scalar_text(v);
        break;
      case 'c':
        ev.client_id = scalar_text(v);
        break;
      case 'S':
        ev.side = scalar_text(v);
        break;
      case 'X':
        ev.status = scalar_text(v);
        break;
      case 'q':
        ev.orig_qty = scalar_text(v);
        break;
      case 'z':
        ev.remaining = scalar_text(v);
        break;
      case 'Z':
        ev.executed = scalar_text(v);
        break;
      case 'L':
        ev.last_px = scalar_text(v);
        break;
      case 't':
        ev.trade_id = scalar_text(v);
        break;
      case 'n':
        ev.fee = scalar_text(v);
        break;
      case 'r':
        ev.reason = scalar_text(v);
        break;
      case 'm': {
        bool b = false;
        if (v.get_bool().get(b) == sj::SUCCESS) ev.maker = b;
        break;
      }
      default:
        break;
    }
  }

  if (event.empty() && r.control.present) {
    ++stats_.control;
    r.status = r.control.status == 200 ? ParseStatus::Ignored : ParseStatus::Error;
    return r;
  }
  if (event != "orderUpdate") {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  if (ev.status.empty()) return malformed(stats_, r);
  const InstrumentId inst =
      ev.symbol.empty() ? InstrumentId::invalid() : symbols_.find(venue_, ev.symbol);
  ClientOrderId cl{};
  if (const auto id = decode_cl_ord_id(ev.client_id)) {
    cl = *id;
  } else {
    ++stats_.foreign_ids;
  }
  const std::int64_t ts_ns = ev.update_ns != 0 ? ev.update_ns : ev.event_ns;
  auto stamp = [&](EventHeader& h) {
    h.recv_ts = recv_ts;
    h.t0_cycles = t0;
    h.exch_ts = Timestamp{ts_ns};
  };

  if (ev.status == "NEW" || ev.status == "OPEN") {
    auto* m = place<OrderAckMsg>(out);
    init_header(*m, EventType::OrderAck, inst, venue_);
    m->cl_ord_id = cl;
    m->venue_order_id.assign(ev.order_id);
    stamp(m->hdr);
    ++stats_.acks;
    r.len = sizeof(OrderAckMsg);
    r.order_kind = OrderEventKind::Ack;
  } else if (ev.status == "PARTIALLY_FILLED" || ev.status == "FILLED") {
    const Qty qty = qty_or_zero(ev.executed);
    const auto px = parse_price(ev.last_px);
    if (!qty.is_positive() || !px || ev.trade_id.empty()) return malformed(stats_, r);
    auto* m = place<OrderFillMsg>(out);
    init_header(*m, EventType::OrderFill, inst, venue_);
    m->cl_ord_id = cl;
    m->venue_order_id.assign(ev.order_id);
    m->exec_id.assign(ev.trade_id);
    m->price = *px;
    m->qty = qty;
    m->leaves_qty = qty_or_zero(ev.remaining);
    const Qty orig = qty_or_zero(ev.orig_qty);
    if (orig.is_positive()) m->cum_qty = orig - m->leaves_qty;
    if (!ev.fee.empty()) {
      if (const auto f = parse_notional(ev.fee)) m->fee = *f;
    }
    m->fee_asset = FeeAsset::Quote;
    m->side = ev.side == "SELL" ? Side::Sell : Side::Buy;
    m->liquidity = ev.maker ? Liquidity::Maker : Liquidity::Taker;
    stamp(m->hdr);
    ++stats_.fills;
    r.len = sizeof(OrderFillMsg);
    r.order_kind = OrderEventKind::Fill;
  } else if (ev.status == "CANCELED") {
    const Qty cum = qty_or_zero(ev.executed);
    if (is_expiry_reason(ev.reason)) {
      auto* m = place<OrderExpiredMsg>(out);
      init_header(*m, EventType::OrderExpired, inst, venue_);
      m->cl_ord_id = cl;
      m->venue_order_id.assign(ev.order_id);
      m->cum_qty = cum;
      stamp(m->hdr);
      r.len = sizeof(OrderExpiredMsg);
    } else {
      auto* m = place<OrderCancelAckMsg>(out);
      init_header(*m, EventType::OrderCancelAck, inst, venue_);
      m->cl_ord_id = cl;
      m->venue_order_id.assign(ev.order_id);
      m->cum_qty = cum;
      stamp(m->hdr);
      r.len = sizeof(OrderCancelAckMsg);
    }
    ++stats_.cancels;
    r.order_kind = OrderEventKind::CancelAck;
  } else if (ev.status == "REJECTED") {
    auto* m = place<OrderRejectMsg>(out);
    init_header(*m, EventType::OrderReject, inst, venue_);
    m->cl_ord_id = cl;
    m->reason = map_reason(ev.reason).reason;
    m->venue_code = 0;
    m->text.assign(ev.reason);
    stamp(m->hdr);
    ++stats_.rejects;
    r.len = sizeof(OrderRejectMsg);
    r.order_kind = OrderEventKind::Reject;
  } else {
    ++stats_.ignored;
    r.status = ParseStatus::Ignored;
    return r;
  }
  r.status = ParseStatus::Ok;
  r.count = 1;
  return r;
}

}  // namespace fastmm::venues::gemini
