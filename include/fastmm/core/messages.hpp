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
#include "fastmm/core/risk_limits.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
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
    // Journal only (format v2, ADR-0010): reserved0 holds the engine clock at consumption as a
    // signed ns delta from the previous kEngineTime record or EngineTimeMsg.
    kEngineTime = 1U << 4,
    kDropped = 1U << 5,  // journal only: outbound message the transport did not accept
  };

  std::uint32_t len;     // total bytes incl. header, multiple of 64   (offset 0)
  EventType type;        // (offset 4)
  std::uint8_t version;  // kMessageVersion
  VenueId venue;
  std::uint8_t flags;
  InstrumentId instrument;  // dense id, invalid for non-instrument events (offset 8)
  std::uint32_t reserved0;  // journal: engine-time delta when kEngineTime (offset 12)
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

// Options ticker (EventType::OptionTicker): the venue's mark, implied volatilities and greeks for
// one option instrument. mark_price is in the instrument's price unit (the base coin for Deribit
// inverse options); underlying_price (the forward the venue prices the option on) and
// index_price are in the underlying's quote currency. Implied vols are annualised decimals
// (0.312 = 31.2 %). Greeks are copied as the venue reports them (Deribit: Black-76 delta, gamma
// per quote unit, vega and rho per 1 vol / rate point, theta per day, in the quote currency; see
// core/options/black76.hpp). NaN marks a field the venue did not send.
struct OptionTickerMsg {
  EventHeader hdr;
  Price mark_price;        // 64
  Price underlying_price;  // 72
  Price index_price;       // 80
  double mark_iv;          // 88
  double bid_iv;           // 96
  double ask_iv;           // 104
  double delta;            // 112
  double gamma;            // 120
  double vega;             // 128
  double theta;            // 136
  double rho;              // 144
  double interest_rate;    // 152  annualised decimal
  std::uint8_t pad_[32];   // 160
};
static_assert(sizeof(OptionTickerMsg) == 192);
static_assert(offsetof(OptionTickerMsg, mark_iv) == 88 &&
              offsetof(OptionTickerMsg, interest_rate) == 152);

// ---- order events (venue -> engine) ----------------------------------------------------

struct OrderAckMsg {
  // The venue amended the resting order in place (Binance Spot order.amend.keepPriority,
  // execution type REPLACED): same venue order, same queue position, fills so far kept. Without
  // it an ack under a new client id means a new venue order whose cumulative quantity is zero.
  static constexpr std::uint8_t kAmendedInPlace = 1U << 0;

  EventHeader hdr;
  ClientOrderId cl_ord_id;
  VenueOrderId venue_order_id;
  std::uint8_t flags;
  std::uint8_t pad_[14];
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
  // The execution comes from the venue's trade history (Venue::request_executions), not from the
  // private stream: it may already have been booked, so cum_qty and leaves_qty are not reported and
  // the OMS works out how much of it is new (Oms::on_fill, OmsUpdate::corrected_qty).
  static constexpr std::uint8_t kReplayed = 1U << 0;
  EventHeader hdr;
  ClientOrderId cl_ord_id;
  VenueOrderId venue_order_id;
  ExecId exec_id;  // dedupe key
  std::uint8_t pad0_[6];
  Price price;
  Qty qty;      // this execution
  Qty cum_qty;  // cumulative after this execution (0 with kReplayed: the venue did not say)
  Qty leaves_qty;
  Notional fee;  // >= 0 paid, < 0 rebate; in units of fee_asset
  Side side;
  Liquidity liquidity;
  FeeAsset fee_asset;  // Quote: `fee` is a quote amount; Base: base units; Other: not convertible
  std::uint8_t flags;  // kReplayed
  std::uint8_t pad_[52];
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

// A perpetual funding payment the venue booked on the account's position in hdr.instrument
// (EventType::Funding). `amount` is signed, in `asset` (the settlement currency): negative paid,
// positive received. hdr.exch_ts is the venue's time of the payment. `funding_id` is the venue's
// id of it (Binance tranId, Bybit execId); the engine and the gateway book one id per instrument
// once, so the private stream and a replay from the venue's history may both deliver it.
struct FundingMsg {
  // From the venue's history (Venue::request_executions), not the private stream.
  static constexpr std::uint8_t kReplayed = 1U << 0;
  EventHeader hdr;
  Notional amount;       // 64
  ExecId funding_id;     // 72 -> 113
  FixedString<8> asset;  // 113 -> 122
  std::uint8_t flags;    // 122 kReplayed
  std::uint8_t pad_[5];  // -> 128
};
static_assert(sizeof(FundingMsg) == 128 && offsetof(FundingMsg, funding_id) == 72 &&
              offsetof(FundingMsg, flags) == 122);
// A funding id in the list of venue ids a restarted session's store already holds
// (store::Recovery::VenueResume::known_exec_ids, next to the trade ids): the prefix keeps the two
// id spaces apart.
inline constexpr std::string_view kFundingIdPrefix = "funding:";

// ---- engine-internal -------------------------------------------------------------------

struct TimerMsg {
  EventHeader hdr;
  TimerId timer_id;
  std::uint8_t engine;  // 1: the engine's own timer (max_param_age), not the strategy's
  std::uint8_t pad0_[3];
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

// ControlCommand::SetLimits: a ControlMsg with the new limits after it. The prefix is a ControlMsg
// (same type, same offsets), so the engine's dispatch reads `command` from either and only this
// command looks past it; hdr.len tells the rings, the journal and a replay how long the record is.
struct ControlLimitsMsg {
  EventHeader hdr;
  ControlCommand command;
  std::uint8_t pad0_[7];
  std::uint64_t arg;
  RiskLimits limits;
  std::uint8_t pad_[16];
};
static_assert(sizeof(ControlLimitsMsg) == 192 && std::is_trivially_copyable_v<ControlLimitsMsg>);
static_assert(offsetof(ControlLimitsMsg, command) == offsetof(ControlMsg, command) &&
              offsetof(ControlLimitsMsg, arg) == offsetof(ControlMsg, arg));

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
//
// Begin with kSentWatermark set carries the highest client order id (New or Replace) the venue had
// taken from the outbound ring when it requested the snapshot. Orders with a higher id were not yet
// sent, so the snapshot cannot contain them and End must not mark them cancelled. Without the flag
// every order of the venue missing from the snapshot is cancelled.
struct ReconcileMsg {
  enum class Kind : std::uint8_t { Begin = 0, OpenOrder = 1, Position = 2, End = 3 };
  static constexpr std::uint8_t kSentWatermark = 1U << 0;
  // Begin: every execution the venue made since the last one the engine booked was replayed before
  // this snapshot (Venue::request_executions succeeded), so the snapshot's quantities are the
  // venue's own and nothing in it has to be guessed at. Without it the connector could not ask, and
  // a quantity the snapshot reports that no fill covered is an estimate: see
  // Engine::book_missed_fill and Oms::reconcile_end.
  static constexpr std::uint8_t kExecutionsExact = 1U << 1;
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
  Qty position_qty;              // Kind::Position
  Price avg_px;                  // Kind::Position
  ClientOrderId sent_watermark;  // Kind::Begin, valid with kSentWatermark
  std::uint8_t flags;            // Kind::Begin: kSentWatermark
  std::uint8_t pad_[15];
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

// Journal only (format v2): the engine clock as an absolute value. Written when the engine starts
// and finishes, and before a consumed event whose clock delta does not fit kEngineTime's int32 ns.
// Never dispatched.
struct EngineTimeMsg {
  enum class Kind : std::uint8_t { Sync = 0, Start = 1, Finish = 2 };
  EventHeader hdr;
  Timestamp engine_ts;
  Kind kind;
  std::uint8_t pad_[55];
};
static_assert(sizeof(EngineTimeMsg) == 128);

// New strategy parameter values (ADR-0013): up to kMaxFields (field index, raw value) pairs, built
// and validated off the engine thread (strategies/param_publisher.hpp). The engine applies them
// together at one event, journals the message and calls on_params. A field index is the position
// of the parameter in the strategy's schema; the journal header records the schema, so a replay
// resolves indices by name. A raw value is the field's int64 form (ParamDesc::get_raw): the value
// of an integer or bool, the raw fixed-point value of Price, Qty, Notional and Ratio, the
// nanoseconds of a Duration and the IEEE-754 bits of a double.
struct ParamUpdateMsg {
  static constexpr std::size_t kMaxFields = 32;
  // hdr.instrument of an update for every instrument: the invalid id.
  static constexpr InstrumentId kAllInstruments{};

