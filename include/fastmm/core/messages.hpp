#pragma once
// Event bus message structs (5.5). Every message:
//   * starts with a 64-byte EventHeader whose first 4 bytes are the total length and 5th
//     byte the EventType (MsgRing protocol),
//   * is trivially copyable with sizeof a multiple of 64 (explicit padding),
//   * carries a `version` so journals remain readable across layout changes.
//
// Messages are constructed in place inside a ring/journal buffer; use init_header().
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace fastmm {

inline constexpr std::uint8_t kMessageVersion = 1;

// One price level. Shared by book messages and the L2 book.
struct Level {
  Price price;
  Qty qty;
  constexpr bool operator==(const Level&) const noexcept = default;
};
static_assert(sizeof(Level) == 16 && std::is_trivially_copyable_v<Level>);

struct EventHeader {
  enum Flags : std::uint8_t {
    kSynthetic = 1U << 0,  // generated locally (reconcile fill, sim), not from the venue
    kReplayed = 1U << 1,   // came from a journal
    kSnapshot = 1U << 2,   // BookDelta carries a full snapshot
    kOutbound = 1U << 3,   // engine -> venue message copied into the journal
  };

  std::uint32_t len;     // total bytes incl. header, multiple of 64   (offset 0)
  EventType type;        // (offset 4)
  std::uint8_t version;  // kMessageVersion
  VenueId venue;
  std::uint8_t flags;
  InstrumentId instrument;  // dense id, invalid for non-instrument events (offset 8)
  std::uint32_t reserved0;  // (offset 12)
  std::uint64_t seq;        // engine consumption order; assigned by JournalWriter (16)
  std::uint64_t venue_seq;  // venue update id / sequence number (24)
  Timestamp exch_ts;        // venue event time (32)
  Timestamp recv_ts;        // T0 wall-clock (40)
  Cycles t0_cycles;         // T0 raw TSC at recv (48)
  std::uint32_t t1_delta;   // cycles T0->T1 (decoded)  (56)
  std::uint32_t t2_delta;   // cycles T0->T2 (book applied) (60)
};
static_assert(sizeof(EventHeader) == 64);
static_assert(offsetof(EventHeader, len) == 0 && offsetof(EventHeader, type) == 4);
static_assert(std::is_trivially_copyable_v<EventHeader>);

// For fixed-size messages len defaults to sizeof(M); BookDeltaMsg passes size_for().
template <class M>
constexpr void init_header(M& m,
                           EventType type,
                           InstrumentId inst = {},
                           VenueId venue = {},
                           std::uint32_t len = static_cast<std::uint32_t>(sizeof(M))) noexcept {
  static_assert(std::is_trivially_copyable_v<M>);
  FASTMM_ASSERT(len % 64 == 0 && len >= sizeof(EventHeader));
  m.hdr = EventHeader{};
  m.hdr.len = len;
  m.hdr.type = type;
  m.hdr.version = kMessageVersion;
  m.hdr.venue = venue;
  m.hdr.instrument = inst;
}

// ---- market data ---------------------------------------------------------------------

// Bids then asks follow the fixed part as a flexible array of Level (bid_count + ask_count).
// Used both for deltas (qty == 0 means delete) and full snapshots (flag kSnapshot).
//
// Never construct a BookDeltaMsg by value, not even with zero levels. The fixed part is 96
// bytes but hdr.len is rounded to the 64-byte message granule, so size_for(0, 0) == 128.
// Everything that forwards a message copies hdr.len bytes (MsgRing, journal, BookSyncer), so a
// stack or member BookDeltaMsg is read 32 bytes past its end. Build it in place instead:
//   alignas(64) std::byte buf[BookDeltaMsg::size_for(bids, asks)];   or   ring.try_reserve(len)
struct BookDeltaMsg {
  EventHeader hdr;
  std::uint32_t bid_count;
  std::uint32_t ask_count;
  std::uint64_t first_update_id;  // Binance U
  std::uint64_t last_update_id;   // Binance u / Bybit u
  std::uint64_t prev_update_id;   // Binance futures pu (0 if n/a)

  [[nodiscard]] static constexpr std::uint32_t size_for(std::uint32_t bids,
                                                        std::uint32_t asks) noexcept {
    const std::uint32_t raw =
        static_cast<std::uint32_t>(sizeof(BookDeltaMsg)) + (bids + asks) * 16U;
    return (raw + 63U) & ~63U;
  }
  [[nodiscard]] Level* levels() noexcept {
    return reinterpret_cast<Level*>(reinterpret_cast<std::byte*>(this) + sizeof(BookDeltaMsg));
  }
  [[nodiscard]] const Level* levels() const noexcept {
    return reinterpret_cast<const Level*>(reinterpret_cast<const std::byte*>(this) +
                                          sizeof(BookDeltaMsg));
  }
  [[nodiscard]] std::span<const Level> bids() const noexcept { return {levels(), bid_count}; }
  [[nodiscard]] std::span<const Level> asks() const noexcept {
    return {levels() + bid_count, ask_count};
  }
  [[nodiscard]] std::span<Level> bids() noexcept { return {levels(), bid_count}; }
  [[nodiscard]] std::span<Level> asks() noexcept { return {levels() + bid_count, ask_count}; }
  [[nodiscard]] bool is_snapshot() const noexcept {
    return (hdr.flags & EventHeader::kSnapshot) != 0;
  }
};
static_assert(sizeof(BookDeltaMsg) == 96 && alignof(BookDeltaMsg) == 8);
using BookSnapshotMsg = BookDeltaMsg;  // same layout; hdr.type == BookSnapshot, kSnapshot set

struct TradeMsg {
  EventHeader hdr;
  Price price;
  Qty qty;
  std::uint64_t trade_id;
  Side aggressor;
  std::uint8_t pad_[39];
};
static_assert(sizeof(TradeMsg) == 128);

struct BookTickerMsg {
  EventHeader hdr;
  Price bid_px;
  Qty bid_qty;
  Price ask_px;
  Qty ask_qty;
  std::uint8_t pad_[32];
};
static_assert(sizeof(BookTickerMsg) == 128);

// ---- order events (venue -> engine) ----------------------------------------------------

struct OrderAckMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  VenueOrderId venue_order_id;
  std::uint8_t pad_[15];
};
static_assert(sizeof(OrderAckMsg) == 128);

