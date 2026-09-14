#include "fastmm/codecs/fix/fix_decoder.hpp"

#include <cstring>

namespace fastmm::codecs::fix {

using venues::EventSink;
using venues::ParseStatus;

namespace {

std::uint64_t fnv1a(std::string_view s) noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h | 1U;  // never 0: 0 marks an empty history slot
}

// OrdRejReason(103) -> engine reason.
RejectReason reject_reason(std::int64_t code) noexcept {
  switch (code) {
    case ord_rej::kUnknownSymbol:
      return RejectReason::InstrumentDisabled;
    case ord_rej::kUnknownOrder:
      return RejectReason::VenueUnknownOrder;
    case ord_rej::kDuplicateOrder:
      return RejectReason::DuplicateId;
    case ord_rej::kIncorrectQuantity:
      return RejectReason::InvalidLot;
    default:
      return RejectReason::VenueReject;
  }
}

// venue_seq = MsgSeqNum, exch_ts = TransactTime(60) else SendingTime(52), recv_ts = rx.
void stamp(EventHeader& h, const FixView& v, std::int64_t rx_ts) noexcept {
  h.recv_ts = Timestamp{rx_ts};
  h.venue_seq = static_cast<std::uint64_t>(v.get_int(tag::kMsgSeqNum).value_or(0));
  auto ts = v.get_timestamp_ns(tag::kTransactTime);
  if (!ts) ts = v.get_timestamp_ns(tag::kSendingTime);
  h.exch_ts = Timestamp{ts.value_or(0)};
}

template <class M>
M* reserve_zeroed(EventSink& sink) noexcept {
  M* m = sink.reserve<M>();
  if (m != nullptr) std::memset(static_cast<void*>(m), 0, sizeof(M));
  return m;
}

std::optional<ClientOrderId> order_id_of(const FixView& v, bool prefer_orig) noexcept {
  if (prefer_orig) {
    if (auto id = decode_cl_ord_id(v.get(tag::kOrigClOrdID))) return id;
  }
  return decode_cl_ord_id(v.get(tag::kClOrdID));
}

}  // namespace

ParseStatus FixDecoder::status(ParseStatus s) noexcept {
  switch (s) {
    case ParseStatus::Ignored:
      ++stats_.ignored;
      break;
    case ParseStatus::Malformed:
      ++stats_.malformed;
      break;
    case ParseStatus::UnknownSymbol:
      ++stats_.unknown_symbol;
      break;
    case ParseStatus::Overflow:
      ++stats_.overflow;
      break;
    default:
      break;
  }
  return s;
}

ParseStatus FixDecoder::decode(const FrameView& frame,
                               std::int64_t rx_ts,
                               EventSink& sink) noexcept {
  if (frame.payload.empty()) return status(ParseStatus::Ignored);
  if (view_.parse(frame.payload, begin_string_.view(), verify_checksum_) != FixError::None)
    return status(ParseStatus::Malformed);
  return decode_view(view_, rx_ts, sink);
}

ParseStatus FixDecoder::decode_view(const FixView& v,
                                    std::int64_t rx_ts,
                                    EventSink& sink) noexcept {
  ++stats_.messages;
  const std::string_view t = v.msg_type();
  if (t.size() != 1) return status(ParseStatus::Ignored);
  switch (t[0]) {
    case '8':
      return execution_report(v, rx_ts, sink);
    case '9':
      return cancel_reject(v, rx_ts, sink);
    case 'W':
      return snapshot(v, rx_ts, sink);
    case 'X':
      return incremental(v, rx_ts, sink);
    default:
      return status(ParseStatus::Ignored);
  }
}

bool FixDecoder::exec_id_seen(std::uint64_t h) const noexcept {
  for (const std::uint64_t e : exec_ids_) {
    if (e == h) return true;
  }
  return false;
}

void FixDecoder::remember_exec_id(std::uint64_t h) noexcept {
  exec_ids_[exec_pos_] = h;
  exec_pos_ = (exec_pos_ + 1) % kExecIdHistory;
}

