#pragma once
// Record stream: the engine's second outbound queue, next to the journal.
//
//   engine thread --RecordWriter--> MsgRing --store thread--> a storage backend
//
// The journal is the byte-exact stream a replay consumes; this is the interpreted one, the fills,
// orders, positions and kill events a store turns into rows. Both are written the same way: the
// engine copies a trivially copyable record into an SPSC ring and never waits. A full ring drops
// the record and counts it (EngineStats::records_dropped); nothing in the trading path depends on
// a record being consumed, so a slow or broken store cannot stall or kill a session. The journal
// is the authority for anything a drop lost (docs/reference/storage.md).
//
// Records share the MsgRing prefix (4-byte length, 1-byte non-zero type) and are a multiple of 64
// bytes, like the event messages, but they carry their own RecordType space and version: the store
// format is free to change without touching the journal format.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/order.hpp"
#include "fastmm/core/position.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace fastmm {

inline constexpr std::uint8_t kRecordVersion = 1;

// Never 0: the ring reserves type 0 for padding.
enum class RecordType : std::uint8_t {
  Fill = 1,
  Order = 2,
  Position = 3,
  Kill = 4,
  Funding = 5,
  Replayed = 6,
  Param = 7,
  Reject = 8,
};
[[nodiscard]] constexpr std::string_view to_string(RecordType t) noexcept {
  switch (t) {
    case RecordType::Fill:
      return "Fill";
    case RecordType::Order:
      return "Order";
    case RecordType::Position:
      return "Position";
    case RecordType::Kill:
      return "Kill";
    case RecordType::Funding:
      return "Funding";
    case RecordType::Replayed:
      return "Replayed";
    case RecordType::Param:
      return "Param";
    case RecordType::Reject:
      return "Reject";
  }
  return "?";
}

struct RecordHeader {
  enum Flags : std::uint8_t {
    kSynthetic = 1U << 0,  // booked by the engine, not reported by the venue (a cum_qty jump)
    kLate = 1U << 1,       // fill for an order that was already terminal; on an order record,
                           // that fill's update of the order (only cum_qty and state are valid)
    kUnknown = 1U << 2,    // fill for an order id the OMS does not know
    kTerminal = 1U << 3,   // the order reached a terminal state with this record
    kVenue = 1U << 4,      // kill record: one venue's switch, not the global one
    kReplayed = 1U << 5,   // funding record: from the venue's history, not its private stream
  };

  std::uint32_t len;         // total bytes, a multiple of 64            (offset 0)
  RecordType type;           //                                          (4)
  std::uint8_t version;      // kRecordVersion                           (5)
  VenueId venue;             //                                          (6)
  std::uint8_t flags;        //                                          (7)
  InstrumentId instrument;   // invalid when the record is not per instrument (8)
  std::uint8_t aux[4];       // record-specific small fields             (12)
  std::uint64_t seq;         // record order within the session, from 1  (16)
  Timestamp engine_ts;       // the engine clock the record was made at  (24)
  Timestamp wall_ts;         // wall clock at the same point             (32)
  std::uint64_t session_id;  //                                       (40)
  // The venue's time of the event behind the record (a fill: the trade time the venue reported),
  // 0 when there is none. A restart resumes the venue's execution replay from it
  // (store/reader.hpp).
  Timestamp exch_ts;     //                                       (48)
  std::uint8_t pad_[8];  //                                       -> 64
};
static_assert(sizeof(RecordHeader) == 64 && std::is_trivially_copyable_v<RecordHeader>);
static_assert(offsetof(RecordHeader, len) == 0 && offsetof(RecordHeader, type) == 4 &&
              offsetof(RecordHeader, seq) == 16 && offsetof(RecordHeader, exch_ts) == 48);

