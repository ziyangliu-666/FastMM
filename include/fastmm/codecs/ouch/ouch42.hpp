#pragma once
// Nasdaq OUCH 4.2 (fastmm::codecs::ouch42).
//
// Source: "O*U*C*H Version 4.2", Nasdaq North American Trading Services, updated October 2025
// (OUCH4.2.pdf). Integers are unsigned big-endian, alpha fields left-justified and space
// padded, prices 4-byte integers with 4 implied decimals, timestamps 8-byte nanoseconds past
// midnight. Messages travel inside SoupBinTCP (inbound: Unsequenced Data, outbound:
// Sequenced Data).
//
// Order Token (14 alphanumeric, day-unique per OUCH account) <-> ClientOrderId: the token is
// exactly encode_cl_ord_id() ("fm" + 12 lowercase hex digits, 14 characters), so decoding a
// token is decode_cl_ord_id() and no id table is needed. Tokens of any other shape are
// counted as foreign and ignored.
//
// OuchEncoder (satisfies Encoder), OrderCommand ->
//   New      Enter Order 'O'  (49 bytes)
//   Replace  Replace Order 'U' (47): existing token = orig_cl_ord_id, replacement = cl_ord_id
//   Cancel   Cancel Order 'X' (19) with Shares 0 (cancel the whole balance)
//   Time in Force: GTC -> 99999 (system hours), DAY -> 99998 (market hours), IOC -> 0,
//   FOK -> 0 with Minimum Quantity = Shares. PostOnly -> Display 'P'. Market orders have no
//   continuous-market form in OUCH 4.2 (the special price 0x7FFFFFFF is for crosses) and are
//   refused. Replace uses EncoderConfig::replace_time_in_force because OrderCommand does not
//   carry a TIF for replaces. The Replace Shares field is "total shares liable inclusive of
//   previous executions"; the command's qty is sent unchanged, so the caller must pass that
//   total when the order was partially filled.
//
// OuchDecoder (satisfies Decoder), outbound message ->
//   'A' Accepted          OrderAckMsg (+ OrderExpiredMsg when Order State is 'D')
//   'U' Replaced          OrderAckMsg for the replacement token (the OMS completes the replace
//                         on the pending id); cum carries over the replace chain
//   'C' Canceled, 'D' AIQ OrderCancelAckMsg once no shares are left, OrderExpiredMsg when the
//                         reason is 'I' (IOC) or 'T' (timeout); partial reductions update the
//                         order table only
//   'E' Executed, 'G' Executed with Reference Price -> OrderFillMsg
//   'J' Rejected          OrderRejectMsg (venue_code = reason character)
//   'I' Cancel Reject     OrderCancelRejectMsg
//   'S' System Event, 'B' Broken Trade, 'P' Cancel Pending, 'T' Order Priority Update,
//   'M' Order Modified (updates leaves)  -> no event (counted)
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/ouch/ouch_common.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/venues/order_commands.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fastmm::codecs::ouch42 {

using venues::ParseStatus;

#pragma pack(push, 1)

// ---- inbound (client -> Nasdaq), section 2 ----------------------------------------------

struct EnterOrder {  // 'O' 2.1
  char type;
  char order_token[14];
  char buy_sell_indicator;
  be32_t shares;
  char stock[8];
  be32_t price;
  be32_t time_in_force;
  char firm[4];
  char display;
  char capacity;
  char intermarket_sweep_eligibility;
  be32_t minimum_quantity;
  char cross_type;
  char customer_type;
};

struct ReplaceOrder {  // 'U' 2.2
  char type;
  char existing_order_token[14];
  char replacement_order_token[14];
  be32_t shares;
  be32_t price;
  be32_t time_in_force;
  char display;
  char intermarket_sweep_eligibility;
  be32_t minimum_quantity;
};

struct CancelOrder {  // 'X' 2.3
  char type;
  char order_token[14];
  be32_t shares;  // new intended order size; 0 cancels the balance
};

struct ModifyOrder {  // 'M' 2.4
  char type;
  char order_token[14];
  char buy_sell_indicator;
  be32_t shares;
};

// ---- outbound (Nasdaq -> client), section 3 ---------------------------------------------

struct SystemEvent {  // 'S' 3.1
  char type;
  be64_t timestamp;
  char event_code;  // S start of day, E end of day
};

struct Accepted {  // 'A' 3.3
  char type;
  be64_t timestamp;
  char order_token[14];
  char buy_sell_indicator;
  be32_t shares;
  char stock[8];
  be32_t price;
  be32_t time_in_force;
  char firm[4];
  char display;
  be64_t order_reference_number;
  char capacity;
  char intermarket_sweep_eligibility;
  be32_t minimum_quantity;
  char cross_type;
  char order_state;  // L live, D dead
  char bbo_weight_indicator;
};

struct Replaced {  // 'U' 3.4
  char type;
  be64_t timestamp;
  char replacement_order_token[14];
  char buy_sell_indicator;
  be32_t shares;
  char stock[8];
  be32_t price;
  be32_t time_in_force;
  char firm[4];
  char display;
  be64_t order_reference_number;
  char capacity;
  char intermarket_sweep_eligibility;
  be32_t minimum_quantity;
  char cross_type;
  char order_state;
  char previous_order_token[14];
  char bbo_weight_indicator;
};

struct Canceled {  // 'C' 3.5
  char type;
  be64_t timestamp;
  char order_token[14];
  be32_t decrement_shares;
  char reason;
};

struct AiqCanceled {  // 'D' 3.6
  char type;
  be64_t timestamp;
  char order_token[14];
  be32_t decrement_shares;
  char reason;  // 'Q'
  be32_t quantity_prevented_from_trading;
  be32_t execution_price;
  char liquidity_flag;
  char aiq_strategy;
};

struct Executed {  // 'E' 3.7
  char type;
  be64_t timestamp;
  char order_token[14];
  be32_t executed_shares;
  be32_t execution_price;
  char liquidity_flag;
  be64_t match_number;
};

struct BrokenTrade {  // 'B' 3.8
  char type;
  be64_t timestamp;
  char order_token[14];
  be64_t match_number;
  char reason;
};

struct ExecutedWithReferencePrice {  // 'G' 3.9
  char type;
  be64_t timestamp;
  char order_token[14];
  be32_t executed_shares;
  be32_t execution_price;
  char liquidity_flag;
  be64_t match_number;
  be32_t reference_price;
  char reference_price_type;
};

struct Rejected {  // 'J' 3.10
  char type;
  be64_t timestamp;
  char order_token[14];
  char reason;
};

struct CancelPending {  // 'P' 3.11
  char type;
  be64_t timestamp;
  char order_token[14];
};

struct CancelReject {  // 'I' 3.12
  char type;
  be64_t timestamp;
  char order_token[14];
};

struct OrderPriorityUpdate {  // 'T' 3.13
  char type;
  be64_t timestamp;
  char order_token[14];
  be32_t price;
  char display;
  be64_t order_reference_number;
};

struct OrderModified {  // 'M' 3.14
  char type;
  be64_t timestamp;
  char order_token[14];
  char buy_sell_indicator;
  be32_t shares;
};

#pragma pack(pop)

static_assert(sizeof(EnterOrder) == 49 && offsetof(EnterOrder, shares) == 16 &&
              offsetof(EnterOrder, stock) == 20 && offsetof(EnterOrder, price) == 28 &&
              offsetof(EnterOrder, time_in_force) == 32 && offsetof(EnterOrder, firm) == 36 &&
              offsetof(EnterOrder, display) == 40 && offsetof(EnterOrder, minimum_quantity) == 43 &&
              offsetof(EnterOrder, cross_type) == 47 && offsetof(EnterOrder, customer_type) == 48);
static_assert(sizeof(ReplaceOrder) == 47 && offsetof(ReplaceOrder, replacement_order_token) == 15 &&
              offsetof(ReplaceOrder, shares) == 29 && offsetof(ReplaceOrder, price) == 33 &&
              offsetof(ReplaceOrder, time_in_force) == 37 &&
              offsetof(ReplaceOrder, display) == 41 &&
              offsetof(ReplaceOrder, minimum_quantity) == 43);
static_assert(sizeof(CancelOrder) == 19 && offsetof(CancelOrder, shares) == 15);
static_assert(sizeof(ModifyOrder) == 20 && offsetof(ModifyOrder, shares) == 16);
static_assert(sizeof(SystemEvent) == 10 && offsetof(SystemEvent, event_code) == 9);
static_assert(sizeof(Accepted) == 66 && offsetof(Accepted, order_token) == 9 &&
              offsetof(Accepted, buy_sell_indicator) == 23 && offsetof(Accepted, shares) == 24 &&
              offsetof(Accepted, stock) == 28 && offsetof(Accepted, price) == 36 &&
              offsetof(Accepted, time_in_force) == 40 && offsetof(Accepted, firm) == 44 &&
              offsetof(Accepted, display) == 48 &&
              offsetof(Accepted, order_reference_number) == 49 &&
              offsetof(Accepted, capacity) == 57 && offsetof(Accepted, minimum_quantity) == 59 &&
              offsetof(Accepted, cross_type) == 63 && offsetof(Accepted, order_state) == 64 &&
              offsetof(Accepted, bbo_weight_indicator) == 65);
static_assert(sizeof(Replaced) == 80 && offsetof(Replaced, order_reference_number) == 49 &&
              offsetof(Replaced, order_state) == 64 &&
              offsetof(Replaced, previous_order_token) == 65 &&
              offsetof(Replaced, bbo_weight_indicator) == 79);
static_assert(sizeof(Canceled) == 28 && offsetof(Canceled, decrement_shares) == 23 &&
              offsetof(Canceled, reason) == 27);
static_assert(sizeof(AiqCanceled) == 38 &&
              offsetof(AiqCanceled, quantity_prevented_from_trading) == 28 &&
              offsetof(AiqCanceled, execution_price) == 32 &&
              offsetof(AiqCanceled, liquidity_flag) == 36 &&
              offsetof(AiqCanceled, aiq_strategy) == 37);
static_assert(sizeof(Executed) == 40 && offsetof(Executed, executed_shares) == 23 &&
              offsetof(Executed, execution_price) == 27 &&
              offsetof(Executed, liquidity_flag) == 31 && offsetof(Executed, match_number) == 32);
static_assert(sizeof(BrokenTrade) == 32 && offsetof(BrokenTrade, match_number) == 23 &&
              offsetof(BrokenTrade, reason) == 31);
static_assert(sizeof(ExecutedWithReferencePrice) == 45 &&
              offsetof(ExecutedWithReferencePrice, reference_price) == 40 &&
              offsetof(ExecutedWithReferencePrice, reference_price_type) == 44);
static_assert(sizeof(Rejected) == 24 && offsetof(Rejected, reason) == 23);
static_assert(sizeof(CancelPending) == 23 && sizeof(CancelReject) == 23);
static_assert(sizeof(OrderPriorityUpdate) == 36 && offsetof(OrderPriorityUpdate, display) == 27 &&
              offsetof(OrderPriorityUpdate, order_reference_number) == 28);
static_assert(sizeof(OrderModified) == 28 && offsetof(OrderModified, shares) == 24);

// Data Types: special Time in Force values and price limits.
inline constexpr std::uint32_t kTifImmediateOrCancel = 0;
inline constexpr std::uint32_t kTifExtendedTradingClose = 99'996;
inline constexpr std::uint32_t kTifMarketHours = 99'998;
inline constexpr std::uint32_t kTifSystemHours = 99'999;
inline constexpr std::uint32_t kMaxPrice = 0x7735939C;          // $199,999.9900
inline constexpr std::uint32_t kMarketCrossPrice = 0x7FFFFFFF;  // market order for a cross
inline constexpr std::uint32_t kMaxShares = 999'999;            // "less than 1,000,000"

[[nodiscard]] std::size_t inbound_length(char type) noexcept;   // 0 for unknown types
[[nodiscard]] std::size_t outbound_length(char type) noexcept;  // 0 for unknown types
[[nodiscard]] std::string_view reject_reason_text(char reason) noexcept;
[[nodiscard]] std::string_view cancel_reason_text(char reason) noexcept;

void put_token(char* dst14, ClientOrderId id) noexcept;
[[nodiscard]] std::optional<ClientOrderId> token_to_cl_ord_id(const char* token14) noexcept;

struct EncoderConfig {
  char firm[4] = {' ', ' ', ' ', ' '};  // blank: the account's default firm
  char display = 'Y';                   // Anonymous-Price to Comply
  char capacity = 'P';                  // principal
  char intermarket_sweep = 'N';
  char cross_type = 'N';     // continuous market
  char customer_type = ' ';  // port default
  std::uint32_t replace_time_in_force = kTifSystemHours;
};

class OuchEncoder {
 public:
  explicit OuchEncoder(const EncoderConfig& cfg = {}) : cfg_(cfg) {}

  bool add_symbol(std::string_view symbol, InstrumentId id) noexcept {
    return symbols_.add(symbol, id);
  }
  std::size_t encode(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;

  [[nodiscard]] static std::uint32_t time_in_force(TimeInForce tif) noexcept;
  [[nodiscard]] const ouch::EncoderStats& stats() const noexcept { return stats_; }

 private:
  std::size_t encode_new(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;
  std::size_t encode_replace(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;
  std::size_t encode_cancel(const venues::OrderCommand& cmd, std::span<std::byte> out) noexcept;

  EncoderConfig cfg_;
  ouch::SymbolMap symbols_;
  ouch::EncoderStats stats_{};
};
static_assert(codecs::Encoder<OuchEncoder>);

class OuchDecoder {
 public:
  explicit OuchDecoder(VenueId venue = VenueId{0}) : venue_(venue) {}
  OuchDecoder(const OuchDecoder&) = delete;
  OuchDecoder& operator=(const OuchDecoder&) = delete;

  bool add_symbol(std::string_view symbol, InstrumentId id) noexcept {
    return symbols_.add(symbol, id);
  }
  void set_midnight(Timestamp midnight) noexcept { midnight_ns_ = midnight.ns; }
  void set_venue_seq(std::uint64_t seq) noexcept { venue_seq_ = seq; }

  ParseStatus decode(const FrameView& frame, std::int64_t rx_ts, venues::EventSink& sink) noexcept;

  [[nodiscard]] const ouch::OrderEntry* order(ClientOrderId id) const noexcept {
    return orders_.find(id.value);
  }
  [[nodiscard]] std::size_t open_orders() const noexcept { return orders_.size(); }
  [[nodiscard]] const ouch::DecoderStats& stats() const noexcept { return stats_; }

 private:
  [[nodiscard]] ouch::EventStamp stamp(std::uint64_t ts,
                                       InstrumentId inst,
                                       std::int64_t rx) const noexcept {
    return {venue_, inst, venue_seq_, midnight_ns_ + static_cast<std::int64_t>(ts), rx};
  }
  ParseStatus done(bool committed, std::uint64_t events) noexcept;
  ParseStatus on_accepted(const Accepted& m, std::int64_t rx, venues::EventSink& sink) noexcept;
  ParseStatus on_replaced(const Replaced& m, std::int64_t rx, venues::EventSink& sink) noexcept;
  ParseStatus on_canceled(std::uint64_t ts,
                          const char* token,
                          std::uint32_t decrement,
                          char reason,
                          std::int64_t rx,
                          venues::EventSink& sink) noexcept;
  ParseStatus on_executed(std::uint64_t ts,
                          const char* token,
                          std::uint32_t shares,
                          std::uint32_t price,
                          char liquidity,
                          std::uint64_t match,
                          std::int64_t rx,
                          venues::EventSink& sink) noexcept;

  VenueId venue_;
  std::int64_t midnight_ns_ = 0;
  std::uint64_t venue_seq_ = 0;
  ouch::SymbolMap symbols_;
  ouch::OrderTable orders_;
  ouch::DecoderStats stats_{};
};
static_assert(codecs::Decoder<OuchDecoder>);

// ---- host side (simulated OUCH port, replay): outbound builders -------------------------
// Each returns bytes written, 0 when `out` is too small.
namespace host {
std::size_t system_event(std::span<std::byte> out, std::uint64_t ts, char event_code) noexcept;
// Echoes the Enter Order fields; firm, BBO weight and capacity as entered.
std::size_t accepted(std::span<std::byte> out,
                     std::uint64_t ts,
                     const EnterOrder& in,
                     std::uint32_t shares,
                     std::uint64_t reference_number,
                     char order_state) noexcept;
std::size_t replaced(std::span<std::byte> out,
                     std::uint64_t ts,
                     const ReplaceOrder& in,
                     const EnterOrder& original,
                     std::uint32_t shares_outstanding,
                     std::uint64_t reference_number,
                     char order_state) noexcept;
std::size_t canceled(std::span<std::byte> out,
                     std::uint64_t ts,
                     const char* token14,
                     std::uint32_t decrement_shares,
                     char reason) noexcept;
std::size_t executed(std::span<std::byte> out,
                     std::uint64_t ts,
                     const char* token14,
                     std::uint32_t shares,
                     std::uint32_t price,
                     char liquidity_flag,
                     std::uint64_t match_number) noexcept;
std::size_t rejected(std::span<std::byte> out,
                     std::uint64_t ts,
                     const char* token14,
                     char reason) noexcept;
std::size_t cancel_reject(std::span<std::byte> out, std::uint64_t ts, const char* token14) noexcept;
}  // namespace host

}  // namespace fastmm::codecs::ouch42
