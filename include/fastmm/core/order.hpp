#pragma once
// Order: the OMS's 128-byte record for one order across its lifetime.
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstdint>
#include <type_traits>

namespace fastmm {

struct Order {
  enum Flags : std::uint8_t {
    kPostOnly = 1U << 0,
    kReduceOnly = 1U << 1,
    kUnsolicitedCancel = 1U << 2,   // venue cancelled it (not requested by us)
    kReconciled = 1U << 3,          // state fixed up by reconciliation
    kSeenInReconcile = 1U << 4,     // scratch flag during a reconcile pass
    kSynthetic = 1U << 5,           // created from reconcile (venue had it, we did not)
    kReplaceOldCanceled = 1U << 6,  // cancel-then-new replace: old leg already cancelled
  };

  ClientOrderId cl_ord_id;          // 8
  VenueOrderId venue_order_id;      // 41 -> 49
  std::uint8_t pad0_[7];            // -> 56
  InstrumentId instrument;          // 4 -> 60
  VenueId venue;                    // 1
  Side side;                        // 1
  OrderType type;                   // 1
  TimeInForce tif;                  // 1 -> 64
  OrderState state;                 // 1
  RejectReason reject_reason;       // 1
  std::uint8_t flags;               // 1
  std::uint8_t cancel_attempts;     // 1
  std::uint32_t user_tag;           // 4 -> 72   (QuoteManager slot id etc.)
  Price price;                      // 8 -> 80
  Qty qty;                          // 8 -> 88
  Qty cum_qty;                      // 8 -> 96
  Timestamp created;                // 8 -> 104
  ClientOrderId pending_cl_ord_id;  // 8 -> 112 replace: id of the replacement order
  Price pending_price;              // 8 -> 120
  Qty pending_qty;                  // 8 -> 128

  [[nodiscard]] constexpr Qty leaves_qty() const noexcept { return qty - cum_qty; }
  [[nodiscard]] constexpr bool is_open() const noexcept { return !is_terminal(state); }
  [[nodiscard]] constexpr bool is_working() const noexcept {
    return state == OrderState::Live || state == OrderState::PartiallyFilled;
  }
  [[nodiscard]] constexpr bool has(Flags f) const noexcept { return (flags & f) != 0; }
};
static_assert(sizeof(Order) == 128 && std::is_trivially_copyable_v<Order>);

// When the order's working leg (the order, or its latest replacement once acknowledged) was sent
// and accepted. Kept by the OMS next to Order, not in it (Oms::times, OmsUpdate::times).
struct OrderTimes {
  Timestamp sent;       // engine clock when the order or replace went out
  Timestamp venue_ack;  // the ack's exch_ts: the venue's accept time (Binance: whole ms)
  Timestamp local_ack;  // the ack's recv_ts: when it reached us
};

struct LimitOrder;

struct NewOrderRequest {
  InstrumentId instrument;
  VenueId venue;
  Side side;
  OrderType type = OrderType::Limit;
  TimeInForce tif = TimeInForce::Gtc;
  bool post_only = false;
  bool reduce_only = false;
  Price price;
  Qty qty;
  std::uint32_t user_tag = 0;

  // A GTC limit order, refined with the builder's methods:
  //   ctx.send(NewOrderRequest::limit(id, Side::Buy, px, qty).post_only().tag(7));
  [[nodiscard]] static constexpr LimitOrder limit(InstrumentId instrument,
                                                  Side side,
                                                  Price price,
                                                  Qty qty) noexcept;
};

// Chainable builder returned by NewOrderRequest::limit(); converts to NewOrderRequest.
struct LimitOrder {
  NewOrderRequest request{};

  [[nodiscard]] constexpr LimitOrder post_only() const noexcept {
    LimitOrder o = *this;
    o.request.type = OrderType::PostOnly;
    o.request.post_only = true;
    return o;
  }
  [[nodiscard]] constexpr LimitOrder reduce_only() const noexcept {
    LimitOrder o = *this;
    o.request.reduce_only = true;
    return o;
  }
  [[nodiscard]] constexpr LimitOrder ioc() const noexcept {
    LimitOrder o = *this;
    o.request.tif = TimeInForce::Ioc;
    return o;
  }
  // user_tag: echoed in the order record. Tags in the QuoteManager's range are rejected
  // (RejectReason::InvalidTag).
  [[nodiscard]] constexpr LimitOrder tag(std::uint32_t user_tag) const noexcept {
    LimitOrder o = *this;
    o.request.user_tag = user_tag;
    return o;
  }
  // NOLINTNEXTLINE(google-explicit-constructor): passes straight to ctx.send().
  constexpr operator NewOrderRequest() const noexcept { return request; }
};

inline constexpr LimitOrder NewOrderRequest::limit(InstrumentId instrument,
                                                   Side side,
                                                   Price price,
                                                   Qty qty) noexcept {
  LimitOrder o;
  o.request.instrument = instrument;
  o.request.side = side;
  o.request.price = price;
  o.request.qty = qty;
  return o;
}

}  // namespace fastmm