// One execution. `fee` is the engine's booked fee in the instrument's settlement currency;
// `fee_amount` is what the venue reported, in units of `fee_asset` (a commission in a third asset
// is reported but not booked, so `fee` is then zero).
struct FillRecord {
  RecordHeader hdr;
  ClientOrderId cl_ord_id;      // 64
  Price price;                  // 72
  Qty qty;                      // 80  this execution, as the venue reported it
  Qty booked_qty;               // 88  what the position moved by (base-asset fees deducted)
  Qty cum_qty;                  // 96
  Qty leaves_qty;               // 104
  Notional fee;                 // 112 settlement currency, booked
  Notional fee_amount;          // 120 as reported, in fee_asset units
  Qty position_qty;             // 128 the instrument's position after this fill
  Price position_avg_px;        // 136
  Notional position_realized;   // 144
  Notional position_fees;       // 152
  Side side;                    // 160
  Liquidity liquidity;          // 161
  FeeAsset fee_asset;           // 162
  std::uint8_t pad0_[5];        // 163 -> 168
  VenueOrderId venue_order_id;  // 168 -> 209
  ExecId exec_id;               // 209 -> 250
  std::uint8_t pad_[6];         // -> 256
};
static_assert(sizeof(FillRecord) == 256 && std::is_trivially_copyable_v<FillRecord>);
static_assert(offsetof(FillRecord, venue_order_id) == 168 && offsetof(FillRecord, exec_id) == 209);

// The OMS record after a transition. hdr.aux[0] is the previous OrderState, hdr.aux[1] the
// OmsAction as an integer, and hdr.aux[2] is 1 when this closes the client order id a
// cancel-replace superseded (order.cl_ord_id is that id; every other field belongs to the order
// that carries on, so a store updates only the state). With RecordHeader::kLate the order had
// already ended and a fill that arrived after it changed its cum_qty, and its state when the fill
// completed it (Filled instead of Canceled): a store updates those two.
struct OrderRecord {
  RecordHeader hdr;
  Order order;
};
static_assert(sizeof(OrderRecord) == 192 && std::is_trivially_copyable_v<OrderRecord>);

// One instrument's position and the portfolio totals at the same instant.
struct PositionRecord {
  RecordHeader hdr;
  Position pos;               // 64 -> 128
  Notional total_realized;    // 128
  Notional total_unrealized;  // 136
  Notional total_fees;        // 144
  Notional pnl_carry;         // 152 carried in from earlier sessions (EngineConfig::pnl_carry)
  Notional funding;           // 160 this instrument's funding so far, part of pos.realized
  Notional total_funding;     // 168 over every instrument, part of total_realized
  std::uint8_t pad_[16];      // -> 192
};
static_assert(sizeof(PositionRecord) == 192 && std::is_trivially_copyable_v<PositionRecord>);

// A funding payment the engine booked (FundingMsg): `amount` in the instrument's settlement
// currency, negative paid. hdr.exch_ts is the venue's time of it; hdr.flags kReplayed when it came
// from the venue's history. A position record follows it.
struct FundingRecord {
  RecordHeader hdr;
  Notional amount;             // 64
  Qty position_qty;            // 72  the position it was paid on
  Notional position_realized;  // 80  the instrument's realized after it
  Notional position_funding;   // 88  the instrument's funding after it
  Notional total_funding;      // 96  over every instrument
  ExecId funding_id;           // 104 -> 145
  FixedString<8> asset;        // 145 -> 154
  std::uint8_t pad_[38];       // -> 192
};
static_assert(sizeof(FundingRecord) == 192 && std::is_trivially_copyable_v<FundingRecord>);

// A kill switch trip. hdr.flags & kVenue marks a per-venue trip (hdr.venue names it).
struct KillRecord {
  RecordHeader hdr;
  KillReason reason;          // 64
  std::uint8_t pad0_[3];      // -> 68
  std::uint32_t kill_flags;   // 68  RiskEngine::kill_flags()
  std::uint64_t kills;        // 72  global trips so far
  std::uint64_t venue_kills;  // 80
  Notional realized;          // 88
  Notional unrealized;        // 96
  Notional fees;              // 104
  Notional pnl_carry;         // 112
  std::uint8_t pad_[8];       // -> 128
};
static_assert(sizeof(KillRecord) == 128 && std::is_trivially_copyable_v<KillRecord>);

