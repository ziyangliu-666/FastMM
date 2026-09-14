#pragma once
// Nasdaq TotalView-ITCH 5.0 wire layouts (fastmm::codecs::itch).
//
// Source: "Nasdaq TotalView-ITCH 5.0" specification, nasdaqtrader.com
// (NQTVITCHspecification.pdf, revision log entry of April 28, 2023). Section numbers below
// refer to it. Every message is a #pragma pack(1) struct whose sizeof is asserted against the
// length derived from the spec's offset/length table. All integers are big-endian; alpha
// fields are ASCII, left-justified and space padded. The first 11 bytes of every message are
// the common header (type, stock locate, tracking number, 6-byte timestamp).
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/itch/nasdaq_fields.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace fastmm::codecs::itch {

using nasdaq::be48_t;

enum class MessageType : char {
  SystemEvent = 'S',                // 1.1
  StockDirectory = 'R',             // 1.2.1
  StockTradingAction = 'H',         // 1.2.2
  RegShoRestriction = 'Y',          // 1.2.3
  MarketParticipantPosition = 'L',  // 1.2.4
  MwcbDeclineLevel = 'V',           // 1.2.5.1
  MwcbStatus = 'W',                 // 1.2.5.2
  IpoQuotingPeriodUpdate = 'K',     // 1.2.6
  LuldAuctionCollar = 'J',          // 1.2.7
  OperationalHalt = 'h',            // 1.2.8
  AddOrder = 'A',                   // 1.3.1
  AddOrderMpid = 'F',               // 1.3.2
  OrderExecuted = 'E',              // 1.4.1
  OrderExecutedWithPrice = 'C',     // 1.4.2
  OrderCancel = 'X',                // 1.4.3
  OrderDelete = 'D',                // 1.4.4
  OrderReplace = 'U',               // 1.4.5
  Trade = 'P',                      // 1.5.1 (non-cross)
  CrossTrade = 'Q',                 // 1.5.2
  BrokenTrade = 'B',                // 1.5.3
  Noii = 'I',                       // 1.6
  RetailInterest = 'N',             // 1.7
  DlcrPriceDiscovery = 'O',         // 1.8
};

#pragma pack(push, 1)

// Offsets 0..10, identical in every message ("The Stock Locate code appears [in] all messages,
// and at the same position"). Stock Locate is 0 for messages that are not stock dependent.
struct MessageHeader {
  char message_type;
  be16_t stock_locate;
  be16_t tracking_number;
  be48_t timestamp;  // nanoseconds since midnight
};

struct SystemEvent {  // 'S'
  MessageHeader hdr;
  char event_code;  // O S Q M E C
};

struct StockDirectory {  // 'R'
  MessageHeader hdr;
  char stock[8];
  char market_category;
  char financial_status_indicator;
  be32_t round_lot_size;
  char round_lots_only;
  char issue_classification;
  char issue_sub_type[2];
  char authenticity;
  char short_sale_threshold_indicator;
  char ipo_flag;
  char luld_reference_price_tier;
  char etp_flag;
  be32_t etp_leverage_factor;
  char inverse_indicator;
};

struct StockTradingAction {  // 'H'
  MessageHeader hdr;
  char stock[8];
  char trading_state;  // H P Q T
  char reserved;
  char reason[4];
};

struct RegShoRestriction {  // 'Y'
  MessageHeader hdr;
  char stock[8];
  char reg_sho_action;  // '0' '1' '2'
};

struct MarketParticipantPosition {  // 'L'
  MessageHeader hdr;
  char mpid[4];
  char stock[8];
  char primary_market_maker;
  char market_maker_mode;
  char market_participant_state;
};

struct MwcbDeclineLevel {  // 'V'
  MessageHeader hdr;
  be64_t level1;  // Price(8)
  be64_t level2;  // Price(8)
  be64_t level3;  // Price(8)
};

struct MwcbStatus {  // 'W'
  MessageHeader hdr;
  char breached_level;  // '1' '2' '3'
};

struct IpoQuotingPeriodUpdate {  // 'K'
  MessageHeader hdr;
  char stock[8];
  be32_t ipo_quotation_release_time;  // seconds since midnight
  char ipo_quotation_release_qualifier;
  be32_t ipo_price;  // Price(4)
};

struct LuldAuctionCollar {  // 'J'
  MessageHeader hdr;
  char stock[8];
  be32_t auction_collar_reference_price;  // Price(4)
  be32_t upper_auction_collar_price;      // Price(4)
  be32_t lower_auction_collar_price;      // Price(4)
  be32_t auction_collar_extension;
};

struct OperationalHalt {  // 'h'
  MessageHeader hdr;
  char stock[8];
  char market_code;              // Q B X
  char operational_halt_action;  // H T
};

struct AddOrder {  // 'A'
  MessageHeader hdr;
  be64_t order_reference_number;
  char buy_sell_indicator;  // B S
  be32_t shares;
  char stock[8];
  be32_t price;  // Price(4)
};

struct AddOrderMpid {  // 'F'
  MessageHeader hdr;
  be64_t order_reference_number;
  char buy_sell_indicator;
  be32_t shares;
  char stock[8];
  be32_t price;  // Price(4)
  char attribution[4];
};

struct OrderExecuted {  // 'E'
  MessageHeader hdr;
  be64_t order_reference_number;
  be32_t executed_shares;
  be64_t match_number;
};

struct OrderExecutedWithPrice {  // 'C'
  MessageHeader hdr;
  be64_t order_reference_number;
  be32_t executed_shares;
  be64_t match_number;
  char printable;          // N Y
  be32_t execution_price;  // Price(4)
};

struct OrderCancel {  // 'X'
  MessageHeader hdr;
  be64_t order_reference_number;
  be32_t cancelled_shares;
};

struct OrderDelete {  // 'D'
  MessageHeader hdr;
  be64_t order_reference_number;
};

struct OrderReplace {  // 'U'
  MessageHeader hdr;
  be64_t original_order_reference_number;
  be64_t new_order_reference_number;
  be32_t shares;
  be32_t price;  // Price(4)
};

struct Trade {  // 'P'
  MessageHeader hdr;
  be64_t order_reference_number;  // zero-filled since 2010-12-06
  char buy_sell_indicator;        // always 'B' since 2014-07-14
  be32_t shares;
  char stock[8];
  be32_t price;  // Price(4)
  be64_t match_number;
};

struct CrossTrade {  // 'Q'
  MessageHeader hdr;
  be64_t shares;  // 8 bytes in this message
  char stock[8];
  be32_t cross_price;  // Price(4)
  be64_t match_number;
  char cross_type;  // O C H
};

struct BrokenTrade {  // 'B'
  MessageHeader hdr;
  be64_t match_number;
};

struct Noii {  // 'I'
  MessageHeader hdr;
  be64_t paired_shares;
  be64_t imbalance_shares;
  char imbalance_direction;
  char stock[8];
  be32_t far_price;                // Price(4)
  be32_t near_price;               // Price(4)
  be32_t current_reference_price;  // Price(4)
  char cross_type;
  char price_variation_indicator;
};

struct RetailInterest {  // 'N'
  MessageHeader hdr;
  char stock[8];
  char interest_flag;  // B S A N
};

struct DlcrPriceDiscovery {  // 'O'
  MessageHeader hdr;
  char stock[8];
  char open_eligibility_status;
  be32_t minimum_allowable_price;  // Price(4)
  be32_t maximum_allowable_price;  // Price(4)
  be32_t near_execution_price;     // Price(4)
  be64_t near_execution_time;
  be32_t lower_price_range_collar;  // Price(4)
  be32_t upper_price_range_collar;  // Price(4)
};

#pragma pack(pop)

// Lengths from the spec tables (offset of the last field + its length).
static_assert(sizeof(MessageHeader) == 11);
static_assert(sizeof(SystemEvent) == 12);
static_assert(sizeof(StockDirectory) == 39);
static_assert(sizeof(StockTradingAction) == 25);
static_assert(sizeof(RegShoRestriction) == 20);
static_assert(sizeof(MarketParticipantPosition) == 26);
static_assert(sizeof(MwcbDeclineLevel) == 35);
static_assert(sizeof(MwcbStatus) == 12);
static_assert(sizeof(IpoQuotingPeriodUpdate) == 28);
static_assert(sizeof(LuldAuctionCollar) == 35);
static_assert(sizeof(OperationalHalt) == 21);
static_assert(sizeof(AddOrder) == 36);
static_assert(sizeof(AddOrderMpid) == 40);
static_assert(sizeof(OrderExecuted) == 31);
static_assert(sizeof(OrderExecutedWithPrice) == 36);
static_assert(sizeof(OrderCancel) == 23);
static_assert(sizeof(OrderDelete) == 19);
static_assert(sizeof(OrderReplace) == 35);
static_assert(sizeof(Trade) == 44);
static_assert(sizeof(CrossTrade) == 40);
static_assert(sizeof(BrokenTrade) == 19);
static_assert(sizeof(Noii) == 50);
static_assert(sizeof(RetailInterest) == 20);
static_assert(sizeof(DlcrPriceDiscovery) == 48);

// Offsets from the spec tables for the fields the decoder and encoder touch.
static_assert(offsetof(MessageHeader, stock_locate) == 1 &&
              offsetof(MessageHeader, tracking_number) == 3 &&
              offsetof(MessageHeader, timestamp) == 5);
static_assert(offsetof(StockDirectory, stock) == 11 &&
              offsetof(StockDirectory, round_lot_size) == 21 &&
              offsetof(StockDirectory, etp_leverage_factor) == 34 &&
              offsetof(StockDirectory, inverse_indicator) == 38);
static_assert(offsetof(StockTradingAction, reason) == 21);
static_assert(offsetof(MarketParticipantPosition, stock) == 15 &&
              offsetof(MarketParticipantPosition, market_participant_state) == 25);
static_assert(offsetof(AddOrder, order_reference_number) == 11 &&
              offsetof(AddOrder, buy_sell_indicator) == 19 && offsetof(AddOrder, shares) == 20 &&
              offsetof(AddOrder, stock) == 24 && offsetof(AddOrder, price) == 32);
static_assert(offsetof(AddOrderMpid, attribution) == 36);
static_assert(offsetof(OrderExecuted, executed_shares) == 19 &&
              offsetof(OrderExecuted, match_number) == 23);
static_assert(offsetof(OrderExecutedWithPrice, printable) == 31 &&
              offsetof(OrderExecutedWithPrice, execution_price) == 32);
static_assert(offsetof(OrderCancel, cancelled_shares) == 19);
static_assert(offsetof(OrderReplace, new_order_reference_number) == 19 &&
              offsetof(OrderReplace, shares) == 27 && offsetof(OrderReplace, price) == 31);
static_assert(offsetof(Trade, buy_sell_indicator) == 19 && offsetof(Trade, shares) == 20 &&
              offsetof(Trade, price) == 32 && offsetof(Trade, match_number) == 36);
static_assert(offsetof(CrossTrade, stock) == 19 && offsetof(CrossTrade, cross_price) == 27 &&
              offsetof(CrossTrade, match_number) == 31 && offsetof(CrossTrade, cross_type) == 39);
static_assert(offsetof(Noii, imbalance_direction) == 27 && offsetof(Noii, stock) == 28 &&
              offsetof(Noii, current_reference_price) == 44 && offsetof(Noii, cross_type) == 48);
static_assert(offsetof(MwcbDeclineLevel, level2) == 19 && offsetof(MwcbDeclineLevel, level3) == 27);
static_assert(offsetof(IpoQuotingPeriodUpdate, ipo_quotation_release_qualifier) == 23 &&
              offsetof(IpoQuotingPeriodUpdate, ipo_price) == 24);
static_assert(offsetof(LuldAuctionCollar, lower_auction_collar_price) == 27 &&
              offsetof(LuldAuctionCollar, auction_collar_extension) == 31);
static_assert(offsetof(OperationalHalt, operational_halt_action) == 20);
static_assert(offsetof(DlcrPriceDiscovery, minimum_allowable_price) == 20 &&
              offsetof(DlcrPriceDiscovery, near_execution_time) == 32 &&
              offsetof(DlcrPriceDiscovery, upper_price_range_collar) == 44);

// Wire length by message type byte; 0 for bytes that are not an ITCH 5.0 message type.
[[nodiscard]] constexpr std::size_t message_length(char type) noexcept {
  switch (type) {
    case 'S':
      return sizeof(SystemEvent);
    case 'R':
      return sizeof(StockDirectory);
    case 'H':
      return sizeof(StockTradingAction);
    case 'Y':
      return sizeof(RegShoRestriction);
    case 'L':
      return sizeof(MarketParticipantPosition);
    case 'V':
      return sizeof(MwcbDeclineLevel);
    case 'W':
      return sizeof(MwcbStatus);
    case 'K':
      return sizeof(IpoQuotingPeriodUpdate);
    case 'J':
      return sizeof(LuldAuctionCollar);
    case 'h':
      return sizeof(OperationalHalt);
    case 'A':
      return sizeof(AddOrder);
    case 'F':
      return sizeof(AddOrderMpid);
    case 'E':
      return sizeof(OrderExecuted);
    case 'C':
      return sizeof(OrderExecutedWithPrice);
    case 'X':
      return sizeof(OrderCancel);
    case 'D':
      return sizeof(OrderDelete);
    case 'U':
      return sizeof(OrderReplace);
    case 'P':
      return sizeof(Trade);
    case 'Q':
      return sizeof(CrossTrade);
    case 'B':
      return sizeof(BrokenTrade);
    case 'I':
      return sizeof(Noii);
    case 'N':
      return sizeof(RetailInterest);
    case 'O':
      return sizeof(DlcrPriceDiscovery);
    default:
      return 0;
  }
}

// All 23 ITCH 5.0 message type bytes.
inline constexpr std::array<char, 23> kAllMessageTypes = {'S', 'R', 'H', 'Y', 'L', 'V', 'W', 'K',
                                                          'J', 'h', 'A', 'F', 'E', 'C', 'X', 'D',
                                                          'U', 'P', 'Q', 'B', 'I', 'N', 'O'};

// Zero-copy view of a wire message as its packed struct. The caller checks the length first.
template <class M>
[[nodiscard]] inline const M& view_as(const std::byte* p) noexcept {
  return *reinterpret_cast<const M*>(p);
}

}  // namespace fastmm::codecs::itch
