// OUCH 4.2 encoder, decoder and host-side builders (see ouch42.hpp).
#include "fastmm/codecs/ouch/ouch42.hpp"

#include "fastmm/core/config_macros.hpp"

#include <cstring>

namespace fastmm::codecs::ouch42 {

std::size_t inbound_length(char type) noexcept {
  switch (type) {
    case 'O':
      return sizeof(EnterOrder);
    case 'U':
      return sizeof(ReplaceOrder);
    case 'X':
      return sizeof(CancelOrder);
    case 'M':
      return sizeof(ModifyOrder);
    default:
      return 0;
  }
}

std::size_t outbound_length(char type) noexcept {
  switch (type) {
    case 'S':
      return sizeof(SystemEvent);
    case 'A':
      return sizeof(Accepted);
    case 'U':
      return sizeof(Replaced);
    case 'C':
      return sizeof(Canceled);
    case 'D':
      return sizeof(AiqCanceled);
    case 'E':
      return sizeof(Executed);
    case 'B':
      return sizeof(BrokenTrade);
    case 'G':
      return sizeof(ExecutedWithReferencePrice);
    case 'J':
      return sizeof(Rejected);
    case 'P':
      return sizeof(CancelPending);
    case 'I':
      return sizeof(CancelReject);
    case 'T':
      return sizeof(OrderPriorityUpdate);
    case 'M':
      return sizeof(OrderModified);
    default:
      return 0;
  }
}

std::string_view reject_reason_text(char reason) noexcept {
  switch (reason) {  // 3.10.1
    case 'a':
      return "Risk: Restricted Stock";
    case 'b':
      return "Risk: Short Sell Restricted";
    case 'c':
      return "Risk: Order Type Restricted";
    case 'C':
      return "NASDAQ is closed";
    case 'd':
      return "Risk: Exceeds ADV Limit";
    case 'D':
      return "Invalid Display Type";
    case 'e':
      return "Risk: Fat Finger";
    case 'H':
      return "Halted";
    case 'L':
      return "Firm not authorized for clearing type";
    case 'm':
      return "Risk: Max Shares Exceeded";
    case 'M':
      return "Outside permitted times for clearing type";
    case 'n':
      return "Risk: Max Notional Exceeded";
    case 'N':
      return "Invalid Minimum Quantity";
    case 'o':
      return "No reference price for LOO/LOC";
    case 'O':
      return "Other";
    case 'q':
      return "Midpoint Peg not accepted in crossed market";
    case 'r':
      return "Risk: Market Impact";
    case 'R':
      return "Order not allowed in this cross";
    case 'S':
      return "Invalid stock";
    case 'T':
      return "Test Mode";
    case 'u':
      return "LOO/LOC priced too aggressively";
    case 'v':
      return "Risk: Aggregate Exposure Exceeded";
    case 'V':
      return "Retail Not Allowed";
    case 'w':
      return "Risk: Symbol Message Rate Restriction";
    case 'W':
      return "Invalid Midpoint Post Only Price";
    case 'x':
      return "Risk: Port Message Rate Restriction";
    case 'X':
      return "Invalid price";
    case 'y':
      return "Risk: Duplicate Message Rate Restriction";
    case 'Z':
      return "Shares exceed safety threshold";
    default:
      return "Unknown reject reason";
  }
}

std::string_view cancel_reason_text(char reason) noexcept {
  switch (reason) {  // 3.5.1
    case 'U':
      return "User requested cancel";
    case 'I':
      return "Immediate or Cancel";
    case 'T':
      return "Timeout";
    case 'S':
      return "Supervisory";
    case 'D':
      return "Regulatory restriction";
    case 'Q':
      return "Self Match Prevention";
    case 'Z':
      return "System cancel";
    case 'C':
      return "Cross canceled";
    case 'K':
      return "Market Collars";
    case 'H':
      return "Halted";
    case 'X':
      return "Open Protection";
    case 'E':
      return "Closed";
    case 'F':
      return "Post Only cancel (NMS)";
    case 'G':
      return "Post Only cancel (contra displayed order)";
    default:
      return "Unknown cancel reason";
  }
}

std::optional<ClientOrderId> token_to_cl_ord_id(const char* token14) noexcept {
  return decode_cl_ord_id(std::string_view(token14, kClOrdIdChars));
}

// ---- encoder -------------------------------------------------------------------------------

std::uint32_t OuchEncoder::time_in_force(TimeInForce tif) noexcept {
  switch (tif) {
    case TimeInForce::Ioc:
    case TimeInForce::Fok:
      return kTifImmediateOrCancel;
    case TimeInForce::Day:
      return kTifMarketHours;
    case TimeInForce::Gtc:
      return kTifSystemHours;
  }
  return kTifSystemHours;
}

std::size_t OuchEncoder::encode(const venues::OrderCommand& cmd,
                                std::span<std::byte> out) noexcept {
  switch (cmd.kind) {
    case venues::OrderCommandKind::New:
      return encode_new(cmd, out);
    case venues::OrderCommandKind::Replace:
      return encode_replace(cmd, out);
    case venues::OrderCommandKind::Cancel:
      return encode_cancel(cmd, out);
  }
  return 0;
}

std::size_t OuchEncoder::encode_new(const venues::OrderCommand& cmd,
                                    std::span<std::byte> out) noexcept {
  if (FASTMM_UNLIKELY(out.size() < sizeof(EnterOrder))) {
    ++stats_.buffer_too_small;
    return 0;
  }
  if (FASTMM_UNLIKELY(cmd.type == OrderType::Market)) {
    ++stats_.unsupported;
    return 0;
  }
  const char* stock = symbols_.symbol8(cmd.instrument);
  if (FASTMM_UNLIKELY(stock == nullptr)) {
    ++stats_.unknown_instrument;
    return 0;
  }
  std::uint32_t shares = 0;
  std::uint32_t price = 0;
  if (FASTMM_UNLIKELY(!nasdaq::qty_to_shares(cmd.qty, shares) || shares == 0 ||
                      shares > kMaxShares || !nasdaq::price_to_price4(cmd.price, price) ||
                      price == 0 || price > kMaxPrice)) {
    ++stats_.bad_value;
    return 0;
  }
  EnterOrder& m = ouch::emplace<EnterOrder>(out);
  m.type = 'O';
  put_token(m.order_token, cmd.cl_ord_id);
  m.buy_sell_indicator = ouch::side_code(cmd.side);
  m.shares.set(shares);
  std::memcpy(m.stock, stock, sizeof m.stock);
  m.price.set(price);
  m.time_in_force.set(time_in_force(cmd.tif));
  std::memcpy(m.firm, cfg_.firm, sizeof m.firm);
  m.display = cmd.type == OrderType::PostOnly ? 'P' : cfg_.display;
  m.capacity = cfg_.capacity;
  m.intermarket_sweep_eligibility = cfg_.intermarket_sweep;
  m.minimum_quantity.set(cmd.tif == TimeInForce::Fok ? shares : 0);
  m.cross_type = cfg_.cross_type;
  m.customer_type = cfg_.customer_type;
  ++stats_.encoded;
  return sizeof(EnterOrder);
}

std::size_t OuchEncoder::encode_replace(const venues::OrderCommand& cmd,
                                        std::span<std::byte> out) noexcept {
  if (FASTMM_UNLIKELY(out.size() < sizeof(ReplaceOrder))) {
    ++stats_.buffer_too_small;
    return 0;
  }
  std::uint32_t shares = 0;
  std::uint32_t price = 0;
  if (FASTMM_UNLIKELY(!nasdaq::qty_to_shares(cmd.qty, shares) || shares == 0 ||
                      shares > kMaxShares || !nasdaq::price_to_price4(cmd.price, price) ||
                      price == 0 || price > kMaxPrice)) {
    ++stats_.bad_value;
    return 0;
  }
  ReplaceOrder& m = ouch::emplace<ReplaceOrder>(out);
  m.type = 'U';
  put_token(m.existing_order_token, cmd.orig_cl_ord_id);
  put_token(m.replacement_order_token, cmd.cl_ord_id);
  m.shares.set(shares);
  m.price.set(price);
  m.time_in_force.set(cfg_.replace_time_in_force);
  m.display = cfg_.display;
  m.intermarket_sweep_eligibility = cfg_.intermarket_sweep;
  m.minimum_quantity.set(0);
  ++stats_.encoded;
  return sizeof(ReplaceOrder);
}

std::size_t OuchEncoder::encode_cancel(const venues::OrderCommand& cmd,
                                       std::span<std::byte> out) noexcept {
  if (FASTMM_UNLIKELY(out.size() < sizeof(CancelOrder))) {
    ++stats_.buffer_too_small;
    return 0;
  }
  CancelOrder& m = ouch::emplace<CancelOrder>(out);
  m.type = 'X';
  put_token(m.order_token, cmd.cl_ord_id);
  m.shares.set(0);
  ++stats_.encoded;
  return sizeof(CancelOrder);
}

// ---- decoder -------------------------------------------------------------------------------

ParseStatus OuchDecoder::done(bool committed, std::uint64_t events) noexcept {
  if (FASTMM_UNLIKELY(!committed)) {
    ++stats_.overflow;
    return ParseStatus::Overflow;
  }
  stats_.events += events;
  return ParseStatus::Ok;
}

ParseStatus OuchDecoder::decode(const FrameView& frame,
                                std::int64_t rx_ts,
                                venues::EventSink& sink) noexcept {
  const std::span<const std::byte> in = frame.payload;
  ++stats_.messages;
  if (FASTMM_UNLIKELY(in.empty())) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  const char type = static_cast<char>(in[0]);
  const std::size_t need = outbound_length(type);
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
    case 'A':
      return on_accepted(ouch::view_as<Accepted>(p), rx_ts, sink);
    case 'U':
      return on_replaced(ouch::view_as<Replaced>(p), rx_ts, sink);
    case 'C': {
      const auto& m = ouch::view_as<Canceled>(p);
      return on_canceled(
          m.timestamp.get(), m.order_token, m.decrement_shares.get(), m.reason, rx_ts, sink);
    }
    case 'D': {
      const auto& m = ouch::view_as<AiqCanceled>(p);
      return on_canceled(
          m.timestamp.get(), m.order_token, m.decrement_shares.get(), m.reason, rx_ts, sink);
    }
    case 'E': {
      const auto& m = ouch::view_as<Executed>(p);
      return on_executed(m.timestamp.get(),
                         m.order_token,
                         m.executed_shares.get(),
                         m.execution_price.get(),
                         m.liquidity_flag,
                         m.match_number.get(),
                         rx_ts,
                         sink);
    }
    case 'G': {
      const auto& m = ouch::view_as<ExecutedWithReferencePrice>(p);
      return on_executed(m.timestamp.get(),
                         m.order_token,
                         m.executed_shares.get(),
                         m.execution_price.get(),
                         m.liquidity_flag,
                         m.match_number.get(),
                         rx_ts,
                         sink);
    }
    case 'J': {
      const auto& m = ouch::view_as<Rejected>(p);
      const std::optional<ClientOrderId> id = token_to_cl_ord_id(m.order_token);
      if (!id) {
        ++stats_.foreign_id;
        return ParseStatus::Ignored;
      }
      orders_.erase(id->value);
      return done(ouch::emit_reject(sink,
                                    stamp(m.timestamp.get(), InstrumentId{}, rx_ts),
                                    *id,
                                    static_cast<std::int32_t>(m.reason),
                                    reject_reason_text(m.reason)),
                  1);
    }
    case 'I': {
      const auto& m = ouch::view_as<CancelReject>(p);
      const std::optional<ClientOrderId> id = token_to_cl_ord_id(m.order_token);
      if (!id) {
        ++stats_.foreign_id;
        return ParseStatus::Ignored;
      }
      const ouch::OrderEntry* e = orders_.find(id->value);
      return done(
          ouch::emit_cancel_reject(
              sink,
              stamp(m.timestamp.get(), e != nullptr ? e->instrument : InstrumentId{}, rx_ts),
              *id,
              static_cast<std::int32_t>('I'),
              "Cancel Reject"),
          1);
    }
    case 'M': {
      const auto& m = ouch::view_as<OrderModified>(p);
      if (const std::optional<ClientOrderId> id = token_to_cl_ord_id(m.order_token)) {
        if (ouch::OrderEntry* e = orders_.find(id->value)) {
          const Qty total = nasdaq::shares_to_qty(m.shares.get());  // "shares outstanding"
          e->leaves = total;
          static_cast<void>(ouch::side_from_code(m.buy_sell_indicator, e->side));
        }
      }
      ++stats_.ignored;
      return ParseStatus::Ignored;
    }
    case 'S': {
      const auto& m = ouch::view_as<SystemEvent>(p);
      stats_.last_system_event = m.event_code;
      ++stats_.system_events;
      ++stats_.ignored;
      return ParseStatus::Ignored;
    }
    case 'B':
      ++stats_.broken_trades;
      ++stats_.ignored;
      return ParseStatus::Ignored;
    default:  // P T
      ++stats_.ignored;
      return ParseStatus::Ignored;
  }
}