ParseStatus FixDecoder::execution_report(const FixView& v,
                                         std::int64_t rx_ts,
                                         EventSink& sink) noexcept {
  const auto et = v.get_char(tag::kExecType);
  if (!et) return status(ParseStatus::Malformed);
  const InstrumentId inst = symbols_->find(v.get(tag::kSymbol));
  switch (*et) {
    case exec_type::kNew:
    case exec_type::kReplaced: {
      const auto id = order_id_of(v, false);
      if (!id) {
        ++stats_.foreign_ids;
        return status(ParseStatus::Ignored);
      }
      auto* m = reserve_zeroed<OrderAckMsg>(sink);
      if (m == nullptr) return status(ParseStatus::Overflow);
      init_header(*m, EventType::OrderAck, inst, venue_);
      stamp(m->hdr, v, rx_ts);
      m->cl_ord_id = *id;
      m->venue_order_id.assign(v.get(tag::kOrderID));
      sink.commit();
      ++stats_.acks;
      return ParseStatus::Ok;
    }
    case exec_type::kRejected: {
      const auto id = order_id_of(v, false);
      if (!id) {
        ++stats_.foreign_ids;
        return status(ParseStatus::Ignored);
      }
      auto* m = reserve_zeroed<OrderRejectMsg>(sink);
      if (m == nullptr) return status(ParseStatus::Overflow);
      init_header(*m, EventType::OrderReject, inst, venue_);
      stamp(m->hdr, v, rx_ts);
      m->cl_ord_id = *id;
      const auto code = v.get_int(tag::kOrdRejReason);
      m->reason = reject_reason(code.value_or(ord_rej::kOther));
      m->venue_code = static_cast<std::int32_t>(code.value_or(-1));
      m->text.assign(v.get(tag::kText));
      sink.commit();
      ++stats_.rejects;
      return ParseStatus::Ok;
    }
    case exec_type::kCanceled: {
      const auto id = order_id_of(v, true);
      if (!id) {
        ++stats_.foreign_ids;
        return status(ParseStatus::Ignored);
      }
      auto* m = reserve_zeroed<OrderCancelAckMsg>(sink);
      if (m == nullptr) return status(ParseStatus::Overflow);
      init_header(*m, EventType::OrderCancelAck, inst, venue_);
      stamp(m->hdr, v, rx_ts);
      m->cl_ord_id = *id;
      m->venue_order_id.assign(v.get(tag::kOrderID));
      m->cum_qty = v.get_qty(tag::kCumQty).value_or(Qty{});
      sink.commit();
      ++stats_.cancel_acks;
      return ParseStatus::Ok;
    }
    case exec_type::kExpired:
    case exec_type::kDoneForDay: {
      const auto id = order_id_of(v, false);
      if (!id) {
        ++stats_.foreign_ids;
        return status(ParseStatus::Ignored);
      }
      auto* m = reserve_zeroed<OrderExpiredMsg>(sink);
      if (m == nullptr) return status(ParseStatus::Overflow);
      init_header(*m, EventType::OrderExpired, inst, venue_);
      stamp(m->hdr, v, rx_ts);
      m->cl_ord_id = *id;
      m->venue_order_id.assign(v.get(tag::kOrderID));
      m->cum_qty = v.get_qty(tag::kCumQty).value_or(Qty{});
      sink.commit();
      ++stats_.expired;
      return ParseStatus::Ok;
    }
    case exec_type::kTrade:
    case exec_type::kPartialFillLegacy:
    case exec_type::kFillLegacy: {
      const auto id = order_id_of(v, false);
      if (!id) {
        ++stats_.foreign_ids;
        return status(ParseStatus::Ignored);
      }
      const auto last_qty = v.get_qty(tag::kLastQty);
      const auto last_px = v.get_price(tag::kLastPx);
      const auto cum = v.get_qty(tag::kCumQty);
      const auto leaves = v.get_qty(tag::kLeavesQty);
      const std::string_view exec_id = v.get(tag::kExecID);
      if (!last_qty || !last_px || !cum || !leaves || exec_id.empty())
        return status(ParseStatus::Malformed);
      const std::uint64_t h = fnv1a(exec_id);
      if (exec_id_seen(h)) {
        ++stats_.duplicate_fills;
        return status(ParseStatus::Ignored);
      }
      auto* m = reserve_zeroed<OrderFillMsg>(sink);
      if (m == nullptr) return status(ParseStatus::Overflow);
      init_header(*m, EventType::OrderFill, inst, venue_);
      stamp(m->hdr, v, rx_ts);
      m->cl_ord_id = *id;
      m->venue_order_id.assign(v.get(tag::kOrderID));
      m->exec_id.assign(exec_id);
      m->price = *last_px;
      m->qty = *last_qty;
      m->cum_qty = *cum;
      m->leaves_qty = *leaves;
      m->side = v.get_char(tag::kSide).value_or(kSideBuy) == kSideSell ? Side::Sell : Side::Buy;
      const auto liq = v.get_int(tag::kLastLiquidityInd);
      m->liquidity = !liq                        ? Liquidity::Unknown
                     : *liq == kLiquidityAdded   ? Liquidity::Maker
                     : *liq == kLiquidityRemoved ? Liquidity::Taker
                                                 : Liquidity::Unknown;
      sink.commit();
      remember_exec_id(h);
      ++stats_.fills;
      return ParseStatus::Ok;
    }
    default:
      return status(ParseStatus::Ignored);
  }
}