struct OrderRejectMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  RejectReason reason;
  std::uint8_t pad0_[3];
  std::int32_t venue_code;
  FixedString<40> text;
  std::uint8_t pad_[7];
};
static_assert(sizeof(OrderRejectMsg) == 128);

struct OrderCancelAckMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  VenueOrderId venue_order_id;
  std::uint8_t pad0_[7];
  Qty cum_qty;  // filled before the cancel took effect
};
static_assert(sizeof(OrderCancelAckMsg) == 128);

struct OrderCancelRejectMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  RejectReason reason;  // VenueUnknownOrder => the venue has no such order
  std::uint8_t pad0_[3];
  std::int32_t venue_code;
  FixedString<40> text;
  std::uint8_t pad_[7];
};
static_assert(sizeof(OrderCancelRejectMsg) == 128);

struct OrderFillMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  VenueOrderId venue_order_id;
  ExecId exec_id;  // dedupe key
  std::uint8_t pad0_[6];
  Price price;
  Qty qty;      // this execution
  Qty cum_qty;  // cumulative after this execution
  Qty leaves_qty;
  Notional fee;  // >= 0 paid, < 0 rebate
  Side side;
  Liquidity liquidity;
  std::uint8_t pad_[54];
};
static_assert(sizeof(OrderFillMsg) == 256);

struct OrderExpiredMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  VenueOrderId venue_order_id;
  std::uint8_t pad0_[7];
  Qty cum_qty;
};
static_assert(sizeof(OrderExpiredMsg) == 128);

struct PositionUpdateMsg {
  EventHeader hdr;
  Qty qty;  // signed net position
  Price avg_px;
  Notional realized;
  Notional unrealized;
  Notional fees;
  std::uint8_t pad_[24];
};
static_assert(sizeof(PositionUpdateMsg) == 128);

// ---- engine-internal -------------------------------------------------------------------

struct TimerMsg {
  EventHeader hdr;
  TimerId timer_id;
  std::uint32_t pad0_;
  std::uint64_t user_data;
  Timestamp fire_ts;
  std::uint8_t pad_[40];
};
static_assert(sizeof(TimerMsg) == 128);

struct ControlMsg {
  EventHeader hdr;
  ControlCommand command;
  std::uint8_t pad0_[7];
  std::uint64_t arg;
  std::uint8_t pad_[48];
};
static_assert(sizeof(ControlMsg) == 128);

struct ConnectionStateMsg {
  EventHeader hdr;
  ConnState state;
  std::uint8_t channel;  // 0 = market data, 1 = order/user stream
  std::uint8_t pad0_[2];
  std::int32_t reason_code;
  std::uint8_t pad_[56];
};
static_assert(sizeof(ConnectionStateMsg) == 128);

// Control thread injects the venue's view after a reconnect (5.8). A reconciliation is a
// Begin, N OpenOrder / Position records, then End.
struct ReconcileMsg {
  enum class Kind : std::uint8_t { Begin = 0, OpenOrder = 1, Position = 2, End = 3 };
  EventHeader hdr;
  Kind kind;
  Side side;
  OrderState state;
  std::uint8_t pad0_[5];
  ClientOrderId cl_ord_id;
  VenueOrderId venue_order_id;
  std::uint8_t pad1_[7];
  Price price;
  Qty orig_qty;
  Qty cum_qty;
  Qty position_qty;  // Kind::Position
  Price avg_px;      // Kind::Position
  std::uint8_t pad_[24];
};
static_assert(sizeof(ReconcileMsg) == 192);

