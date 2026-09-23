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
  }
  return "?";
}

struct RecordHeader {
  enum Flags : std::uint8_t {
    kSynthetic = 1U << 0,  // booked by the engine, not reported by the venue (a cum_qty jump)
    kLate = 1U << 1,       // fill for an order that was already terminal
    kUnknown = 1U << 2,    // fill for an order id the OMS does not know
    kTerminal = 1U << 3,   // the order reached a terminal state with this record
    kVenue = 1U << 4,      // kill record: one venue's switch, not the global one
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
  std::uint8_t pad_[16];     //                                       -> 64
};
static_assert(sizeof(RecordHeader) == 64 && std::is_trivially_copyable_v<RecordHeader>);
static_assert(offsetof(RecordHeader, len) == 0 && offsetof(RecordHeader, type) == 4 &&
              offsetof(RecordHeader, seq) == 16);

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
// that carries on, so a store updates only the state).
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
  std::uint8_t pad_[32];      // -> 192
};
static_assert(sizeof(PositionRecord) == 192 && std::is_trivially_copyable_v<PositionRecord>);

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
