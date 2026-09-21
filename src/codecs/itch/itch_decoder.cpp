// TotalView-ITCH 5.0 decoder (see itch_decoder.hpp for the message -> event mapping).
#include "fastmm/codecs/itch/itch_decoder.hpp"

#include "fastmm/core/config_macros.hpp"

#include <algorithm>

namespace fastmm::codecs::itch {

namespace {
[[nodiscard]] constexpr bool side_from_indicator(char c, Side& out) noexcept {
  if (c == 'B') {
    out = Side::Buy;
    return true;
  }
  if (c == 'S') {
    out = Side::Sell;
    return true;
  }
  return false;
}
}  // namespace

ItchDecoder::ItchDecoder(VenueId venue) : venue_(venue), locate_(new InstrumentId[kLocateSlots]) {}

bool ItchDecoder::add_symbol(std::string_view symbol, InstrumentId id) noexcept {
  if (symbol.empty() || symbol.size() > sizeof(StockDirectory::stock) || !id.valid()) return false;
  return symbols_.assign(nasdaq::symbol_key(symbol), id) != nullptr;
}

void ItchDecoder::clear_locates() noexcept {
  std::fill_n(locate_.get(), kLocateSlots, InstrumentId{});
}

template <class M, class Sink>
M* ItchDecoder::start(Sink& sink,
                      EventType type,
                      InstrumentId inst,
                      const MessageHeader& h,
                      std::int64_t rx_ts) noexcept {
  M* m = sink.template reserve<M>();
  if (FASTMM_UNLIKELY(m == nullptr)) {
    ++stats_.overflow;
    return nullptr;
  }
  *m = M{};
  init_header(*m, type, inst, venue_);
  m->hdr.venue_seq = venue_seq_;
  m->hdr.exch_ts = Timestamp{midnight_ns_ + static_cast<std::int64_t>(h.timestamp.get())};
  m->hdr.recv_ts = Timestamp{rx_ts};
  return m;
}

template <class Sink>
ParseStatus ItchDecoder::commit(Sink& sink) noexcept {
  sink.commit();
  ++stats_.events;
  return ParseStatus::Ok;
}

bool ItchDecoder::lookup(const MessageHeader& h, InstrumentId& out) noexcept {
  out = locate_[h.stock_locate.get()];
  if (FASTMM_LIKELY(out.valid())) return true;
  ++stats_.unknown_locate;
  return false;
}

template <class Sink>
ParseStatus ItchDecoder::add(const MessageHeader& h,
                             std::uint64_t ref,
                             char side,
                             std::uint32_t shares,
                             std::uint32_t price4,
                             std::int64_t rx_ts,
                             Sink& sink) noexcept {
  InstrumentId inst;
  if (!lookup(h, inst)) return ParseStatus::Ignored;
  Side s = Side::Buy;
  if (FASTMM_UNLIKELY(!side_from_indicator(side, s))) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  auto* m = start<OrderAddL3Msg>(sink, EventType::OrderAddL3, inst, h, rx_ts);
  if (m == nullptr) return ParseStatus::Overflow;
  m->order_ref = ref;
  m->price = nasdaq::price4_to_price(price4);
  m->qty = nasdaq::shares_to_qty(shares);
  m->side = s;
  return commit(sink);
}

template <class Sink>
ParseStatus ItchDecoder::execute(const MessageHeader& h,
                                 std::uint64_t ref,
                                 std::uint32_t shares,
                                 std::uint64_t match,
                                 Price exec_price,
                                 std::uint8_t flags,
                                 std::int64_t rx_ts,
                                 Sink& sink) noexcept {
  InstrumentId inst;
  if (!lookup(h, inst)) return ParseStatus::Ignored;
  auto* m = start<OrderExecL3Msg>(sink, EventType::OrderExecL3, inst, h, rx_ts);
  if (m == nullptr) return ParseStatus::Overflow;
  m->order_ref = ref;
  m->exec_qty = nasdaq::shares_to_qty(shares);
  m->exec_price = exec_price;
  m->match_id = match;
  m->exec_flags = flags;
  return commit(sink);
}

template <class Sink>
ParseStatus ItchDecoder::cancel(const MessageHeader& h,
                                std::uint64_t ref,
                                Qty canceled,
                                std::int64_t rx_ts,
                                Sink& sink) noexcept {
  InstrumentId inst;
  if (!lookup(h, inst)) return ParseStatus::Ignored;
  auto* m = start<OrderCancelL3Msg>(sink, EventType::OrderCancelL3, inst, h, rx_ts);
  if (m == nullptr) return ParseStatus::Overflow;
  m->order_ref = ref;
  m->canceled_qty = canceled;
  return commit(sink);
}

template <class Sink>
ParseStatus ItchDecoder::trade(const MessageHeader& h,
                               Price price,
                               Qty qty,
                               std::uint64_t match,
                               Side aggressor,
                               std::int64_t rx_ts,
                               Sink& sink) noexcept {
  InstrumentId inst;
  if (!lookup(h, inst)) return ParseStatus::Ignored;
  auto* m = start<TradeMsg>(sink, EventType::Trade, inst, h, rx_ts);
  if (m == nullptr) return ParseStatus::Overflow;
  m->price = price;
  m->qty = qty;
  m->trade_id = match;
  m->aggressor = aggressor;
  return commit(sink);
}

ParseStatus ItchDecoder::decode(const FrameView& frame,
                                std::int64_t rx_ts,
                                venues::EventSink& sink) noexcept {
  return decode_into(frame, rx_ts, sink);
}

template <class Sink>
ParseStatus ItchDecoder::decode_into(const FrameView& frame,
                                     std::int64_t rx_ts,
                                     Sink& sink) noexcept {
  const std::span<const std::byte> in = frame.payload;
  ++stats_.messages;
  if (FASTMM_UNLIKELY(in.empty())) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  const char type = static_cast<char>(in[0]);
  const std::size_t need = message_length(type);
  if (FASTMM_UNLIKELY(need == 0)) {
    ++stats_.unknown_type;
    return ParseStatus::Ignored;
  }
  if (FASTMM_UNLIKELY(in.size() < need)) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  const std::byte* p = in.data();
  switch (type) {
    case 'A': {
      const auto& m = view_as<AddOrder>(p);
      return add(m.hdr,
                 m.order_reference_number.get(),
                 m.buy_sell_indicator,
                 m.shares.get(),
                 m.price.get(),
                 rx_ts,
                 sink);
    }
    case 'F': {
      const auto& m = view_as<AddOrderMpid>(p);
      return add(m.hdr,
                 m.order_reference_number.get(),
                 m.buy_sell_indicator,
                 m.shares.get(),
                 m.price.get(),
                 rx_ts,
                 sink);
    }
    case 'E': {
      const auto& m = view_as<OrderExecuted>(p);
      return execute(m.hdr,
                     m.order_reference_number.get(),
                     m.executed_shares.get(),
                     m.match_number.get(),
                     Price{},
                     0,
                     rx_ts,
                     sink);
    }
    case 'C': {
      const auto& m = view_as<OrderExecutedWithPrice>(p);
      return execute(m.hdr,
                     m.order_reference_number.get(),
                     m.executed_shares.get(),
                     m.match_number.get(),
                     nasdaq::price4_to_price(m.execution_price.get()),
                     m.printable == 'Y' ? std::uint8_t{0} : OrderExecL3Msg::kNonPrintable,
                     rx_ts,
                     sink);
    }
    case 'X': {
      const auto& m = view_as<OrderCancel>(p);
      const std::uint32_t shares = m.cancelled_shares.get();
      if (FASTMM_UNLIKELY(shares == 0)) {  // canceled_qty 0 would mean "delete"
        ++stats_.ignored;
        return ParseStatus::Ignored;
      }
      return cancel(
          m.hdr, m.order_reference_number.get(), nasdaq::shares_to_qty(shares), rx_ts, sink);
    }
    case 'D': {
      const auto& m = view_as<OrderDelete>(p);
      return cancel(m.hdr, m.order_reference_number.get(), Qty{}, rx_ts, sink);
    }
    case 'U': {
      const auto& m = view_as<OrderReplace>(p);
      InstrumentId inst;
      if (!lookup(m.hdr, inst)) return ParseStatus::Ignored;
      auto* e = start<OrderReplaceL3Msg>(sink, EventType::OrderReplaceL3, inst, m.hdr, rx_ts);
      if (e == nullptr) return ParseStatus::Overflow;
      e->old_order_ref = m.original_order_reference_number.get();
      e->new_order_ref = m.new_order_reference_number.get();
      e->price = nasdaq::price4_to_price(m.price.get());
      e->qty = nasdaq::shares_to_qty(m.shares.get());
      return commit(sink);
    }
    case 'P': {
      const auto& m = view_as<Trade>(p);
      Side s = Side::Buy;
      if (FASTMM_UNLIKELY(!side_from_indicator(m.buy_sell_indicator, s))) {
        ++stats_.malformed;
        return ParseStatus::Malformed;
      }
      return trade(m.hdr,
                   nasdaq::price4_to_price(m.price.get()),
                   nasdaq::shares_to_qty(m.shares.get()),
                   m.match_number.get(),
                   s,
                   rx_ts,
                   sink);
    }
    case 'Q': {
      const auto& m = view_as<CrossTrade>(p);
      Qty qty;
      if (FASTMM_UNLIKELY(!nasdaq::shares_to_qty(m.shares.get(), qty))) {
        ++stats_.malformed;
        return ParseStatus::Malformed;
      }
      if (qty.is_zero()) {  // "may show the shares as zero" when no cross took place
        ++stats_.ignored;
        return ParseStatus::Ignored;
      }
      return trade(m.hdr,
                   nasdaq::price4_to_price(m.cross_price.get()),
                   qty,
                   m.match_number.get(),
                   Side::Buy,
                   rx_ts,
                   sink);
    }
    case 'R': {
      const auto& m = view_as<StockDirectory>(p);
      ++stats_.directory;
      if (const InstrumentId* id = symbols_.find(nasdaq::symbol_key8(m.stock))) {
        locate_[m.hdr.stock_locate.get()] = *id;
        ++stats_.directory_mapped;
      }
      ++stats_.ignored;
      return ParseStatus::Ignored;
    }
    default:
      ++stats_.ignored;
      return ParseStatus::Ignored;
  }
}

template ParseStatus ItchDecoder::decode_into(const FrameView&,
                                              std::int64_t,
                                              venues::EventSink&) noexcept;
template ParseStatus ItchDecoder::decode_into(const FrameView&,
                                              std::int64_t,
                                              ScratchSink&) noexcept;

}  // namespace fastmm::codecs::itch