struct LatencySampleMsg {
  EventHeader hdr;
  std::uint8_t interval;  // LatencyInterval index
  std::uint8_t pad0_[7];
  std::uint64_t count;
  std::int64_t p50_ns;
  std::int64_t p90_ns;
  std::int64_t p99_ns;
  std::int64_t p999_ns;
  std::int64_t max_ns;
  std::uint8_t pad_[8];
};
static_assert(sizeof(LatencySampleMsg) == 128);

// ---- outbound (engine -> venue) --------------------------------------------------------

struct OutNewOrderMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  Price price;
  Qty qty;
  Side side;
  OrderType type;
  TimeInForce tif;
  std::uint8_t reduce_only;
  std::uint8_t pad_[36];
};
static_assert(sizeof(OutNewOrderMsg) == 128);

struct OutCancelMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  VenueOrderId venue_order_id;  // may be empty if not yet acked
  std::uint8_t pad_[15];
};
static_assert(sizeof(OutCancelMsg) == 128);

struct OutReplaceMsg {
  EventHeader hdr;
  ClientOrderId cl_ord_id;       // new id
  ClientOrderId orig_cl_ord_id;  // order being replaced
  VenueOrderId venue_order_id;
  std::uint8_t pad0_[7];
  Price price;
  Qty qty;
  std::uint8_t pad_[48];
};
static_assert(sizeof(OutReplaceMsg) == 192);

// ---- L3 (ITCH-style) -------------------------------------------------------------------

struct OrderAddL3Msg {
  EventHeader hdr;
  std::uint64_t order_ref;
  Price price;
  Qty qty;
  Side side;
  std::uint8_t pad_[39];
};
static_assert(sizeof(OrderAddL3Msg) == 128);

struct OrderExecL3Msg {
  EventHeader hdr;
  std::uint64_t order_ref;
  Qty exec_qty;
  Price exec_price;  // zero -> at the order's price
  std::uint64_t match_id;
  std::uint8_t pad_[32];
};
static_assert(sizeof(OrderExecL3Msg) == 128);

struct OrderCancelL3Msg {
  EventHeader hdr;
  std::uint64_t order_ref;
  Qty canceled_qty;  // zero -> delete the whole order
  std::uint8_t pad_[48];
};
static_assert(sizeof(OrderCancelL3Msg) == 128);

struct OrderReplaceL3Msg {
  EventHeader hdr;
  std::uint64_t old_order_ref;
  std::uint64_t new_order_ref;
  Price price;
  Qty qty;
  std::uint8_t pad_[32];
};
static_assert(sizeof(OrderReplaceL3Msg) == 128);

// Largest fixed-size message; BookDelta is bounded separately by kMaxBookLevelsPerMsg.
inline constexpr std::uint32_t kMaxBookLevelsPerMsg = 1024;
inline constexpr std::uint32_t kMaxMsgBytes =
    BookDeltaMsg::size_for(kMaxBookLevelsPerMsg, kMaxBookLevelsPerMsg);

// Every message: trivially copyable with the EventHeader first. Fixed-size messages are
// additionally a multiple of 64 bytes (BookDeltaMsg is variable-length: see size_for()).
template <class M>
concept MessageLike = std::is_trivially_copyable_v<M> &&
                      std::is_same_v<decltype(M::hdr), EventHeader> && alignof(M) <= 64;
template <class M>
concept FixedSizeMessage = MessageLike<M> && (sizeof(M) % 64 == 0);
static_assert(MessageLike<BookDeltaMsg> && FixedSizeMessage<OrderFillMsg> &&
              FixedSizeMessage<OutReplaceMsg>);
static_assert(FixedSizeMessage<TradeMsg> && FixedSizeMessage<BookTickerMsg> &&
              FixedSizeMessage<OrderAckMsg> && FixedSizeMessage<OrderRejectMsg> &&
              FixedSizeMessage<OrderCancelAckMsg> && FixedSizeMessage<OrderCancelRejectMsg> &&
              FixedSizeMessage<OrderExpiredMsg> && FixedSizeMessage<PositionUpdateMsg> &&
              FixedSizeMessage<TimerMsg> && FixedSizeMessage<ControlMsg> &&
              FixedSizeMessage<ConnectionStateMsg> && FixedSizeMessage<ReconcileMsg> &&
              FixedSizeMessage<LatencySampleMsg> && FixedSizeMessage<OutNewOrderMsg> &&
              FixedSizeMessage<OutCancelMsg> && FixedSizeMessage<OrderAddL3Msg> &&
              FixedSizeMessage<OrderExecL3Msg> && FixedSizeMessage<OrderCancelL3Msg> &&
              FixedSizeMessage<OrderReplaceL3Msg>);

template <MessageLike M>
[[nodiscard]] FASTMM_FORCE_INLINE const M& msg_cast(const EventHeader* h) noexcept {
  return *reinterpret_cast<const M*>(h);
}

}  // namespace fastmm