ParseStatus FixDecoder::cancel_reject(const FixView& v,
                                      std::int64_t rx_ts,
                                      EventSink& sink) noexcept {
  const bool replace =
      v.get_char(tag::kCxlRejResponseTo).value_or(kCxlRejToCancel) == kCxlRejToReplace;
  const auto code = v.get_int(tag::kCxlRejReason);
  const RejectReason reason = code && *code == kCxlRejUnknownOrder ? RejectReason::VenueUnknownOrder
                                                                   : RejectReason::VenueReject;
  const InstrumentId inst = symbols_->find(v.get(tag::kSymbol));
  if (replace) {
    // A rejected cancel/replace: the OMS resolves the pending replace through a reject of the
    // replacement id (ClOrdID), as the Binance connector reports a failed cancelReplace.
    const auto id = order_id_of(v, false);
    if (!id) {
      ++stats_.foreign_ids;
      return status(ParseStatus::Ignored);
    }
    auto* m = reserve_zeroed<OrderRejectMsg>(sink);
    if (m == nullptr) return status(ParseStatus::Overflow);
    init_header(*m, EventType::OrderReject, inst, venue_);
    stamp(m->hdr, v, rx_ts);
    m->cl_ord_id = *id;
    m->reason = reason;
    m->venue_code = static_cast<std::int32_t>(code.value_or(-1));
    m->text.assign(v.get(tag::kText));
    sink.commit();
    ++stats_.cancel_rejects;
    return ParseStatus::Ok;
  }
  const auto id = order_id_of(v, true);
  if (!id) {
    ++stats_.foreign_ids;
    return status(ParseStatus::Ignored);
  }
  auto* m = reserve_zeroed<OrderCancelRejectMsg>(sink);
  if (m == nullptr) return status(ParseStatus::Overflow);
  init_header(*m, EventType::OrderCancelReject, inst, venue_);
  stamp(m->hdr, v, rx_ts);
  m->cl_ord_id = *id;
  m->reason = reason;
  m->venue_code = static_cast<std::int32_t>(code.value_or(-1));
  m->text.assign(v.get(tag::kText));
  sink.commit();
  ++stats_.cancel_rejects;
  return ParseStatus::Ok;
}

ParseStatus FixDecoder::snapshot(const FixView& v, std::int64_t rx_ts, EventSink& sink) noexcept {
  const InstrumentId inst = symbols_->find(v.get(tag::kSymbol));
  if (!inst.valid()) return status(ParseStatus::UnknownSymbol);
  FixGroupReader g(v, tag::kNoMDEntries, tag::kMDEntryType);
  std::uint32_t bids = 0;
  std::uint32_t asks = 0;
  std::size_t n = 0;
  FixRange e;
  while (g.next(e)) {
    if (n >= kMaxMdEntries) return status(ParseStatus::Malformed);
    const char type = e.get_char(tag::kMDEntryType).value_or('\0');
    if (type == kMdBid) ++bids;
    if (type == kMdOffer) ++asks;
    entry_begin_[n] = e.begin_index();
    entry_end_[n] = e.end_index();
    entry_type_[n] = type;
    ++n;
  }
  if (!g.complete() || bids > kMaxBookLevelsPerMsg || asks > kMaxBookLevelsPerMsg)
    return status(ParseStatus::Malformed);
  const std::uint32_t len = BookDeltaMsg::size_for(bids, asks);
  std::byte* p = sink.reserve_bytes(len);
  if (p == nullptr) return status(ParseStatus::Overflow);
  std::memset(p, 0, len);
  auto* m = reinterpret_cast<BookDeltaMsg*>(p);
  init_header(*m, EventType::BookSnapshot, inst, venue_, len);
  m->hdr.flags = EventHeader::kSnapshot;
  stamp(m->hdr, v, rx_ts);
  m->bid_count = bids;
  m->ask_count = asks;
  m->first_update_id = m->hdr.venue_seq;
  m->last_update_id = m->hdr.venue_seq;
  Level* out = m->levels();
  std::uint32_t bi = 0;
  std::uint32_t ai = bids;
  for (std::size_t i = 0; i < n; ++i) {
    const char type = entry_type_[i];
    if (type != kMdBid && type != kMdOffer) continue;
    const FixRange r(&v, entry_begin_[i], entry_end_[i]);
    const auto px = r.get_price(tag::kMDEntryPx);
    const auto qty = r.get_qty(tag::kMDEntrySize);
    if (!px || !qty) return status(ParseStatus::Malformed);  // reserved, never committed
    out[type == kMdBid ? bi++ : ai++] = Level{*px, *qty};
  }
  sink.commit();
  ++stats_.book_updates;
  return ParseStatus::Ok;
}

