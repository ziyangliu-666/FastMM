// OUCH 5.0 encoder, decoder, UserRefNum map and host-side builders (see ouch50.hpp).
#include "fastmm/codecs/ouch/ouch50.hpp"

#include "fastmm/core/config_macros.hpp"

#include <array>
#include <cstring>

namespace fastmm::codecs::ouch50 {

namespace {
[[nodiscard]] bool blank4(const char* f) noexcept {
  return f[0] == ' ' && f[1] == ' ' && f[2] == ' ' && f[3] == ' ';
}
void put_cl_ord_id(char* dst14, ClientOrderId id) noexcept {
  const FixedString<16> s = encode_cl_ord_id(id);
  std::memcpy(dst14, s.data(), kClOrdIdChars);
}
}  // namespace

OutboundLayout outbound_layout(char type) noexcept {
  switch (type) {
    case 'S':
      return {sizeof(SystemEvent), false, false};
    case 'A':
      return {offsetof(OrderAccepted, appendage_length), true, false};
    case 'U':
      return {offsetof(OrderReplaced, appendage_length), true, false};
    case 'C':
      return {offsetof(OrderCanceled, appendage_length), true, true};
    case 'D':
      return {offsetof(AiqCanceled, appendage_length), true, true};
    case 'E':
      return {offsetof(OrderExecuted, appendage_length), true, false};
    case 'B':
      return {offsetof(BrokenTrade, appendage_length), true, true};
    case 'J':
      return {offsetof(Rejected, appendage_length), true, true};
    case 'P':
    case 'I':
      return {offsetof(CancelPending, appendage_length), true, true};
    case 'T':
      return {offsetof(OrderPriorityUpdate, appendage_length), true, true};
    case 'M':
      return {offsetof(OrderModified, appendage_length), true, true};
    case 'R':
      return {offsetof(OrderRestated, appendage_length), true, false};
    case 'X':
      return {offsetof(MassCancelResponse, appendage_length), true, false};
    case 'G':
    case 'K':
      return {offsetof(OrderEntryControlResponse, appendage_length), true, false};
    case 'Q':
      return {offsetof(AccountQueryResponse, appendage_length), true, true};
    default:
      return {};
  }
}

std::size_t inbound_base(char type) noexcept {
  switch (type) {
    case 'O':
      return offsetof(EnterOrder, appendage_length);
    case 'U':
      return offsetof(ReplaceOrder, appendage_length);
    case 'X':
      return offsetof(CancelOrder, appendage_length);
    case 'M':
      return offsetof(ModifyOrder, appendage_length);
    case 'C':
      return offsetof(MassCancel, appendage_length);
    case 'D':
    case 'E':
      return offsetof(OrderEntryControl, appendage_length);
    case 'Q':
      return offsetof(AccountQuery, appendage_length);
    default:
      return 0;
  }
}

bool split_message(std::span<const std::byte> msg,
                   std::size_t base,
                   bool optional,
                   std::span<const std::byte>& appendage) noexcept {
  appendage = {};
  if (msg.size() < base) return false;
  if (msg.size() < base + 2) return optional && msg.size() == base;
  const std::size_t n = nasdaq::load_be16(msg.data() + base);
  if (msg.size() < base + 2 + n) return false;
  appendage = msg.subspan(base + 2, n);
  // TagValue structure: every element's Length must stay inside the appendage.
  std::size_t off = 0;
  while (off < n) {
    const auto len = std::to_integer<std::size_t>(appendage[off]);
    if (len == 0 || n - off - 1 < len) return false;
    off += 1 + len;
  }
  return true;
}

bool append_option(std::span<std::byte> out,
                   std::size_t& used,
                   OptionTag tag,
                   std::span<const std::byte> value) noexcept {
  if (value.size() > 254 || out.size() < used + 2 + value.size()) return false;
  out[used] = static_cast<std::byte>(1 + value.size());
  out[used + 1] = static_cast<std::byte>(tag);
  if (!value.empty()) std::memcpy(out.data() + used + 2, value.data(), value.size());
  used += 2 + value.size();
  return true;
}

bool find_option(std::span<const std::byte> appendage,
                 OptionTag tag,
                 std::span<const std::byte>& value) noexcept {
  std::size_t off = 0;
  while (off < appendage.size()) {
    const auto len = std::to_integer<std::size_t>(appendage[off]);
    if (len == 0 || appendage.size() - off - 1 < len) return false;
    if (static_cast<OptionTag>(appendage[off + 1]) == tag) {
      value = appendage.subspan(off + 2, len - 1);
      return true;
    }
    off += 1 + len;
  }
  return false;
}

std::string_view reject_reason_text(std::uint16_t reason) noexcept {
  switch (reason) {  // "Order Reject Reasons" appendix
    case 0x0001:
      return "Quote Unavailable";
    case 0x0002:
      return "Destination Closed";
    case 0x0003:
      return "Invalid Display";
    case 0x0004:
      return "Invalid Max Floor";
    case 0x0005:
      return "Invalid Peg Type";
    case 0x0006:
      return "Fat Finger";
    case 0x0007:
      return "Halted";
    case 0x0008:
      return "ISO Not Allowed";
    case 0x0009:
      return "Invalid Side";
    case 0x000A:
      return "Processing Error";
    case 0x000B:
      return "Cancel Pending";
    case 0x000C:
      return "Firm Not Authorized";
    case 0x000D:
      return "Invalid Min Quantity";
    case 0x000E:
      return "No Closing Reference Price";
    case 0x000F:
      return "Other";
    case 0x0010:
      return "Cancel Not Allowed";
    case 0x0011:
      return "Pegging Not Allowed";
    case 0x0012:
      return "Crossed Market";
    case 0x0013:
      return "Invalid Quantity";
    case 0x0014:
      return "Invalid Cross Order";
    case 0x0015:
      return "Replace Not Allowed";
    case 0x0016:
      return "Routing Not Allowed";
    case 0x0017:
      return "Invalid Symbol";
    case 0x0018:
      return "Test";
    case 0x0019:
      return "Late LOC Too Aggressive";
    case 0x001A:
      return "Retail Not Allowed";
    case 0x001B:
      return "Invalid Midpoint Post Only Price";
    case 0x001C:
      return "Invalid Destination";
    case 0x001D:
      return "Invalid Price";
    case 0x001E:
      return "Shares Exceed Threshold";
    case 0x001F:
      return "Exceeds Maximum Allowed Notional Value";
    case 0x0020:
      return "Risk: Aggregate Exposure Exceeded";
    case 0x0021:
      return "Risk: Market Impact";
    case 0x0022:
      return "Risk: Restricted Stock";
    case 0x0023:
      return "Risk: Short Sell Restricted";
    case 0x0024:
      return "Risk: ISO Not Allowed";
    case 0x0025:
      return "Risk: Exceeds ADV Limit";
    case 0x0026:
      return "Risk: Fat Finger";
    case 0x0027:
      return "Risk: Locate Required";
    case 0x0028:
      return "Risk: Symbol Message Rate Restriction";
    case 0x0029:
      return "Risk: Port Message Rate Restriction";
    case 0x002A:
      return "Risk: Duplicate Message Rate Restriction";
    case 0x002B:
      return "Risk: Short Sell Not Allowed";
    case 0x002C:
      return "Risk: Market Order Not Allowed";
    case 0x002D:
      return "Risk: Pre-Market Not Allowed";
    case 0x002E:
      return "Risk: Post-Market Not Allowed";
    case 0x002F:
      return "Risk: Short Sell Exempt Not Allowed";
    case 0x0030:
      return "Risk: Single Order Notional Exceeded";
    case 0x0031:
      return "Risk: Max Quantity Exceeded";
    case 0x0032:
      return "Reg SHO State Not Available";
    case 0x0033:
      return "Risk: IPO Market Buy Not Allowed";
    case 0x0040:
      return "Invalid AIQ";
    default:
      return "Unknown reject reason";
  }
}

// ---- UserRefMap ----------------------------------------------------------------------------

std::uint32_t UserRefMap::assign(ClientOrderId id) noexcept {
  if (const std::uint32_t* have = by_id_.find(id.value)) return *have;
  if (next_ == 0 || by_id_.size() >= decltype(by_id_)::kMaxSize ||
      by_urn_.size() >= decltype(by_urn_)::kMaxSize)
    return 0;
  const std::uint32_t urn = next_;
  by_id_.insert(id.value, urn);
  by_urn_.insert(urn, id);
  ++next_;  // wraps to 0 after 0xFFFFFFFF: the map then refuses new ids
  return urn;
}

std::uint32_t UserRefMap::find(ClientOrderId id) const noexcept {
  const std::uint32_t* p = by_id_.find(id.value);
  return p == nullptr ? 0 : *p;
}

ClientOrderId UserRefMap::find(std::uint32_t user_ref_num) const noexcept {
  const ClientOrderId* p = by_urn_.find(user_ref_num);
  return p == nullptr ? ClientOrderId{} : *p;
}

void UserRefMap::erase(std::uint32_t user_ref_num) noexcept {
  if (const ClientOrderId* p = by_urn_.find(user_ref_num)) {
    by_id_.erase(p->value);
    by_urn_.erase(user_ref_num);
  }
}

// ---- encoder -------------------------------------------------------------------------------

char OuchEncoder::time_in_force(TimeInForce tif) noexcept {
  switch (tif) {
    case TimeInForce::Ioc:
    case TimeInForce::Fok:
      return kTifIoc;
    case TimeInForce::Day:
    case TimeInForce::Gtc:
      return kTifDay;
  }
  return kTifDay;
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
  if (FASTMM_UNLIKELY(cmd.type == OrderType::Market)) {
    ++stats_.unsupported;
    return 0;
  }
  const char* symbol = symbols_.symbol8(cmd.instrument);
  if (FASTMM_UNLIKELY(symbol == nullptr)) {
    ++stats_.unknown_instrument;
    return 0;
  }
  std::uint32_t qty = 0;
  std::uint64_t price = 0;
  if (FASTMM_UNLIKELY(!nasdaq::qty_to_shares(cmd.qty, qty) || qty == 0 || qty > kMaxQuantity ||
                      !nasdaq::price_to_price4(cmd.price, price) || price == 0 ||
                      price > kMaxPrice)) {
    ++stats_.bad_value;
    return 0;
  }
  // Options appendage (at most Firm 6 + MinQty 6 + PostOnly 3 bytes).
  std::array<std::byte, 32> app{};
  std::size_t app_len = 0;
  if (!blank4(cfg_.firm))
    append_option(
        app, app_len, OptionTag::Firm, std::as_bytes(std::span<const char>(cfg_.firm, 4)));
  if (cmd.tif == TimeInForce::Fok) {
    std::array<std::byte, 4> v{};
    be32_t be{};
    be.set(qty);
    std::memcpy(v.data(), &be, 4);
    append_option(app, app_len, OptionTag::MinQty, v);
  }
  if (cmd.type == OrderType::PostOnly) {
    const std::array<std::byte, 1> v{std::byte{'P'}};
    append_option(app, app_len, OptionTag::PostOnly, v);
  }
  const std::size_t total = sizeof(EnterOrder) + app_len;
  if (FASTMM_UNLIKELY(out.size() < total)) {
    ++stats_.buffer_too_small;
    return 0;
  }
  const std::uint32_t urn = ids_.assign(cmd.cl_ord_id);
  if (FASTMM_UNLIKELY(urn == 0)) {
    ++stats_.id_table_full;
    return 0;
  }
  EnterOrder m{};
  m.type = 'O';
  m.user_ref_num.set(urn);
  m.side = ouch::side_code(cmd.side);
  m.quantity.set(qty);
  std::memcpy(m.symbol, symbol, sizeof m.symbol);
  m.price.set(price);
  m.time_in_force = time_in_force(cmd.tif);
  m.display = cfg_.display;
  m.capacity = cfg_.capacity;
  m.intermarket_sweep_eligibility = cfg_.intermarket_sweep;
  m.cross_type = cfg_.cross_type;
  put_cl_ord_id(m.cl_ord_id, cmd.cl_ord_id);
  m.appendage_length.set(static_cast<std::uint16_t>(app_len));
  ouch::put(out, m);
  if (app_len != 0) std::memcpy(out.data() + sizeof(EnterOrder), app.data(), app_len);
  ++stats_.encoded;
  return total;
}

std::size_t OuchEncoder::encode_replace(const venues::OrderCommand& cmd,
                                        std::span<std::byte> out) noexcept {
  if (FASTMM_UNLIKELY(out.size() < sizeof(ReplaceOrder))) {
    ++stats_.buffer_too_small;
    return 0;
  }
  std::uint32_t qty = 0;
  std::uint64_t price = 0;
  if (FASTMM_UNLIKELY(!nasdaq::qty_to_shares(cmd.qty, qty) || qty == 0 || qty > kMaxQuantity ||
                      !nasdaq::price_to_price4(cmd.price, price) || price == 0 ||
                      price > kMaxPrice)) {
    ++stats_.bad_value;
    return 0;
  }
  const std::uint32_t orig = ids_.find(cmd.orig_cl_ord_id);
  if (FASTMM_UNLIKELY(orig == 0)) {
    ++stats_.unknown_order;
    return 0;
  }
  const std::uint32_t urn = ids_.assign(cmd.cl_ord_id);
  if (FASTMM_UNLIKELY(urn == 0)) {
    ++stats_.id_table_full;
    return 0;
  }
  ReplaceOrder m{};
  m.type = 'U';
  m.orig_user_ref_num.set(orig);
  m.user_ref_num.set(urn);
  m.quantity.set(qty);
  m.price.set(price);
  m.time_in_force = cfg_.replace_time_in_force;
  m.display = cfg_.display;
  m.intermarket_sweep_eligibility = cfg_.intermarket_sweep;
  put_cl_ord_id(m.cl_ord_id, cmd.cl_ord_id);
  m.appendage_length.set(0);
  ++stats_.encoded;
  return ouch::put(out, m);
}

std::size_t OuchEncoder::encode_cancel(const venues::OrderCommand& cmd,
                                       std::span<std::byte> out) noexcept {
  if (FASTMM_UNLIKELY(out.size() < sizeof(CancelOrder))) {
    ++stats_.buffer_too_small;
    return 0;
  }
  const std::uint32_t urn = ids_.find(cmd.cl_ord_id);
  if (FASTMM_UNLIKELY(urn == 0)) {
    ++stats_.unknown_order;
    return 0;
  }
  CancelOrder m{};
  m.type = 'X';
  m.user_ref_num.set(urn);
  m.quantity.set(0);
  m.appendage_length.set(0);
  ++stats_.encoded;
  return ouch::put(out, m);
}

// ---- decoder -------------------------------------------------------------------------------

ClientOrderId OuchDecoder::resolve(std::uint32_t urn, const char* cl_ord_id14) const noexcept {
  if (ids_ != nullptr) {
    const ClientOrderId id = ids_->find(urn);
    if (id.valid()) return id;
  }
  if (cl_ord_id14 != nullptr) {
    if (const auto id = decode_cl_ord_id(std::string_view(cl_ord_id14, kClOrdIdChars))) return *id;
  }
  if (const ouch::OrderEntry* e = orders_.find(urn)) return e->cl_ord_id;
  return ClientOrderId{};
}

void OuchDecoder::forget(std::uint32_t urn) noexcept {
  orders_.erase(urn);
  if (ids_ != nullptr) ids_->erase(urn);
}

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
  const OutboundLayout layout = outbound_layout(type);
  if (FASTMM_UNLIKELY(layout.base == 0)) {
    ++stats_.unknown_type;
    return ParseStatus::Ignored;
  }
  std::span<const std::byte> appendage;
  const bool valid = layout.has_appendage
                         ? split_message(in, layout.base, layout.appendage_optional, appendage)
                         : in.size() >= layout.base;
  if (FASTMM_UNLIKELY(!valid)) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  const std::byte* p = in.data();
  switch (type) {
    case 'A':
      return on_accepted(ouch::view_as<OrderAccepted>(p), rx_ts, sink);
    case 'U':
      return on_replaced(ouch::view_as<OrderReplaced>(p), rx_ts, sink);
    case 'C': {
      const auto& m = ouch::view_as<OrderCanceled>(p);  // reads stop before appendage_length
      return on_canceled(
          m.timestamp.get(), m.user_ref_num.get(), m.quantity.get(), m.reason, rx_ts, sink);
    }
    case 'D': {
      const auto& m = ouch::view_as<AiqCanceled>(p);
      return on_canceled(
          m.timestamp.get(), m.user_ref_num.get(), m.decrement_shares.get(), m.reason, rx_ts, sink);
    }
    case 'E':
      return on_executed(ouch::view_as<OrderExecuted>(p), rx_ts, sink);
    case 'J': {
      const auto& m = ouch::view_as<Rejected>(p);
      const std::uint32_t urn = m.user_ref_num.get();
      const ClientOrderId id = resolve(urn, m.cl_ord_id);
      if (!id.valid()) {
        ++stats_.foreign_id;
        return ParseStatus::Ignored;
      }
      forget(urn);
      const std::uint16_t reason = m.reason.get();
      return done(ouch::emit_reject(sink,
                                    stamp(m.timestamp.get(), InstrumentId{}, rx_ts),
                                    id,
                                    static_cast<std::int32_t>(reason),
                                    reject_reason_text(reason)),
                  1);
    }
    case 'I': {
      const auto& m = ouch::view_as<CancelReject>(p);
      const std::uint32_t urn = m.user_ref_num.get();
      const ClientOrderId id = resolve(urn, nullptr);
      if (!id.valid()) {
        ++stats_.unknown_order;
        return ParseStatus::Ignored;
      }
      const ouch::OrderEntry* e = orders_.find(urn);
      return done(
          ouch::emit_cancel_reject(
              sink,
              stamp(m.timestamp.get(), e != nullptr ? e->instrument : InstrumentId{}, rx_ts),
              id,
              static_cast<std::int32_t>('I'),
              "Cancel Reject"),
          1);
    }
    case 'M': {
      const auto& m = ouch::view_as<OrderModified>(p);
      if (ouch::OrderEntry* e = orders_.find(m.user_ref_num.get())) {
        e->leaves = nasdaq::shares_to_qty(m.quantity.get());
        static_cast<void>(ouch::side_from_code(m.side, e->side));
      }
      ++stats_.ignored;
      return ParseStatus::Ignored;
    }
    case 'Q': {
      const auto& m = ouch::view_as<AccountQueryResponse>(p);
      if (ids_ != nullptr) ids_->set_next(m.next_user_ref_num.get());
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
    default:  // P T R X G K
      ++stats_.ignored;
      return ParseStatus::Ignored;
  }
}

ParseStatus OuchDecoder::on_accepted(const OrderAccepted& m,
                                     std::int64_t rx,
                                     venues::EventSink& sink) noexcept {
  const std::uint32_t urn = m.user_ref_num.get();
  const ClientOrderId id = resolve(urn, m.cl_ord_id);
  if (!id.valid()) {
    ++stats_.foreign_id;
    return ParseStatus::Ignored;
  }
  ouch::OrderEntry e{};
  if (FASTMM_UNLIKELY(!ouch::side_from_code(m.side, e.side))) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  e.cl_ord_id = id;
  e.reference_number = m.order_reference_number.get();
  e.leaves = nasdaq::shares_to_qty(m.quantity.get());
  e.instrument = symbols_.find(m.symbol);
  const ouch::EventStamp st = stamp(m.timestamp.get(), e.instrument, rx);
  if (!ouch::emit_ack(sink, st, id, e.reference_number)) return done(false, 0);
  if (m.order_state == 'D') {
    forget(urn);
    return done(ouch::emit_expired(sink, st, e), 2);
  }
  if (orders_.assign(urn, e) == nullptr) ++stats_.table_full;
  return done(true, 1);
}

ParseStatus OuchDecoder::on_replaced(const OrderReplaced& m,
                                     std::int64_t rx,
                                     venues::EventSink& sink) noexcept {
  const std::uint32_t urn = m.user_ref_num.get();
  const std::uint32_t orig = m.orig_user_ref_num.get();
  const ClientOrderId id = resolve(urn, m.cl_ord_id);
  if (!id.valid()) {
    ++stats_.foreign_id;
    return ParseStatus::Ignored;
  }
  ouch::OrderEntry e{};
  if (FASTMM_UNLIKELY(!ouch::side_from_code(m.side, e.side))) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  e.cl_ord_id = id;
  e.reference_number = m.order_reference_number.get();
  e.leaves = nasdaq::shares_to_qty(m.quantity.get());
  e.instrument = symbols_.find(m.symbol);
  if (const ouch::OrderEntry* old = orders_.find(orig)) e.cum = old->cum;
  forget(orig);
  const ouch::EventStamp st = stamp(m.timestamp.get(), e.instrument, rx);
  if (!ouch::emit_ack(sink, st, id, e.reference_number)) return done(false, 0);
  if (m.order_state == 'D') {
    forget(urn);
    return done(ouch::emit_expired(sink, st, e), 2);
  }
  if (orders_.assign(urn, e) == nullptr) ++stats_.table_full;
  return done(true, 1);
}

ParseStatus OuchDecoder::on_canceled(std::uint64_t ts,
                                     std::uint32_t urn,
                                     std::uint32_t decrement,
                                     char reason,
                                     std::int64_t rx,
                                     venues::EventSink& sink) noexcept {
  ouch::OrderEntry* e = orders_.find(urn);
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
  forget(urn);
  const ouch::EventStamp st = stamp(ts, copy.instrument, rx);
  const bool expired = reason == 'I' || reason == 'T';
  return done(expired ? ouch::emit_expired(sink, st, copy) : ouch::emit_cancel_ack(sink, st, copy),
              1);
}

ParseStatus OuchDecoder::on_executed(const OrderExecuted& m,
                                     std::int64_t rx,
                                     venues::EventSink& sink) noexcept {
  const std::uint32_t urn = m.user_ref_num.get();
  ouch::OrderEntry* e = orders_.find(urn);
  if (e == nullptr) {
    ++stats_.unknown_order;
    return ParseStatus::Ignored;
  }
  Price price;
  if (FASTMM_UNLIKELY(!nasdaq::price4_to_price(m.price.get(), price))) {
    ++stats_.malformed;
    return ParseStatus::Malformed;
  }
  const Qty qty = nasdaq::shares_to_qty(m.quantity.get());
  e->cum += qty;
  e->leaves = qty >= e->leaves ? Qty{} : e->leaves - qty;
  const ouch::OrderEntry copy = *e;
  if (!copy.leaves.is_positive()) forget(urn);
  return done(ouch::emit_fill(sink,
                              stamp(m.timestamp.get(), copy.instrument, rx),
                              copy,
                              m.match_number.get(),
                              price,
                              qty,
                              ouch::liquidity_from_flag(m.liquidity_flag)),
              1);
}

// ---- host builders -------------------------------------------------------------------------

bool put_seq_token(char* cl_ord_id14, std::uint64_t venue_seq) noexcept {
  if (venue_seq == 0 || venue_seq > kMaxSeqToken) return false;
  cl_ord_id14[0] = kSeqTokenPrefix;
  for (std::size_t i = kClOrdIdChars; i > 1; --i) {
    cl_ord_id14[i - 1] = static_cast<char>('0' + static_cast<int>(venue_seq % 10U));
    venue_seq /= 10U;
  }
  return true;
}

std::uint64_t parse_seq_token(const char* cl_ord_id14) noexcept {
  if (cl_ord_id14[0] != kSeqTokenPrefix) return 0;
  std::uint64_t v = 0;
  for (std::size_t i = 1; i < kClOrdIdChars; ++i) {
    const char c = cl_ord_id14[i];
    if (c < '0' || c > '9') return 0;
    v = v * 10U + static_cast<std::uint64_t>(c - '0');
  }
  return v;
}

namespace host {

bool parse_enter(std::span<const std::byte> msg, EnterView& out) noexcept {
  std::span<const std::byte> app;
  if (msg.empty() || static_cast<char>(msg[0]) != 'O' ||
      !split_message(msg, offsetof(EnterOrder, appendage_length), false, app))
    return false;
  const auto& m = ouch::view_as<EnterOrder>(msg.data());
  out = EnterView{};
  out.raw = &m;
  out.user_ref_num = m.user_ref_num.get();
  if (!ouch::side_from_code(m.side, out.side)) return false;
  const std::uint32_t n = m.quantity.get();
  if (n == 0 || !nasdaq::shares_to_qty(n, out.qty)) return false;
  if (!nasdaq::price4_to_price(m.price.get(), out.price)) return false;
  out.symbol = nasdaq::get_alpha_trimmed(m.symbol, sizeof m.symbol);
  std::span<const std::byte> v;
  if (find_option(app, OptionTag::MinQty, v) && v.size() == 4) {
    be32_t min{};
    std::memcpy(&min, v.data(), 4);
    out.min_qty = min.get();
  }
  if (m.time_in_force == kTifIoc) out.tif = out.min_qty == n ? TimeInForce::Fok : TimeInForce::Ioc;
  out.post_only =
      find_option(app, OptionTag::PostOnly, v) && v.size() == 1 && static_cast<char>(v[0]) == 'P';
  out.seq_token = parse_seq_token(m.cl_ord_id);
  return true;
}

bool parse_replace(std::span<const std::byte> msg, ReplaceView& out) noexcept {
  std::span<const std::byte> app;
  if (msg.empty() || static_cast<char>(msg[0]) != 'U' ||
      !split_message(msg, offsetof(ReplaceOrder, appendage_length), false, app))
    return false;
  const auto& m = ouch::view_as<ReplaceOrder>(msg.data());
  out = ReplaceView{};
  out.raw = &m;
  out.orig_user_ref_num = m.orig_user_ref_num.get();
  out.user_ref_num = m.user_ref_num.get();
  out.seq_token = parse_seq_token(m.cl_ord_id);
  if (!nasdaq::shares_to_qty(m.quantity.get(), out.qty)) return false;
  return nasdaq::price4_to_price(m.price.get(), out.price);
}

bool parse_cancel(std::span<const std::byte> msg, CancelView& out) noexcept {
  std::span<const std::byte> app;
  if (msg.empty() || static_cast<char>(msg[0]) != 'X' ||
      !split_message(msg, offsetof(CancelOrder, appendage_length), false, app))
    return false;
  const auto& m = ouch::view_as<CancelOrder>(msg.data());
  out.user_ref_num = m.user_ref_num.get();
  out.quantity = m.quantity.get();
  return true;
}

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
                     std::uint32_t quantity,
                     std::uint64_t reference_number,
                     char order_state) noexcept {
  OrderAccepted m{};
  m.type = 'A';
  m.timestamp.set(ts);
  m.user_ref_num = in.user_ref_num;
  m.side = in.side;
  m.quantity.set(quantity);
  std::memcpy(m.symbol, in.symbol, sizeof m.symbol);
  m.price = in.price;
  m.time_in_force = in.time_in_force;
  m.display = in.display;
  m.order_reference_number.set(reference_number);
  m.capacity = in.capacity;
  m.intermarket_sweep_eligibility = in.intermarket_sweep_eligibility;
  m.cross_type = in.cross_type;
  m.order_state = order_state;
  std::memcpy(m.cl_ord_id, in.cl_ord_id, sizeof m.cl_ord_id);
  m.appendage_length.set(0);
  return ouch::put(out, m);
}

std::size_t replaced(std::span<std::byte> out,
                     std::uint64_t ts,
                     const ReplaceOrder& in,
                     const EnterOrder& original,
                     std::uint32_t quantity_outstanding,
                     std::uint64_t reference_number,
                     char order_state) noexcept {
  OrderReplaced m{};
  m.type = 'U';
  m.timestamp.set(ts);
  m.orig_user_ref_num = in.orig_user_ref_num;
  m.user_ref_num = in.user_ref_num;
  m.side = original.side;
  m.quantity.set(quantity_outstanding);
  std::memcpy(m.symbol, original.symbol, sizeof m.symbol);
  m.price = in.price;
  m.time_in_force = in.time_in_force;
  m.display = in.display;
  m.order_reference_number.set(reference_number);
  m.capacity = original.capacity;
  m.intermarket_sweep_eligibility = in.intermarket_sweep_eligibility;
  m.cross_type = original.cross_type;
  m.order_state = order_state;
  std::memcpy(m.cl_ord_id, in.cl_ord_id, sizeof m.cl_ord_id);
  m.appendage_length.set(0);
  return ouch::put(out, m);
}

std::size_t canceled(std::span<std::byte> out,
                     std::uint64_t ts,
                     std::uint32_t user_ref_num,
                     std::uint32_t quantity,
                     char reason) noexcept {
  OrderCanceled m{};
  m.type = 'C';
  m.timestamp.set(ts);
  m.user_ref_num.set(user_ref_num);
  m.quantity.set(quantity);
  m.reason = reason;
  m.appendage_length.set(0);
  return ouch::put(out, m);
}

std::size_t executed(std::span<std::byte> out,
                     std::uint64_t ts,
                     std::uint32_t user_ref_num,
                     std::uint32_t quantity,
                     std::uint64_t price,
                     char liquidity_flag,
                     std::uint64_t match_number) noexcept {
  OrderExecuted m{};
  m.type = 'E';
  m.timestamp.set(ts);
  m.user_ref_num.set(user_ref_num);
  m.quantity.set(quantity);
  m.price.set(price);
  m.liquidity_flag = liquidity_flag;
  m.match_number.set(match_number);
  m.appendage_length.set(0);
  return ouch::put(out, m);
}

std::size_t rejected(std::span<std::byte> out,
                     std::uint64_t ts,
                     std::uint32_t user_ref_num,
                     std::uint16_t reason,
                     const char* cl_ord_id14) noexcept {
  Rejected m{};
  m.type = 'J';
  m.timestamp.set(ts);
  m.user_ref_num.set(user_ref_num);
  m.reason.set(reason);
  std::memcpy(m.cl_ord_id, cl_ord_id14, sizeof m.cl_ord_id);
  m.appendage_length.set(0);
  return ouch::put(out, m);
}

std::size_t cancel_reject(std::span<std::byte> out,
                          std::uint64_t ts,
                          std::uint32_t user_ref_num) noexcept {
  CancelReject m{};
  m.type = 'I';
  m.timestamp.set(ts);
  m.user_ref_num.set(user_ref_num);
  m.appendage_length.set(0);
  return ouch::put(out, m);
}

std::size_t account_query_response(std::span<std::byte> out,
                                   std::uint64_t ts,
                                   std::uint32_t next_user_ref_num) noexcept {
  AccountQueryResponse m{};
  m.type = 'Q';
  m.timestamp.set(ts);
  m.next_user_ref_num.set(next_user_ref_num);
  m.appendage_length.set(0);
  return ouch::put(out, m);
}

}  // namespace host

}  // namespace fastmm::codecs::ouch50
