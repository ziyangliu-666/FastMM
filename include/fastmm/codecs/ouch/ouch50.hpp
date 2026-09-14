#pragma once
// Nasdaq OUCH 5.0 (fastmm::codecs::ouch50).
//
// Source: "OUCH 5.0 Order Entry Specification", Nasdaq North American Trading Services, updated
// October 2025 (Ouch5.0.pdf). Numerics are big-endian binary; alpha fields left-justified and
// space padded; prices are 8-byte fields with 4 implied decimals; timestamps 8-byte nanoseconds
// since midnight. Most messages end with Appendage Length(2) and an options appendage of
// TagValue elements (Length(1) = remaining bytes | OptionTag(1) | value). Some outbound types
// (C D B J P I T M Q) may omit Appendage Length altogether; the decoder accepts both forms.
//
// Identifiers. OUCH 5.0 identifies a transaction by UserRefNum: a 4-byte unsigned number that
// must be day-unique and strictly increasing per port (new orders with a lower number than the
// last processed are ignored as retransmissions). It is not derived from ClientOrderId:
//
//   UserRefMap   assigns UserRefNums from a counter (first value configurable, and resettable
//                from an Account Query Response's NextUserRefNum) and records
//                ClientOrderId <-> UserRefNum both ways in fixed-capacity tables.
//   OuchEncoder  New -> assign(cl_ord_id); Replace -> find(orig) as OrigUserRefNum and
//                assign(cl_ord_id) as UserRefNum; Cancel -> find(cl_ord_id). The 14-byte ClOrdID
//                field always carries encode_cl_ord_id(cl_ord_id), which Nasdaq echoes in
//                Accepted, Replaced, Rejected and Broken Trade.
//   OuchDecoder  resolves UserRefNum through the shared map, falling back to the echoed ClOrdID
//                when the map has no entry (e.g. after a restart), and erases both directions
//                once the order is done (filled, canceled, rejected, replaced away).
//
// OuchEncoder (satisfies Encoder), OrderCommand ->
//   New      Enter Order 'O' (47 + appendage)   Replace  Replace Order 'U' (40 + appendage)
//   Cancel   Cancel Order 'X' (11, Quantity 0)
//   Time In Force: DAY and GTC -> '0' (Day; OUCH 5.0 has no good-till-cancel), IOC -> '3',
//   FOK -> '3' with the MinQty option = Quantity. PostOnly -> PostOnly option 'P'. A non-blank
//   EncoderConfig::firm adds the Firm option. Market orders are refused (no continuous-market
//   form). Replace sends time_in_force / display from EncoderConfig.
//
// OuchDecoder (satisfies Decoder): the same event mapping as the OUCH 4.2 decoder (see
// ouch42.hpp); 'J' Rejected carries a 2-byte numeric reason (venue_code), 'Q' Account Query
// Response updates UserRefMap::set_next(), 'R' 'X' 'G' 'K' 'T' 'P' 'B' 'S' produce no event.
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/ouch/ouch_common.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fastmm::codecs::ouch50 {

using venues::ParseStatus;

#pragma pack(push, 1)

// ---- inbound, section 2 ----------------------------------------------------------------

struct EnterOrder {  // 'O' 2.1, appendage follows
  char type;
  be32_t user_ref_num;
  char side;
  be32_t quantity;
  char symbol[8];
  be64_t price;
  char time_in_force;
  char display;
  char capacity;
  char intermarket_sweep_eligibility;
  char cross_type;
  char cl_ord_id[14];
  be16_t appendage_length;
};

struct ReplaceOrder {  // 'U' 2.2
  char type;
  be32_t orig_user_ref_num;
  be32_t user_ref_num;
  be32_t quantity;
  be64_t price;
  char time_in_force;
  char display;
  char intermarket_sweep_eligibility;
  char cl_ord_id[14];
  be16_t appendage_length;
};

struct CancelOrder {  // 'X' 2.3
  char type;
  be32_t user_ref_num;
  be32_t quantity;
  be16_t appendage_length;
};

struct ModifyOrder {  // 'M' 2.4
  char type;
  be32_t user_ref_num;
  char side;
  be32_t quantity;
  be16_t appendage_length;
};

struct MassCancel {  // 'C' 2.5
  char type;
  be32_t user_ref_num;
  char firm[4];
  char symbol[8];
  be16_t appendage_length;
};

struct OrderEntryControl {  // 'D' Disable 2.6 / 'E' Enable 2.7
  char type;
  be32_t user_ref_num;
  char firm[4];
  be16_t appendage_length;
};

struct AccountQuery {  // 'Q' 2.8
  char type;
  be16_t appendage_length;
};

// ---- outbound, section 3 ---------------------------------------------------------------

struct SystemEvent {  // 'S' 3.1
  char type;
  be64_t timestamp;
  char event_code;
};

struct OrderAccepted {  // 'A' 3.2
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  char side;
  be32_t quantity;
  char symbol[8];
  be64_t price;
  char time_in_force;
  char display;
  be64_t order_reference_number;
  char capacity;
  char intermarket_sweep_eligibility;
  char cross_type;
  char order_state;
  char cl_ord_id[14];
  be16_t appendage_length;
};

struct OrderReplaced {  // 'U' 3.3
  char type;
  be64_t timestamp;
  be32_t orig_user_ref_num;
  be32_t user_ref_num;
  char side;
  be32_t quantity;
  char symbol[8];
  be64_t price;
  char time_in_force;
  char display;
  be64_t order_reference_number;
  char capacity;
  char intermarket_sweep_eligibility;
  char cross_type;
  char order_state;
  char cl_ord_id[14];
  be16_t appendage_length;
};

struct OrderCanceled {  // 'C' 3.4
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  be32_t quantity;  // decrement
  char reason;
  be16_t appendage_length;  // optional
};

struct AiqCanceled {  // 'D' 3.5
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  be32_t decrement_shares;
  char reason;
  be32_t quantity_prevented_from_trading;
  be64_t execution_price;
  char liquidity_flag;
  char aiq_strategy;
  be16_t appendage_length;  // optional
};

struct OrderExecuted {  // 'E' 3.6
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  be32_t quantity;
  be64_t price;
  char liquidity_flag;
  be64_t match_number;
  be16_t appendage_length;
};

struct BrokenTrade {  // 'B' 3.7
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  be64_t match_number;
  char reason;
  char cl_ord_id[14];
  be16_t appendage_length;  // optional
};

struct Rejected {  // 'J' 3.8
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  be16_t reason;
  char cl_ord_id[14];
  be16_t appendage_length;  // optional
};

struct CancelPending {  // 'P' 3.9 (also the layout of 'I' Cancel Reject 3.10)
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  be16_t appendage_length;  // optional
};
using CancelReject = CancelPending;

struct OrderPriorityUpdate {  // 'T' 3.11
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  be64_t price;
  char display;
  be64_t order_reference_number;
  be16_t appendage_length;  // optional
};

struct OrderModified {  // 'M' 3.12
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  char side;
  be32_t quantity;
  be16_t appendage_length;  // optional
};

struct OrderRestated {  // 'R' 3.13
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  char reason;
  be16_t appendage_length;
};

struct MassCancelResponse {  // 'X' 3.14
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  char firm[4];
  char symbol[8];
  be16_t appendage_length;
};

struct OrderEntryControlResponse {  // 'G' Disable 3.15 / 'K' Enable 3.16
  char type;
  be64_t timestamp;
  be32_t user_ref_num;
  char firm[4];
  be16_t appendage_length;
};

struct AccountQueryResponse {  // 'Q' 3.17
  char type;
  be64_t timestamp;
  be32_t next_user_ref_num;
  be16_t appendage_length;  // optional
};

#pragma pack(pop)

static_assert(sizeof(EnterOrder) == 47 && offsetof(EnterOrder, side) == 5 &&
              offsetof(EnterOrder, quantity) == 6 && offsetof(EnterOrder, symbol) == 10 &&
              offsetof(EnterOrder, price) == 18 && offsetof(EnterOrder, time_in_force) == 26 &&
              offsetof(EnterOrder, cross_type) == 30 && offsetof(EnterOrder, cl_ord_id) == 31 &&
              offsetof(EnterOrder, appendage_length) == 45);
static_assert(sizeof(ReplaceOrder) == 40 && offsetof(ReplaceOrder, user_ref_num) == 5 &&
              offsetof(ReplaceOrder, quantity) == 9 && offsetof(ReplaceOrder, price) == 13 &&
              offsetof(ReplaceOrder, time_in_force) == 21 &&
              offsetof(ReplaceOrder, cl_ord_id) == 24 &&
              offsetof(ReplaceOrder, appendage_length) == 38);
static_assert(sizeof(CancelOrder) == 11 && offsetof(CancelOrder, appendage_length) == 9);
static_assert(sizeof(ModifyOrder) == 12 && offsetof(ModifyOrder, appendage_length) == 10);
static_assert(sizeof(MassCancel) == 19 && offsetof(MassCancel, symbol) == 9);
static_assert(sizeof(OrderEntryControl) == 11 &&
              offsetof(OrderEntryControl, appendage_length) == 9);
static_assert(sizeof(AccountQuery) == 3);
static_assert(sizeof(SystemEvent) == 10);
static_assert(sizeof(OrderAccepted) == 64 && offsetof(OrderAccepted, user_ref_num) == 9 &&
              offsetof(OrderAccepted, side) == 13 && offsetof(OrderAccepted, quantity) == 14 &&
              offsetof(OrderAccepted, symbol) == 18 && offsetof(OrderAccepted, price) == 26 &&
              offsetof(OrderAccepted, time_in_force) == 34 &&
              offsetof(OrderAccepted, display) == 35 &&
              offsetof(OrderAccepted, order_reference_number) == 36 &&
              offsetof(OrderAccepted, capacity) == 44 &&
              offsetof(OrderAccepted, order_state) == 47 &&
              offsetof(OrderAccepted, cl_ord_id) == 48 &&
              offsetof(OrderAccepted, appendage_length) == 62);
static_assert(sizeof(OrderReplaced) == 68 && offsetof(OrderReplaced, user_ref_num) == 13 &&
              offsetof(OrderReplaced, side) == 17 && offsetof(OrderReplaced, price) == 30 &&
              offsetof(OrderReplaced, order_reference_number) == 40 &&
              offsetof(OrderReplaced, order_state) == 51 &&
              offsetof(OrderReplaced, cl_ord_id) == 52 &&
              offsetof(OrderReplaced, appendage_length) == 66);
static_assert(sizeof(OrderCanceled) == 20 && offsetof(OrderCanceled, reason) == 17);
static_assert(sizeof(AiqCanceled) == 34 && offsetof(AiqCanceled, execution_price) == 22 &&
              offsetof(AiqCanceled, liquidity_flag) == 30 &&
              offsetof(AiqCanceled, appendage_length) == 32);
static_assert(sizeof(OrderExecuted) == 36 && offsetof(OrderExecuted, price) == 17 &&
              offsetof(OrderExecuted, liquidity_flag) == 25 &&
              offsetof(OrderExecuted, match_number) == 26 &&
              offsetof(OrderExecuted, appendage_length) == 34);
static_assert(sizeof(BrokenTrade) == 38 && offsetof(BrokenTrade, reason) == 21 &&
              offsetof(BrokenTrade, cl_ord_id) == 22);
static_assert(sizeof(Rejected) == 31 && offsetof(Rejected, reason) == 13 &&
              offsetof(Rejected, cl_ord_id) == 15 && offsetof(Rejected, appendage_length) == 29);
static_assert(sizeof(CancelPending) == 15);
static_assert(sizeof(OrderPriorityUpdate) == 32 && offsetof(OrderPriorityUpdate, display) == 21 &&
              offsetof(OrderPriorityUpdate, order_reference_number) == 22);
static_assert(sizeof(OrderModified) == 20 && offsetof(OrderModified, quantity) == 14);
static_assert(sizeof(OrderRestated) == 16 && offsetof(OrderRestated, appendage_length) == 14);
static_assert(sizeof(MassCancelResponse) == 27 &&
              offsetof(MassCancelResponse, appendage_length) == 25);
static_assert(sizeof(OrderEntryControlResponse) == 19 &&
              offsetof(OrderEntryControlResponse, appendage_length) == 17);
static_assert(sizeof(AccountQueryResponse) == 15);

// Appendix "Optional Fields": OptionTag values.
enum class OptionTag : std::uint8_t {
  SecondaryOrdRefNum = 1,
  Firm = 2,
  MinQty = 3,
  CustomerType = 4,
  MaxFloor = 5,
  PriceType = 6,
  PegOffset = 7,
  DiscretionPrice = 9,
  DiscretionPriceType = 10,
  DiscretionPegOffset = 11,
  PostOnly = 12,
  RandomReserves = 13,
  Route = 14,
  ExpireTime = 15,
  TradeNow = 16,
  HandleInst = 17,
  BboWeightIndicator = 18,
  DisplayQuantity = 22,
  DisplayPrice = 23,
  GroupId = 24,
  SharesLocated = 25,
  LocateBroker = 26,
  Side = 27,
  UserRefIdx = 28,
  AiqStrategy = 29,
  AiqGroupId = 30,
};

inline constexpr char kTifDay = '0';
inline constexpr char kTifIoc = '3';
inline constexpr char kTifGtx = '5';
inline constexpr char kTifGtt = '6';
inline constexpr char kTifAfterHours = 'E';
inline constexpr std::uint64_t kMaxPrice = 0x7735939C;          // $199,999.9900
inline constexpr std::uint64_t kMarketCrossPrice = 0x7FFFFFFF;  // market order for a cross
inline constexpr std::uint32_t kMaxQuantity = 999'999;

// Outbound layout: bytes before Appendage Length, and whether Appendage Length may be absent.
struct OutboundLayout {
  std::size_t base = 0;  // 0: unknown type
  bool has_appendage = true;
  bool appendage_optional = false;
};
[[nodiscard]] OutboundLayout outbound_layout(char type) noexcept;
// Inbound fixed part before Appendage Length (0 for unknown types).
[[nodiscard]] std::size_t inbound_base(char type) noexcept;
// Validates the Appendage Length / TagValue structure after `base` bytes; sets `appendage`.
[[nodiscard]] bool split_message(std::span<const std::byte> msg,
                                 std::size_t base,
                                 bool optional,
                                 std::span<const std::byte>& appendage) noexcept;
// Appends one TagValue element at out[used..]; false when it does not fit.
bool append_option(std::span<std::byte> out,
                   std::size_t& used,
                   OptionTag tag,
                   std::span<const std::byte> value) noexcept;
// Finds a tag's value in an appendage; false when absent or the appendage is malformed.
[[nodiscard]] bool find_option(std::span<const std::byte> appendage,
                               OptionTag tag,
                               std::span<const std::byte>& value) noexcept;
[[nodiscard]] std::string_view reject_reason_text(std::uint16_t reason) noexcept;

class UserRefMap {
 public:
  static constexpr std::size_t kSlots = 1U << 17;

  explicit UserRefMap(std::uint32_t first = 1) noexcept : next_(first == 0 ? 1 : first) {}
  UserRefMap(const UserRefMap&) = delete;
  UserRefMap& operator=(const UserRefMap&) = delete;

  // The id's UserRefNum, assigning the next one for a new id. 0 when full or exhausted.
  std::uint32_t assign(ClientOrderId id) noexcept;
  [[nodiscard]] std::uint32_t find(ClientOrderId id) const noexcept;
  [[nodiscard]] ClientOrderId find(std::uint32_t user_ref_num) const noexcept;
  void erase(std::uint32_t user_ref_num) noexcept;
  // NextUserRefNum from an Account Query Response; never moves the counter backwards.
  void set_next(std::uint32_t next) noexcept {
    if (next > next_) next_ = next;
  }
  [[nodiscard]] std::uint32_t next() const noexcept { return next_; }
  [[nodiscard]] std::size_t size() const noexcept { return by_urn_.size(); }

 private:
  OpenHashMap<std::uint64_t, std::uint32_t, kSlots> by_id_;
  OpenHashMap<std::uint64_t, ClientOrderId, kSlots> by_urn_;
  std::uint32_t next_;
};

struct EncoderConfig {
  char firm[4] = {' ', ' ', ' ', ' '};  // non-blank adds the Firm option
  char display = 'Y';
  char capacity = 'P';
  char intermarket_sweep = 'N';
  char cross_type = 'N';
  char replace_time_in_force = kTifDay;
};

class OuchEncoder {
 public:
  explicit OuchEncoder(UserRefMap& ids, const EncoderConfig& cfg = {}) : ids_(ids), cfg_(cfg) {}

  bool add_symbol(std::string_view symbol, InstrumentId id) noexcept {
    return symbols_.add(symbol, id);
  }
  std::size_t encode(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;

  [[nodiscard]] static char time_in_force(TimeInForce tif) noexcept;
  [[nodiscard]] const ouch::EncoderStats& stats() const noexcept { return stats_; }

 private:
  std::size_t encode_new(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;
  std::size_t encode_replace(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;
  std::size_t encode_cancel(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;

  UserRefMap& ids_;
  EncoderConfig cfg_;
  ouch::SymbolMap symbols_;
  ouch::EncoderStats stats_{};
};
static_assert(codecs::Encoder<OuchEncoder>);

class OuchDecoder {
 public:
  // `ids` may be null: identifiers then come only from echoed ClOrdID fields, and executions
  // or cancels of orders accepted before the decoder started are counted as unknown.
  explicit OuchDecoder(UserRefMap* ids, VenueId venue = VenueId{0}) : ids_(ids), venue_(venue) {}
  OuchDecoder(const OuchDecoder&) = delete;
  OuchDecoder& operator=(const OuchDecoder&) = delete;

  bool add_symbol(std::string_view symbol, InstrumentId id) noexcept {
    return symbols_.add(symbol, id);
  }
  void set_midnight(Timestamp midnight) noexcept { midnight_ns_ = midnight.ns; }
  void set_venue_seq(std::uint64_t seq) noexcept { venue_seq_ = seq; }

  ParseStatus decode(const FrameView& frame, std::int64_t rx_ts, venues::EventSink& sink) noexcept;

  [[nodiscard]] const ouch::OrderEntry* order(std::uint32_t user_ref_num) const noexcept {
    return orders_.find(user_ref_num);
  }
  [[nodiscard]] std::size_t open_orders() const noexcept { return orders_.size(); }
  [[nodiscard]] const ouch::DecoderStats& stats() const noexcept { return stats_; }

 private:
  [[nodiscard]] ouch::EventStamp stamp(std::uint64_t ts,
                                       InstrumentId inst,
                                       std::int64_t rx) const noexcept {
    return {venue_, inst, venue_seq_, midnight_ns_ + static_cast<std::int64_t>(ts), rx};
  }
  [[nodiscard]] ClientOrderId resolve(std::uint32_t urn, const char* cl_ord_id14) const noexcept;
  void forget(std::uint32_t urn) noexcept;
  ParseStatus done(bool committed, std::uint64_t events) noexcept;
  ParseStatus on_accepted(const OrderAccepted& m,
                          std::int64_t rx,
                          venues::EventSink& sink) noexcept;
  ParseStatus on_replaced(const OrderReplaced& m,
                          std::int64_t rx,
                          venues::EventSink& sink) noexcept;
  ParseStatus on_canceled(std::uint64_t ts,
                          std::uint32_t urn,
                          std::uint32_t decrement,
                          char reason,
                          std::int64_t rx,
                          venues::EventSink& sink) noexcept;
  ParseStatus on_executed(const OrderExecuted& m,
                          std::int64_t rx,
                          venues::EventSink& sink) noexcept;

  UserRefMap* ids_;
  VenueId venue_;
  std::int64_t midnight_ns_ = 0;
  std::uint64_t venue_seq_ = 0;
  ouch::SymbolMap symbols_;
  ouch::OrderTable orders_;  // keyed by UserRefNum
  ouch::DecoderStats stats_{};
};
static_assert(codecs::Decoder<OuchDecoder>);

// ---- host side (simulated OUCH port, replay): outbound builders -------------------------
// Each returns bytes written (Appendage Length 0), 0 when `out` is too small.
namespace host {
std::size_t system_event(std::span<std::byte> out, std::uint64_t ts, char event_code) noexcept;
std::size_t accepted(std::span<std::byte> out,
                     std::uint64_t ts,
                     const EnterOrder& in,
                     std::uint32_t quantity,
                     std::uint64_t reference_number,
                     char order_state) noexcept;
std::size_t replaced(std::span<std::byte> out,
                     std::uint64_t ts,
                     const ReplaceOrder& in,
                     const EnterOrder& original,
                     std::uint32_t quantity_outstanding,
                     std::uint64_t reference_number,
                     char order_state) noexcept;
std::size_t canceled(std::span<std::byte> out,
                     std::uint64_t ts,
                     std::uint32_t user_ref_num,
                     std::uint32_t quantity,
                     char reason) noexcept;
std::size_t executed(std::span<std::byte> out,
                     std::uint64_t ts,
                     std::uint32_t user_ref_num,
                     std::uint32_t quantity,
                     std::uint64_t price,
                     char liquidity_flag,
                     std::uint64_t match_number) noexcept;
std::size_t rejected(std::span<std::byte> out,
                     std::uint64_t ts,
                     std::uint32_t user_ref_num,
                     std::uint16_t reason,
                     const char* cl_ord_id14) noexcept;
std::size_t cancel_reject(std::span<std::byte> out,
                          std::uint64_t ts,
                          std::uint32_t user_ref_num) noexcept;
std::size_t account_query_response(std::span<std::byte> out,
                                   std::uint64_t ts,
                                   std::uint32_t next_user_ref_num) noexcept;
}  // namespace host

}  // namespace fastmm::codecs::ouch50
