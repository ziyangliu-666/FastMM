// Shared OUCH helpers (see ouch_common.hpp).
#include "fastmm/codecs/ouch/ouch_common.hpp"

namespace fastmm::codecs::ouch {

namespace {
template <class M>
M* begin(venues::EventSink& sink, EventType type, const EventStamp& st) noexcept {
  M* m = sink.reserve<M>();
  if (m == nullptr) return nullptr;
  *m = M{};
  init_header(*m, type, st.instrument, st.venue);
  m->hdr.venue_seq = st.venue_seq;
  m->hdr.exch_ts = Timestamp{st.exch_ns};
  m->hdr.recv_ts = Timestamp{st.rx_ns};
  return m;
}
void set_decimal(FixedString<40>& out, std::uint64_t v) noexcept {
  char buf[20];
  const std::size_t n = nasdaq::format_decimal(buf, v);
  out.assign(std::string_view(buf, n));
}
constexpr std::size_t kMaxText = 39;
}  // namespace

Liquidity liquidity_from_flag(char flag) noexcept {
  switch (flag) {
    // added liquidity
    case 'A':
    case 'J':
    case 'k':
    case 'W':
    case 'N':
    case 'u':
    case 'e':
    case 'f':
    case 'g':
    case 'j':
    case '1':
    case '2':
    case '4':
    case '5':
    case '7':
    case '8':
    case '9':
      return Liquidity::Maker;
    // removed liquidity
    case 'R':
    case 'm':
    case 'd':
    case 'p':
    case 'q':
    case 'r':
    case 't':
    case '3':
    case '6':
      return Liquidity::Taker;
    default:  // crosses (O M C L H K i), M-ELO (n), supplemental (0), unlisted
      return Liquidity::Unknown;
  }
}

SymbolMap::SymbolMap()
    : names_(new char[kMaxInstruments * 8]), known_(new std::uint8_t[kMaxInstruments]()) {}

bool SymbolMap::add(std::string_view symbol, InstrumentId id) noexcept {
  if (symbol.empty() || symbol.size() > 8 || !id.valid() || id.value >= kMaxInstruments)
    return false;
  char field[8];
  nasdaq::put_alpha(field, sizeof field, symbol);
  if (by_symbol_.assign(nasdaq::symbol_key8(field), id) == nullptr) return false;
  std::memcpy(names_.get() + static_cast<std::size_t>(id.value) * 8U, field, sizeof field);
  known_[id.value] = 1;
  return true;
}

const char* SymbolMap::symbol8(InstrumentId id) const noexcept {
  if (!id.valid() || id.value >= kMaxInstruments || known_[id.value] == 0) return nullptr;
  return names_.get() + static_cast<std::size_t>(id.value) * 8U;
}

InstrumentId SymbolMap::find(const char* field8) const noexcept {
  const InstrumentId* p = by_symbol_.find(nasdaq::symbol_key8(field8));
  return p == nullptr ? InstrumentId{} : *p;
}

bool emit_ack(venues::EventSink& sink,
              const EventStamp& st,
              ClientOrderId id,
              std::uint64_t reference) noexcept {
  auto* m = begin<OrderAckMsg>(sink, EventType::OrderAck, st);
  if (m == nullptr) return false;
  m->cl_ord_id = id;
  set_decimal(m->venue_order_id, reference);
  sink.commit();
  return true;
}

bool emit_reject(venues::EventSink& sink,
                 const EventStamp& st,
                 ClientOrderId id,
                 std::int32_t venue_code,
                 std::string_view text) noexcept {
  auto* m = begin<OrderRejectMsg>(sink, EventType::OrderReject, st);
  if (m == nullptr) return false;
  m->cl_ord_id = id;
  m->reason = RejectReason::VenueReject;
  m->venue_code = venue_code;
  m->text.assign(text.substr(0, kMaxText));
  sink.commit();
  return true;
}

bool emit_cancel_ack(venues::EventSink& sink, const EventStamp& st, const OrderEntry& e) noexcept {
  auto* m = begin<OrderCancelAckMsg>(sink, EventType::OrderCancelAck, st);
  if (m == nullptr) return false;
  m->cl_ord_id = e.cl_ord_id;
  set_decimal(m->venue_order_id, e.reference_number);
  m->cum_qty = e.cum;
  sink.commit();
  return true;
}

bool emit_cancel_reject(venues::EventSink& sink,
                        const EventStamp& st,
                        ClientOrderId id,
                        std::int32_t venue_code,
                        std::string_view text) noexcept {
  auto* m = begin<OrderCancelRejectMsg>(sink, EventType::OrderCancelReject, st);
  if (m == nullptr) return false;
  m->cl_ord_id = id;
  m->reason = RejectReason::VenueReject;
  m->venue_code = venue_code;
  m->text.assign(text.substr(0, kMaxText));
  sink.commit();
  return true;
}

bool emit_expired(venues::EventSink& sink, const EventStamp& st, const OrderEntry& e) noexcept {
  auto* m = begin<OrderExpiredMsg>(sink, EventType::OrderExpired, st);
  if (m == nullptr) return false;
  m->cl_ord_id = e.cl_ord_id;
  set_decimal(m->venue_order_id, e.reference_number);
  m->cum_qty = e.cum;
  sink.commit();
  return true;
}

bool emit_fill(venues::EventSink& sink,
               const EventStamp& st,
               const OrderEntry& e,
               std::uint64_t match_number,
               Price price,
               Qty qty,
               Liquidity liquidity) noexcept {
  auto* m = begin<OrderFillMsg>(sink, EventType::OrderFill, st);
  if (m == nullptr) return false;
  m->cl_ord_id = e.cl_ord_id;
  set_decimal(m->venue_order_id, e.reference_number);
  set_decimal(m->exec_id, match_number);
  m->price = price;
  m->qty = qty;
  m->cum_qty = e.cum;
  m->leaves_qty = e.leaves;
  m->fee = Notional{};
  m->side = e.side;
  m->liquidity = liquidity;
  sink.commit();
  return true;
}

}  // namespace fastmm::codecs::ouch