  EventHeader hdr;
  std::uint32_t count;              // pairs used
  std::uint32_t pad0_;              //
  std::uint64_t publish_seq;        // the publisher's sequence number, from 1
  std::uint16_t field[kMaxFields];  // schema index of each pair
  std::uint8_t pad1_[48];           //
  std::int64_t value[kMaxFields];   // raw value of each pair

  [[nodiscard]] bool all_instruments() const noexcept { return !hdr.instrument.valid(); }
};
static_assert(sizeof(ParamUpdateMsg) == 448 && std::is_trivially_copyable_v<ParamUpdateMsg>);
static_assert(offsetof(ParamUpdateMsg, count) == 64 && offsetof(ParamUpdateMsg, field) == 80 &&
              offsetof(ParamUpdateMsg, value) == 192);

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
  // exec_flags. Zero (E, and records written before the field existed) is a printable execution.
  static constexpr std::uint8_t kNonPrintable = 1U << 0;  // ITCH C with Printable = N

  EventHeader hdr;
  std::uint64_t order_ref;
  Qty exec_qty;
  Price exec_price;  // zero -> at the order's price
  std::uint64_t match_id;
  std::uint8_t exec_flags;
  std::uint8_t pad_[31];

  [[nodiscard]] bool printable() const noexcept { return (exec_flags & kNonPrintable) == 0; }
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
              FixedSizeMessage<OptionTickerMsg> && FixedSizeMessage<OrderAckMsg> &&
              FixedSizeMessage<OrderRejectMsg> && FixedSizeMessage<OrderCancelAckMsg> &&
              FixedSizeMessage<OrderCancelRejectMsg> && FixedSizeMessage<OrderExpiredMsg> &&
              FixedSizeMessage<PositionUpdateMsg> && FixedSizeMessage<FundingMsg> &&
              FixedSizeMessage<TimerMsg> && FixedSizeMessage<ControlMsg> &&
              FixedSizeMessage<ConnectionStateMsg> && FixedSizeMessage<ReconcileMsg> &&
              FixedSizeMessage<LatencySampleMsg> && FixedSizeMessage<EngineTimeMsg> &&
              FixedSizeMessage<OutNewOrderMsg> && FixedSizeMessage<OutCancelMsg> &&
              FixedSizeMessage<OrderAddL3Msg> && FixedSizeMessage<OrderExecL3Msg> &&
              FixedSizeMessage<OrderCancelL3Msg> && FixedSizeMessage<OrderReplaceL3Msg>);

template <MessageLike M>
[[nodiscard]] FASTMM_FORCE_INLINE const M& msg_cast(const EventHeader* h) noexcept {
  return *reinterpret_cast<const M*>(h);
}

}  // namespace fastmm
