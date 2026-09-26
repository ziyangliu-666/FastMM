#pragma once
// OrderCommand: a uniform, non-owning view over the three outbound engine messages
// (OutNewOrderMsg / OutCancelMsg / OutReplaceMsg) so encoders switch on one enum instead of
// three message types. Built from the ring pointer with no copy.
#include "fastmm/core/messages.hpp"

#include <cstddef>
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

// The watermark stamped into an open-order snapshot's ReconcileMsg Begin: the engine's orders at
// or below it can be judged by the snapshot, the ones above it cannot. It is the last client order
// id (New or Replace) the venue sent before the first one the venue has not answered yet (ack,
// reject, fill or cancel), in send order: an order still in flight when the snapshot is asked for
// may reach the matching engine, or the venue's open-order view, after the snapshot is taken
// (Binance Spot, 2026-09-26: an order sent 1 ms before openOrders.status and accepted by the
// matching engine was missing from the reply, and the engine cancelled it as gone). An order with
// no answer after kUnansweredNs no longer holds the watermark back.
//
// One engine allocates its ids in the order it sends them; fastmm-gateway interleaves several
// engines' ids on one venue and finds the point the snapshot was taken at from the id itself
// (live/gateway.hpp), which is why the watermark is always an id that was sent.
class SentWatermark {
 public:
  static constexpr std::int64_t kUnansweredNs = 10'000'000'000;
  static constexpr std::size_t kMaxInFlight = 256;

  void note(const OrderCommand& c, std::int64_t now_ns) noexcept {
    if (c.kind == OrderCommandKind::Cancel) return;
    prune(now_ns);
    if (n_ == kMaxInFlight) pop();  // treat the oldest as answered: never blocks new orders
    in_flight_[(head_ + n_) % kMaxInFlight] = InFlight{c.cl_ord_id, high_, now_ns, false};
    ++n_;
    high_ = c.cl_ord_id;
  }
  // The venue said something about order `id`.
  void answered(ClientOrderId id) noexcept {
    for (std::size_t k = 0; k < n_; ++k) {
      InFlight& f = in_flight_[(head_ + k) % kMaxInFlight];
      if (f.id == id) {
        f.answered = true;
        break;
      }
    }
  }
  // answered() for the order an event from the venue is about.
  void answered(const EventHeader& h) noexcept {
    switch (h.type) {
      case EventType::OrderAck:
        answered(msg_cast<OrderAckMsg>(&h).cl_ord_id);
        break;
      case EventType::OrderReject:
        answered(msg_cast<OrderRejectMsg>(&h).cl_ord_id);
        break;
      case EventType::OrderFill:
        answered(msg_cast<OrderFillMsg>(&h).cl_ord_id);
        break;
      case EventType::OrderCancelAck:
        answered(msg_cast<OrderCancelAckMsg>(&h).cl_ord_id);
        break;
      case EventType::OrderExpired:
        answered(msg_cast<OrderExpiredMsg>(&h).cl_ord_id);
        break;
      default:
        break;
    }
  }
  // The connection the requests went out on is gone: none of them will be answered on it.
  void connection_lost() noexcept {
    head_ = 0;
    n_ = 0;
  }
  // The watermark for a snapshot requested now.
  [[nodiscard]] ClientOrderId value(std::int64_t now_ns) noexcept {
    prune(now_ns);
    return n_ == 0 ? high_ : in_flight_[head_].prev;
  }
  // The last id sent, answered or not.
  [[nodiscard]] ClientOrderId last_sent() const noexcept { return high_; }
  static void stamp(ReconcileMsg& begin, ClientOrderId watermark) noexcept {
    begin.sent_watermark = watermark;
    begin.flags |= ReconcileMsg::kSentWatermark;
  }

 private:
  struct InFlight {
    ClientOrderId id;
    ClientOrderId prev;  // the id sent just before it
    std::int64_t sent_ns;
    bool answered;
  };
  void pop() noexcept {
    head_ = (head_ + 1) % kMaxInFlight;
    --n_;
  }
  void prune(std::int64_t now_ns) noexcept {
    while (n_ > 0) {
      const InFlight& f = in_flight_[head_];
      if (!f.answered && now_ns - f.sent_ns < kUnansweredNs) break;
      pop();
    }
  }

  InFlight in_flight_[kMaxInFlight] = {};
  std::size_t head_ = 0;
  std::size_t n_ = 0;
  ClientOrderId high_{};
};

// The engine asks for a reconciliation (ControlCommand::Reconcile on the outbound ring).
[[nodiscard]] inline bool is_reconcile_request(const EventHeader& h) noexcept {
  return h.type == EventType::Control &&
         msg_cast<ControlMsg>(&h).command == ControlCommand::Reconcile;
}

}  // namespace fastmm::venues
