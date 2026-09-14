#pragma once
// FixEncoder: OrderCommand -> FIX 4.4 order entry (satisfies codecs::Encoder).
//
//   New     -> NewOrderSingle(D):        11 ClOrdID, 18 ExecInst=6 (post-only), 55 Symbol,
//                                        54 Side, 60 TransactTime, 38 OrderQty, 40 OrdType,
//                                        44 Price (not for market), 59 TimeInForce
//   Cancel  -> OrderCancelRequest(F):    41 OrigClOrdID, 37 OrderID (when known), 11 ClOrdID,
//                                        55, 54, 60, 38
//   Replace -> OrderCancelReplaceRequest(G): 37, 41, 11, 18, 55, 54, 60, 38, 40, 44, 59
//
// ClOrdID is the engine's client order id encoding (encode_cl_ord_id, "fm" + 12 hex). A cancel
// request needs its own unique ClOrdID in FIX, so it is the order's id followed by "c" and a
// per-encoder counter; OrigClOrdID(41) carries the order's id, which is what the decoder maps
// cancel acks and cancel rejects back to. OrderType::PostOnly is a Limit order with ExecInst 6
// "Participate don't initiate". TimeInForce: GTC 1, IOC 3, FOK 4, Day 0. reduce_only has no FIX 4.4
// field and is not sent.
//
// F and G require Side, OrderQty and (G) OrdType, which OutCancel/OutReplace do not carry: the
// encoder remembers side/type/tif/instrument/qty of every order it encoded in a preallocated
// direct-mapped table keyed by the client order id's sequence (oldest entries are overwritten
// after `order_slots` newer orders). A cancel/replace for an order it does not know fails
// (returns 0, unknown_orders) unless it was seeded with remember().
//
// Header fields (8, 9, 35, 49, 56, 34, 52) come from the FixSession, which owns MsgSeqNum: encode
// the command, then hand the bytes to FixSession::send_app() (send() does both). Sources: OnixS
// FIX 4.4 dictionary NewOrderSingle (msgType_D_68.html), OrderCancelRequest (msgType_F_70.html),
// OrderCancelReplaceRequest (msgType_G_71.html), tags 18, 40, 54, 59.
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/fix/fix_session.hpp"
#include "fastmm/codecs/fix/fix_symbols.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace fastmm::codecs::fix {

struct FixEncoderStats {
  std::uint64_t new_orders = 0;
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
  std::uint64_t failures = 0;
  std::uint64_t unknown_orders = 0;
  std::uint64_t unknown_symbols = 0;
};

class FixEncoder {
 public:
  static constexpr std::size_t kDefaultOrderSlots = 1U << 14;
  static constexpr std::size_t kMaxOrderMessage = 1024;

  // order_slots is rounded up to a power of two.
  FixEncoder(const FixSession& session,
             const FixSymbolTable& symbols,
             std::size_t order_slots = kDefaultOrderSlots);

  // Encoder: full wire message for MsgSeqNum == session.next_sender_seq(). 0 on failure.
  std::size_t encode(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;
  // encode() into an internal buffer, then session.send_app().
  bool send(FixSession& session, const venues::OrderCommand& cmd) noexcept;

  // Seeds what cancel/replace need for an order this encoder did not send.
  void remember(ClientOrderId id,
                InstrumentId instrument,
                Side side,
                OrderType type,
                TimeInForce tif,
                Price price,
                Qty qty) noexcept;

  [[nodiscard]] const FixEncoderStats& stats() const noexcept { return stats_; }

 private:
  struct OrderInfo {
    ClientOrderId id;
    Price price;
    Qty qty;
    InstrumentId instrument;
    Side side;
    OrderType type;
    TimeInForce tif;
    std::uint8_t pad_;
  };
  [[nodiscard]] const OrderInfo* find(ClientOrderId id) const noexcept;
  std::size_t fail(std::uint64_t& counter) noexcept;

  const FixSession* session_;
  const FixSymbolTable* symbols_;
  std::size_t mask_;
  std::unique_ptr<OrderInfo[]> orders_;
  std::unique_ptr<std::byte[]> buf_;
  std::uint64_t cancel_counter_ = 0;
  FixEncoderStats stats_{};
};

}  // namespace fastmm::codecs::fix
