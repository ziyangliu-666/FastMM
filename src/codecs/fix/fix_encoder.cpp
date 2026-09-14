#include "fastmm/codecs/fix/fix_encoder.hpp"

#include "fastmm/codecs/fix/fix_builder.hpp"

#include <bit>

namespace fastmm::codecs::fix {

using venues::OrderCommand;
using venues::OrderCommandKind;

namespace {

constexpr char side_char(Side s) noexcept {
  return s == Side::Sell ? kSideSell : kSideBuy;
}
constexpr char ord_type_char(OrderType t) noexcept {
  return t == OrderType::Market ? kOrdTypeMarket : kOrdTypeLimit;
}
constexpr char tif_char(TimeInForce t) noexcept {
  switch (t) {
    case TimeInForce::Gtc:
      return kTifGtc;
    case TimeInForce::Ioc:
      return kTifIoc;
    case TimeInForce::Fok:
      return kTifFok;
    case TimeInForce::Day:
      return kTifDay;
  }
  return kTifGtc;
}

}  // namespace

FixEncoder::FixEncoder(const FixSession& session,
                       const FixSymbolTable& symbols,
                       std::size_t order_slots)
    : session_(&session),
      symbols_(&symbols),
      mask_(std::bit_ceil(order_slots < 2 ? std::size_t{2} : order_slots) - 1),
      orders_(std::make_unique<OrderInfo[]>(mask_ + 1)),
      buf_(std::make_unique<std::byte[]>(kMaxOrderMessage)) {}

void FixEncoder::remember(ClientOrderId id,
                          InstrumentId instrument,
                          Side side,
                          OrderType type,
                          TimeInForce tif,
                          Price price,
                          Qty qty) noexcept {
  orders_[id.value & mask_] = OrderInfo{id, price, qty, instrument, side, type, tif, 0};
}

const FixEncoder::OrderInfo* FixEncoder::find(ClientOrderId id) const noexcept {
  const OrderInfo& o = orders_[id.value & mask_];
  return o.id == id && id.valid() ? &o : nullptr;
}

std::size_t FixEncoder::fail(std::uint64_t& counter) noexcept {
  ++counter;
  ++stats_.failures;
  return 0;
}

std::size_t FixEncoder::encode(const OrderCommand& cmd, std::span<std::byte> out) noexcept {
  FixBuilder b(out);
  const std::int64_t now = session_->now_ns();
  const std::string_view sender = session_->sender_comp_id();
  const std::string_view target = session_->target_comp_id();
  const std::uint64_t seq = session_->next_sender_seq();
  const std::string_view bs = session_->begin_string();

  switch (cmd.kind) {
    case OrderCommandKind::New: {
      const std::string_view sym = symbols_->symbol(cmd.instrument);
      if (sym.empty()) return fail(stats_.unknown_symbols);
      b.begin_header(msg::kNewOrderSingle, sender, target, seq, now, false, 0, bs);
      b.field(tag::kClOrdID, encode_cl_ord_id(cmd.cl_ord_id).view());
      if (cmd.type == OrderType::PostOnly)
        b.field_char(tag::kExecInst, kExecInstParticipateDontInitiate);
      b.field(tag::kSymbol, sym)
          .field_char(tag::kSide, side_char(cmd.side))
          .field_timestamp(tag::kTransactTime, now)
          .field_decimal(tag::kOrderQty, cmd.qty)
          .field_char(tag::kOrdType, ord_type_char(cmd.type));
      if (cmd.type != OrderType::Market) b.field_decimal(tag::kPrice, cmd.price);
      b.field_char(tag::kTimeInForce, tif_char(cmd.tif));
      const std::size_t n = b.finish();
      if (n == 0) return fail(stats_.failures);
      remember(cmd.cl_ord_id, cmd.instrument, cmd.side, cmd.type, cmd.tif, cmd.price, cmd.qty);
      ++stats_.new_orders;
      return n;
    }
    case OrderCommandKind::Cancel: {
      const OrderInfo* o = find(cmd.cl_ord_id);
      if (o == nullptr) return fail(stats_.unknown_orders);
      const std::string_view sym = symbols_->symbol(o->instrument);
      if (sym.empty()) return fail(stats_.unknown_symbols);
      const FixedString<16> orig = encode_cl_ord_id(cmd.cl_ord_id);
      // Cancel request id: "<order id>c<counter>".
      FixedString<40> req(orig.view());
      req.push_back('c');
      char digits[20];
      int nd = 0;
      std::uint64_t c = ++cancel_counter_;
      do {
        digits[nd++] = static_cast<char>('0' + c % 10);
        c /= 10;
      } while (c != 0);
      while (nd > 0) req.push_back(digits[--nd]);
      b.begin_header(msg::kOrderCancelRequest, sender, target, seq, now, false, 0, bs);
      b.field(tag::kOrigClOrdID, orig.view());
      if (cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty())
        b.field(tag::kOrderID, cmd.venue_order_id->view());
      b.field(tag::kClOrdID, req.view())
          .field(tag::kSymbol, sym)
          .field_char(tag::kSide, side_char(o->side))
          .field_timestamp(tag::kTransactTime, now)
          .field_decimal(tag::kOrderQty, o->qty);
      const std::size_t n = b.finish();
      if (n == 0) return fail(stats_.failures);
      ++stats_.cancels;
      return n;
    }
    case OrderCommandKind::Replace: {
      const OrderInfo* o = find(cmd.orig_cl_ord_id);
      if (o == nullptr) return fail(stats_.unknown_orders);
      const std::string_view sym = symbols_->symbol(o->instrument);
      if (sym.empty()) return fail(stats_.unknown_symbols);
      const OrderInfo info = *o;  // copy: remember() below may overwrite the slot
      b.begin_header(msg::kOrderCancelReplaceRequest, sender, target, seq, now, false, 0, bs);
      if (cmd.venue_order_id != nullptr && !cmd.venue_order_id->empty())
        b.field(tag::kOrderID, cmd.venue_order_id->view());
      b.field(tag::kOrigClOrdID, encode_cl_ord_id(cmd.orig_cl_ord_id).view())
          .field(tag::kClOrdID, encode_cl_ord_id(cmd.cl_ord_id).view());
      if (info.type == OrderType::PostOnly)
        b.field_char(tag::kExecInst, kExecInstParticipateDontInitiate);
      b.field(tag::kSymbol, sym)
          .field_char(tag::kSide, side_char(info.side))
          .field_timestamp(tag::kTransactTime, now)
          .field_decimal(tag::kOrderQty, cmd.qty)
          .field_char(tag::kOrdType, ord_type_char(info.type));
      if (info.type != OrderType::Market) b.field_decimal(tag::kPrice, cmd.price);
      b.field_char(tag::kTimeInForce, tif_char(info.tif));
      const std::size_t n = b.finish();
      if (n == 0) return fail(stats_.failures);
      remember(cmd.cl_ord_id, info.instrument, info.side, info.type, info.tif, cmd.price, cmd.qty);
      ++stats_.replaces;
      return n;
    }
  }
  return fail(stats_.failures);
}

bool FixEncoder::send(FixSession& session, const OrderCommand& cmd) noexcept {
  const std::size_t n = encode(cmd, std::span<std::byte>(buf_.get(), kMaxOrderMessage));
  if (n == 0) return false;
  return session.send_app(std::span<const char>(reinterpret_cast<const char*>(buf_.get()), n));
}

}  // namespace fastmm::codecs::fix
