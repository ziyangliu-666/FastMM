#pragma once
// OrderCommand: a uniform, non-owning view over the three outbound engine messages
// (OutNewOrderMsg / OutCancelMsg / OutReplaceMsg) so encoders switch on one enum instead of
// three message types. Built from the ring pointer with no copy.
#include "fastmm/core/messages.hpp"

#include <cstdint>
#include <optional>

namespace fastmm::venues {

enum class OrderCommandKind : std::uint8_t { New = 0, Cancel = 1, Replace = 2 };
[[nodiscard]] constexpr std::string_view to_string(OrderCommandKind k) noexcept {
  switch (k) {
    case OrderCommandKind::New:
      return "New";
    case OrderCommandKind::Cancel:
      return "Cancel";
    case OrderCommandKind::Replace:
      return "Replace";
  }
  return "?";
}

struct OrderCommand {
  OrderCommandKind kind = OrderCommandKind::New;
  InstrumentId instrument{};
  VenueId venue{};
  ClientOrderId cl_ord_id{};       // New: the order; Cancel: order to cancel; Replace: new id
  ClientOrderId orig_cl_ord_id{};  // Replace only
  const VenueOrderId* venue_order_id = nullptr;  // Cancel/Replace: may be empty
  Side side = Side::Buy;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  bool reduce_only = false;
  Price price{};
  Qty qty{};
  const EventHeader* header = nullptr;

  // Receive stamp of the inbound event that triggered the order (0 when none, e.g. timers).
  [[nodiscard]] Cycles t0_cycles() const noexcept {
    return header != nullptr ? header->t0_cycles : Cycles{};
  }

  // nullopt for any non-outbound message type.
  [[nodiscard]] static std::optional<OrderCommand> from(const EventHeader& h) noexcept {
    OrderCommand c;
    c.header = &h;
    c.instrument = h.instrument;
    c.venue = h.venue;
    switch (h.type) {
      case EventType::OutNewOrder: {
        const auto& m = msg_cast<OutNewOrderMsg>(&h);
        c.kind = OrderCommandKind::New;
        c.cl_ord_id = m.cl_ord_id;
        c.side = m.side;
        c.type = m.type;
        c.tif = m.tif;
        c.reduce_only = m.reduce_only != 0;
        c.price = m.price;
        c.qty = m.qty;
        return c;
      }
      case EventType::OutCancel: {
        const auto& m = msg_cast<OutCancelMsg>(&h);
        c.kind = OrderCommandKind::Cancel;
        c.cl_ord_id = m.cl_ord_id;
        c.venue_order_id = &m.venue_order_id;
        return c;
      }
      case EventType::OutReplace: {
        const auto& m = msg_cast<OutReplaceMsg>(&h);
        c.kind = OrderCommandKind::Replace;
        c.cl_ord_id = m.cl_ord_id;
        c.orig_cl_ord_id = m.orig_cl_ord_id;
        c.venue_order_id = &m.venue_order_id;
        c.price = m.price;
        c.qty = m.qty;
        return c;
      }
      default:
        return std::nullopt;
    }
  }
};

// The last client order id (New or Replace) a venue has taken from the outbound ring. The value
// at the time a venue requests its open orders is stamped into that snapshot's ReconcileMsg Begin:
// the engine's orders above it had not been sent, so the snapshot cannot show them. One engine
// allocates its ids in the order it sends them, so the last is also the highest; fastmm-gateway
// interleaves several engines' ids on one venue and finds the point the snapshot was taken at from
// the last one (live/gateway.hpp).
class SentWatermark {
 public:
  void note(const OrderCommand& c) noexcept {
    if (c.kind != OrderCommandKind::Cancel) high_ = c.cl_ord_id;
  }
  [[nodiscard]] ClientOrderId value() const noexcept { return high_; }
  static void stamp(ReconcileMsg& begin, ClientOrderId watermark) noexcept {
    begin.sent_watermark = watermark;
    begin.flags |= ReconcileMsg::kSentWatermark;
  }

 private:
  ClientOrderId high_{};
};

// The engine asks for a reconciliation (ControlCommand::Reconcile on the outbound ring).
[[nodiscard]] inline bool is_reconcile_request(const EventHeader& h) noexcept {
  return h.type == EventType::Control &&
         msg_cast<ControlMsg>(&h).command == ControlCommand::Reconcile;
}

}  // namespace fastmm::venues