ParseStatus OuchDecoder::on_accepted(const Accepted& m,
                                     std::int64_t rx,
                                     venues::EventSink& sink) noexcept {
  const std::optional<ClientOrderId> id = token_to_cl_ord_id(m.order_token);
  if (!id) {
    ++stats_.foreign_id;
    return ParseStatus::Ignored;
  }
  ouch::OrderEntry e{};
  if (FASTMM_UNLIKELY(!ouch::side_from_code(m.buy_sell_indicator, e.side))) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  e.cl_ord_id = *id;
  e.reference_number = m.order_reference_number.get();
  e.leaves = nasdaq::shares_to_qty(m.shares.get());
  e.instrument = symbols_.find(m.stock);
  const ouch::EventStamp st = stamp(m.timestamp.get(), e.instrument, rx);
  if (!ouch::emit_ack(sink, st, *id, e.reference_number)) return done(false, 0);
  if (m.order_state == 'D') {  // accepted and automatically canceled
    orders_.erase(id->value);
    return done(ouch::emit_expired(sink, st, e), 2);
  }
  if (orders_.assign(id->value, e) == nullptr) ++stats_.table_full;
  return done(true, 1);
}

ParseStatus OuchDecoder::on_replaced(const Replaced& m,
                                     std::int64_t rx,
                                     venues::EventSink& sink) noexcept {
  const std::optional<ClientOrderId> id = token_to_cl_ord_id(m.replacement_order_token);
  if (!id) {
    ++stats_.foreign_id;
    return ParseStatus::Ignored;
  }
  ouch::OrderEntry e{};
  if (FASTMM_UNLIKELY(!ouch::side_from_code(m.buy_sell_indicator, e.side))) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  e.cl_ord_id = *id;
  e.reference_number = m.order_reference_number.get();
  e.leaves = nasdaq::shares_to_qty(m.shares.get());
  e.instrument = symbols_.find(m.stock);
  if (const std::optional<ClientOrderId> prev = token_to_cl_ord_id(m.previous_order_token)) {
    if (const ouch::OrderEntry* old = orders_.find(prev->value)) e.cum = old->cum;
    orders_.erase(prev->value);
  }
  const ouch::EventStamp st = stamp(m.timestamp.get(), e.instrument, rx);
  if (!ouch::emit_ack(sink, st, *id, e.reference_number)) return done(false, 0);
  if (m.order_state == 'D') {
    orders_.erase(id->value);
    return done(ouch::emit_expired(sink, st, e), 2);
  }
  if (orders_.assign(id->value, e) == nullptr) ++stats_.table_full;
  return done(true, 1);
}