ParseStatus FixDecoder::incremental(const FixView& v,
                                    std::int64_t rx_ts,
                                    EventSink& sink) noexcept {
  FixGroupReader g(v, tag::kNoMDEntries, tag::kMDUpdateAction);
  std::string_view symbol = v.get(tag::kSymbol);  // not in FIX 4.4 X's body; tolerated
  InstrumentId inst = symbol.empty() ? InstrumentId{} : symbols_->find(symbol);
  std::size_t n = 0;
  FixRange e;
  while (g.next(e)) {
    if (n >= kMaxMdEntries) return status(ParseStatus::Malformed);
    const std::string_view s = e.get(tag::kSymbol);
    if (!s.empty() && s != symbol) {
      symbol = s;
      inst = symbols_->find(s);
    }
    entry_begin_[n] = e.begin_index();
    entry_end_[n] = e.end_index();
    entry_inst_[n] = inst.value;
    entry_type_[n] = e.get_char(tag::kMDEntryType).value_or('\0');
    ++n;
  }
  if (!g.complete()) return status(ParseStatus::Malformed);

  std::size_t emitted = 0;
  std::size_t unknown = 0;
  std::size_t i = 0;
  while (i < n) {
    const InstrumentId id{entry_inst_[i]};
    const char type = entry_type_[i];
    if (!id.valid()) {
      ++unknown;
      ++i;
      continue;
    }
    if (type == kMdTrade) {
      const FixRange r(&v, entry_begin_[i], entry_end_[i]);
      const auto px = r.get_price(tag::kMDEntryPx);
      const auto qty = r.get_qty(tag::kMDEntrySize);
      if (!px || !qty) return status(ParseStatus::Malformed);
      auto* m = reserve_zeroed<TradeMsg>(sink);
      if (m == nullptr) return status(ParseStatus::Overflow);
      init_header(*m, EventType::Trade, id, venue_);
      stamp(m->hdr, v, rx_ts);
      m->price = *px;
      m->qty = *qty;
      const std::string_view entry_id = r.get(tag::kMDEntryID);
      const auto numeric = parse_fix_int(entry_id);
      m->trade_id = numeric && *numeric >= 0 ? static_cast<std::uint64_t>(*numeric)
                                             : (entry_id.empty() ? 0 : fnv1a(entry_id));
      m->aggressor = Side::Buy;  // FIX 4.4 MDIncGrp carries no aggressor side
      sink.commit();
      ++stats_.trades;
      ++emitted;
      ++i;
      continue;
    }
    if (type != kMdBid && type != kMdOffer) {
      ++i;
      continue;
    }
    // A run of book entries on one instrument becomes one BookDelta.
    std::size_t j = i;
    std::uint32_t bids = 0;
    std::uint32_t asks = 0;
    while (j < n && entry_inst_[j] == id.value &&
           (entry_type_[j] == kMdBid || entry_type_[j] == kMdOffer)) {
      if (entry_type_[j] == kMdBid) {
        ++bids;
      } else {
        ++asks;
      }
      ++j;
    }
    if (bids > kMaxBookLevelsPerMsg || asks > kMaxBookLevelsPerMsg)
      return status(ParseStatus::Malformed);
    const std::uint32_t len = BookDeltaMsg::size_for(bids, asks);
    std::byte* p = sink.reserve_bytes(len);
    if (p == nullptr) return status(ParseStatus::Overflow);
    std::memset(p, 0, len);
    auto* m = reinterpret_cast<BookDeltaMsg*>(p);
    init_header(*m, EventType::BookDelta, id, venue_, len);
    stamp(m->hdr, v, rx_ts);
    m->bid_count = bids;
    m->ask_count = asks;
    m->first_update_id = m->hdr.venue_seq;
    m->last_update_id = m->hdr.venue_seq;
    Level* out = m->levels();
    std::uint32_t bi = 0;
    std::uint32_t ai = bids;
    for (std::size_t k = i; k < j; ++k) {
      const FixRange r(&v, entry_begin_[k], entry_end_[k]);
      const auto action = r.get_char(tag::kMDUpdateAction);
      const auto px = r.get_price(tag::kMDEntryPx);
      if (!action || !px) return status(ParseStatus::Malformed);
      Qty qty{};
      if (*action != kMdDelete) {
        const auto sz = r.get_qty(tag::kMDEntrySize);
        if (!sz) return status(ParseStatus::Malformed);
        qty = *sz;
      }
      out[entry_type_[k] == kMdBid ? bi++ : ai++] = Level{*px, qty};
    }
    sink.commit();
    ++stats_.book_updates;
    ++emitted;
    i = j;
  }
  if (emitted > 0) return ParseStatus::Ok;
  return status(unknown > 0 ? ParseStatus::UnknownSymbol : ParseStatus::Ignored);
}

}  // namespace fastmm::codecs::fix