// A venue's execution replay ended complete (a reconciliation began with
// ReconcileMsg::kExecutionsExact): every execution the venue made before it is in the records
// before this one. hdr.venue is the venue. A restart reads it as "this session got past its replay
// on that venue": the fills of a session without one may have a hole behind them (it died while
// the replay was still reading), and the next replay starts before them (store/reader.hpp).
struct ReplayedRecord {
  RecordHeader hdr;
};
static_assert(sizeof(ReplayedRecord) == 64 && std::is_trivially_copyable_v<ReplayedRecord>);

// A strategy parameter update the engine applied (ParamUpdateMsg): after the header, the message's
// bytes past its own header, so the fields sit at the same offsets. hdr.instrument is the update's
// target, invalid for every instrument. The store names the fields through the session's
// parameter table (SessionOpen::param_table).
struct ParamRecord {
  RecordHeader hdr;
  std::uint32_t count;                              // 64  pairs used
  ParamUpdateMsg::Origin origin;                    // 68
  std::uint8_t pad0_[3];                            //
  std::uint64_t publish_seq;                        // 72
  std::uint16_t field[ParamUpdateMsg::kMaxFields];  // 80  index in the parameter table
  char source[ParamUpdateMsg::kSourceLen];          // 144 NUL-terminated
  std::uint8_t pad1_[8];                            //
  std::int64_t value[ParamUpdateMsg::kMaxFields];   // 192 raw value of each pair

  [[nodiscard]] std::string_view source_view() const noexcept {
    std::size_t n = 0;
    while (n < ParamUpdateMsg::kSourceLen && source[n] != '\0') ++n;
    return {source, n};
  }
};
static_assert(sizeof(ParamRecord) == sizeof(ParamUpdateMsg) &&
              std::is_trivially_copyable_v<ParamRecord>);
static_assert(offsetof(ParamRecord, count) == offsetof(ParamUpdateMsg, count) &&
              offsetof(ParamRecord, origin) == offsetof(ParamUpdateMsg, origin) &&
              offsetof(ParamRecord, publish_seq) == offsetof(ParamUpdateMsg, publish_seq) &&
              offsetof(ParamRecord, field) == offsetof(ParamUpdateMsg, field) &&
              offsetof(ParamRecord, source) == offsetof(ParamUpdateMsg, source) &&
              offsetof(ParamRecord, value) == offsetof(ParamUpdateMsg, value));

// What refused an order (RejectRecord::source): the limit behind a rate-limit refusal, or the
// side that refused it otherwise. Rate limits stay apart by where they are counted: the engine's
// [risk] bucket, the engine's view of an account's order windows (ctx.order_budget, pool routing),
// the connector's own limiter (never sent), and the venue's answer.
enum class RejectSource : std::uint8_t {
  Risk = 0,              // a pre-trade check other than the rate limits (`reason` says which)
  RiskBucket = 1,        // the [risk] orders_per_sec token bucket, shared by every account
  AccountPaused = 2,     // the account's connector is paused (429 Retry-After, 418)
  AccountOrders10s = 3,  // the account's order window at the connector's cap
  AccountOrders1m = 4,
  AccountOrders1d = 5,
  VenueLocal = 6,  // the connector's own limiter refused it before sending (OrderReject text)
  Venue = 7,       // the venue refused it (venue_code, text)
};
[[nodiscard]] constexpr std::string_view to_string(RejectSource s) noexcept {
  switch (s) {
    case RejectSource::Risk:
      return "risk";
    case RejectSource::RiskBucket:
      return "risk_bucket";
    case RejectSource::AccountPaused:
      return "account_paused";
    case RejectSource::AccountOrders10s:
      return "account_orders_10s";
    case RejectSource::AccountOrders1m:
      return "account_orders_1m";
    case RejectSource::AccountOrders1d:
      return "account_orders_1d";
    case RejectSource::VenueLocal:
      return "venue_local";
    case RejectSource::Venue:
      return "venue";
  }
  return "?";
}