ParseStatus OuchDecoder::on_canceled(std::uint64_t ts,
                                     const char* token,
                                     std::uint32_t decrement,
                                     char reason,
                                     std::int64_t rx,
                                     venues::EventSink& sink) noexcept {
  const std::optional<ClientOrderId> id = token_to_cl_ord_id(token);
  if (!id) {
    ++stats_.foreign_id;
    return ParseStatus::Ignored;
  }
  ouch::OrderEntry* e = orders_.find(id->value);
  if (e == nullptr) {
    ++stats_.unknown_order;
    return ParseStatus::Ignored;
  }
  const Qty dec = nasdaq::shares_to_qty(decrement);
  e->leaves = dec >= e->leaves ? Qty{} : e->leaves - dec;
  if (e->leaves.is_positive()) {
    ++stats_.partial_cancels;
    return ParseStatus::Ignored;
  }
  const ouch::OrderEntry copy = *e;
  orders_.erase(id->value);
  const ouch::EventStamp st = stamp(ts, copy.instrument, rx);
  const bool expired = reason == 'I' || reason == 'T';
  return done(expired ? ouch::emit_expired(sink, st, copy) : ouch::emit_cancel_ack(sink, st, copy),
              1);
}

ParseStatus OuchDecoder::on_executed(std::uint64_t ts,
                                     const char* token,
                                     std::uint32_t shares,
                                     std::uint32_t price,
                                     char liquidity,
                                     std::uint64_t match,
                                     std::int64_t rx,
                                     venues::EventSink& sink) noexcept {
  const std::optional<ClientOrderId> id = token_to_cl_ord_id(token);
  if (!id) {
    ++stats_.foreign_id;
    return ParseStatus::Ignored;
  }
  ouch::OrderEntry* e = orders_.find(id->value);
  if (e == nullptr) {
    ++stats_.unknown_order;
    return ParseStatus::Ignored;
  }
  const Qty qty = nasdaq::shares_to_qty(shares);
  e->cum += qty;
  e->leaves = qty >= e->leaves ? Qty{} : e->leaves - qty;
  const ouch::OrderEntry copy = *e;
  if (!copy.leaves.is_positive()) orders_.erase(id->value);
  return done(ouch::emit_fill(sink,
                              stamp(ts, copy.instrument, rx),
                              copy,
                              match,
                              nasdaq::price4_to_price(price),
                              qty,
                              ouch::liquidity_from_flag(liquidity)),
              1);
}