// One order refused: by the pre-trade checks (the engine's) or by its venue (an OrderReject),
// with the account's order budget as the engine saw it then (ctx.order_budget). hdr.venue is the
// account the order went to or was routed to, hdr.instrument its instrument. Refusals of the
// same instrument, side, account, reason, source and flags within RejectRecord::kFoldNs of the
// last one recorded for them are counted into the next record's `folded` rather than recorded one
// by one: the counts stay exact per account, a strategy retrying on every event does not fill the
// store.
struct RejectRecord {
  static constexpr std::int64_t kFoldNs = 100'000'000;
  enum Flags : std::uint8_t {
    kReplace = 1U << 0,  // a replace, not a new order
    kReduces = 1U << 1,  // with the open orders on its side, it only takes the position to zero
    kReduceOnly = 1U << 2,
    kFromVenue = 1U << 3,  // an OrderReject, not a pre-trade check
  };
  RecordHeader hdr;
  RejectReason reason;         // 64
  RejectSource source;         // 65
  Side side;                   // 66
  std::uint8_t flags;          // 67
  std::uint32_t folded;        // 68  refusals of the same key folded in since the last record
  Price price;                 // 72
  Qty qty;                     // 80
  ClientOrderId cl_ord_id;     // 88  the venue's refusals; 0 for a pre-trade one
  std::int64_t local_tokens;   // 96  [risk] bucket tokens left (OrderBudget::kUnlimited: off)
  std::int64_t local_wait_ns;  // 104
  // The account's windows: used and what the connector admits (RateWindow::admits; 0 unknown).
  std::int64_t orders_10s_used;    // 112
  std::int64_t orders_10s_admits;  // 120
  std::int64_t orders_1m_used;     // 128
  std::int64_t orders_1m_admits;   // 136
  std::int64_t orders_1d_used;     // 144
  std::int64_t orders_1d_admits;   // 152
  std::int64_t weight_used;        // 160
  std::int64_t weight_admits;      // 168
  std::int32_t venue_code;         // 176 the venue's error code (OrderRejectMsg::venue_code)
  std::uint8_t budget_known;       // 180 the account's connector has published a budget
  std::uint8_t paused;             // 181
  std::uint8_t pad0_[2];           //
  FixedString<40> text;            // 184 the venue's text (OrderRejectMsg::text)
  std::uint8_t pad_[31];           // -> 256
};
static_assert(sizeof(RejectRecord) == 256 && std::is_trivially_copyable_v<RejectRecord>);

// Engine-side producer. Every method is allocation-free, wait-free and safe to call from the
// trading thread; a full ring increments dropped() and returns false.
class RecordWriter {
 public:
  explicit RecordWriter(MsgRing* ring = nullptr, std::uint64_t session_id = 0) noexcept
      : ring_(ring), session_id_(session_id) {}

  [[nodiscard]] bool enabled() const noexcept { return ring_ != nullptr; }
  [[nodiscard]] std::uint64_t written() const noexcept { return written_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

  [[nodiscard]] FASTMM_FORCE_INLINE bool put(const RecordHeader& r) noexcept {
    if (ring_ == nullptr) return true;  // disabled: callers guard with enabled()
    std::byte* p = ring_->try_reserve(r.len);
    if (FASTMM_UNLIKELY(p == nullptr)) {
      ++dropped_;
      return false;
    }
    std::memcpy(p, &r, r.len);
    auto* h = reinterpret_cast<RecordHeader*>(p);
    h->version = kRecordVersion;
    h->seq = ++written_;
    h->session_id = session_id_;
    ring_->commit();
    return true;
  }

  // Fills in the parts every record shares. The caller sets the record-specific fields and calls
  // put(). `len` is sizeof the record.
  template <class R>
  FASTMM_FORCE_INLINE void init(R& r,
                                RecordType type,
                                InstrumentId inst,
                                VenueId venue,
                                Timestamp engine_ts,
                                Timestamp wall_ts) noexcept {
    static_assert(std::is_trivially_copyable_v<R> && sizeof(R) % 64 == 0);
    r.hdr = RecordHeader{};
    r.hdr.len = static_cast<std::uint32_t>(sizeof(R));
    r.hdr.type = type;
    r.hdr.version = kRecordVersion;
    r.hdr.instrument = inst;
    r.hdr.venue = venue;
    r.hdr.engine_ts = engine_ts;
    r.hdr.wall_ts = wall_ts;
  }

 private:
  MsgRing* ring_;
  std::uint64_t session_id_;
  std::uint64_t written_ = 0;
  std::uint64_t dropped_ = 0;
};

}  // namespace fastmm