// ---- host builders -------------------------------------------------------------------------

namespace host {

std::size_t system_event(std::span<std::byte> out, std::uint64_t ts, char event_code) noexcept {
  SystemEvent m{};
  m.type = 'S';
  m.timestamp.set(ts);
  m.event_code = event_code;
  return ouch::put(out, m);
}

std::size_t accepted(std::span<std::byte> out,
                     std::uint64_t ts,
                     const EnterOrder& in,
                     std::uint32_t shares,
                     std::uint64_t reference_number,
                     char order_state) noexcept {
  Accepted m{};
  m.type = 'A';
  m.timestamp.set(ts);
  std::memcpy(m.order_token, in.order_token, sizeof m.order_token);
  m.buy_sell_indicator = in.buy_sell_indicator;
  m.shares.set(shares);
  std::memcpy(m.stock, in.stock, sizeof m.stock);
  m.price = in.price;
  m.time_in_force = in.time_in_force;
  std::memcpy(m.firm, in.firm, sizeof m.firm);
  m.display = in.display;
  m.order_reference_number.set(reference_number);
  m.capacity = in.capacity;
  m.intermarket_sweep_eligibility = in.intermarket_sweep_eligibility;
  m.minimum_quantity = in.minimum_quantity;
  m.cross_type = in.cross_type;
  m.order_state = order_state;
  m.bbo_weight_indicator = ' ';
  return ouch::put(out, m);
}

std::size_t replaced(std::span<std::byte> out,
                     std::uint64_t ts,
                     const ReplaceOrder& in,
                     const EnterOrder& original,
                     std::uint32_t shares_outstanding,
                     std::uint64_t reference_number,
                     char order_state) noexcept {
  Replaced m{};
  m.type = 'U';
  m.timestamp.set(ts);
  std::memcpy(
      m.replacement_order_token, in.replacement_order_token, sizeof m.replacement_order_token);
  m.buy_sell_indicator = original.buy_sell_indicator;
  m.shares.set(shares_outstanding);
  std::memcpy(m.stock, original.stock, sizeof m.stock);
  m.price = in.price;
  m.time_in_force = in.time_in_force;
  std::memcpy(m.firm, original.firm, sizeof m.firm);
  m.display = in.display;
  m.order_reference_number.set(reference_number);
  m.capacity = original.capacity;
  m.intermarket_sweep_eligibility = in.intermarket_sweep_eligibility;
  m.minimum_quantity = in.minimum_quantity;
  m.cross_type = original.cross_type;
  m.order_state = order_state;
  std::memcpy(m.previous_order_token, in.existing_order_token, sizeof m.previous_order_token);
  m.bbo_weight_indicator = ' ';
  return ouch::put(out, m);
}

std::size_t canceled(std::span<std::byte> out,
                     std::uint64_t ts,
                     const char* token14,
                     std::uint32_t decrement_shares,
                     char reason) noexcept {
  Canceled m{};
  m.type = 'C';
  m.timestamp.set(ts);
  std::memcpy(m.order_token, token14, sizeof m.order_token);
  m.decrement_shares.set(decrement_shares);
  m.reason = reason;
  return ouch::put(out, m);
}

std::size_t executed(std::span<std::byte> out,
                     std::uint64_t ts,
                     const char* token14,
                     std::uint32_t shares,
                     std::uint32_t price,
                     char liquidity_flag,
                     std::uint64_t match_number) noexcept {
  Executed m{};
  m.type = 'E';
  m.timestamp.set(ts);
  std::memcpy(m.order_token, token14, sizeof m.order_token);
  m.executed_shares.set(shares);
  m.execution_price.set(price);
  m.liquidity_flag = liquidity_flag;
  m.match_number.set(match_number);
  return ouch::put(out, m);
}

std::size_t rejected(std::span<std::byte> out,
                     std::uint64_t ts,
                     const char* token14,
                     char reason) noexcept {
  Rejected m{};
  m.type = 'J';
  m.timestamp.set(ts);
  std::memcpy(m.order_token, token14, sizeof m.order_token);
  m.reason = reason;
  return ouch::put(out, m);
}

std::size_t cancel_reject(std::span<std::byte> out,
                          std::uint64_t ts,
                          const char* token14) noexcept {
  CancelReject m{};
  m.type = 'I';
  m.timestamp.set(ts);
  std::memcpy(m.order_token, token14, sizeof m.order_token);
  return ouch::put(out, m);
}

}  // namespace host

}  // namespace fastmm::codecs::ouch42
