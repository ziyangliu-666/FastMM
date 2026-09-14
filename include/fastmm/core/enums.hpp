#pragma once
// All engine enumerations. Every enum is `enum class` with a fixed underlying type so it
// can be embedded in trivially copyable messages; to_string() is for logs/diagnostics.
#include <cstdint>
#include <string_view>

namespace fastmm {

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}
[[nodiscard]] constexpr int sign(Side s) noexcept {
  return s == Side::Buy ? 1 : -1;
}
[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
  return s == Side::Buy ? "Buy" : "Sell";
}

enum class OrderType : std::uint8_t { Limit = 0, Market = 1, PostOnly = 2 };
[[nodiscard]] constexpr std::string_view to_string(OrderType t) noexcept {
  switch (t) {
    case OrderType::Limit:
      return "Limit";
    case OrderType::Market:
      return "Market";
    case OrderType::PostOnly:
      return "PostOnly";
  }
  return "?";
}

enum class TimeInForce : std::uint8_t { Gtc = 0, Ioc = 1, Fok = 2, Day = 3 };
[[nodiscard]] constexpr std::string_view to_string(TimeInForce t) noexcept {
  switch (t) {
    case TimeInForce::Gtc:
      return "GTC";
    case TimeInForce::Ioc:
      return "IOC";
    case TimeInForce::Fok:
      return "FOK";
    case TimeInForce::Day:
      return "DAY";
  }
  return "?";
}

enum class AssetClass : std::uint8_t {
  Spot = 0,
  Perpetual = 1,
  Future = 2,
  Option = 3,
  Fx = 4,
  Equity = 5
};
[[nodiscard]] constexpr std::string_view to_string(AssetClass a) noexcept {
  switch (a) {
    case AssetClass::Spot:
      return "Spot";
    case AssetClass::Perpetual:
      return "Perpetual";
    case AssetClass::Future:
      return "Future";
    case AssetClass::Option:
      return "Option";
    case AssetClass::Fx:
      return "Fx";
    case AssetClass::Equity:
      return "Equity";
  }
  return "?";
}

enum class OptionType : std::uint8_t { None = 0, Call = 1, Put = 2 };

enum class OrderState : std::uint8_t {
  PendingNew = 0,
  Live = 1,
  PartiallyFilled = 2,
  PendingCancel = 3,
  PendingReplace = 4,
  Filled = 5,
  Canceled = 6,
  Rejected = 7,
  Expired = 8,
};
[[nodiscard]] constexpr bool is_terminal(OrderState s) noexcept {
  return s >= OrderState::Filled;
}
[[nodiscard]] constexpr bool is_pending(OrderState s) noexcept {
  return s == OrderState::PendingNew || s == OrderState::PendingCancel ||
         s == OrderState::PendingReplace;
}
[[nodiscard]] constexpr std::string_view to_string(OrderState s) noexcept {
  switch (s) {
    case OrderState::PendingNew:
      return "PendingNew";
    case OrderState::Live:
      return "Live";
    case OrderState::PartiallyFilled:
      return "PartiallyFilled";
    case OrderState::PendingCancel:
      return "PendingCancel";
    case OrderState::PendingReplace:
      return "PendingReplace";
    case OrderState::Filled:
      return "Filled";
    case OrderState::Canceled:
      return "Canceled";
    case OrderState::Rejected:
      return "Rejected";
    case OrderState::Expired:
      return "Expired";
  }
  return "?";
}

// Ordered exactly like RiskEngine::check_new evaluates them (5.7), followed by OMS /
// transport / venue reasons.
enum class RejectReason : std::uint8_t {
  None = 0,
  KillSwitch = 1,
  VenueKilled = 2,
  InstrumentDisabled = 3,
  InvalidTick = 4,
  InvalidLot = 5,
  BelowMinNotional = 6,
  StaleMarketData = 7,
  PriceCollar = 8,
  FatFinger = 9,
  MaxOrderQty = 10,
  MaxOrderNotional = 11,
  MaxPosition = 12,
  MaxOpenOrders = 13,
  SelfTradePrevention = 14,
  RateLimit = 15,
  MaxLoss = 16,
  // OMS / transport
  PoolExhausted = 32,
  UnknownOrder = 33,
  InvalidState = 34,
  DuplicateId = 35,
  TransportFull = 36,
  NotReconciled = 37,
  // Venue-originated
  VenueReject = 64,
  PostOnlyWouldCross = 65,
  InsufficientBalance = 66,
  VenueRateLimit = 67,
  VenueUnknownOrder = 68,
};
[[nodiscard]] constexpr std::string_view to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::None:
      return "None";
    case RejectReason::KillSwitch:
      return "KillSwitch";
    case RejectReason::VenueKilled:
      return "VenueKilled";
    case RejectReason::InstrumentDisabled:
      return "InstrumentDisabled";
    case RejectReason::InvalidTick:
      return "InvalidTick";
    case RejectReason::InvalidLot:
      return "InvalidLot";
    case RejectReason::BelowMinNotional:
      return "BelowMinNotional";
    case RejectReason::StaleMarketData:
      return "StaleMarketData";
    case RejectReason::PriceCollar:
      return "PriceCollar";
    case RejectReason::FatFinger:
      return "FatFinger";
    case RejectReason::MaxOrderQty:
      return "MaxOrderQty";
    case RejectReason::MaxOrderNotional:
      return "MaxOrderNotional";
    case RejectReason::MaxPosition:
      return "MaxPosition";
    case RejectReason::MaxOpenOrders:
      return "MaxOpenOrders";
    case RejectReason::SelfTradePrevention:
      return "SelfTradePrevention";
    case RejectReason::RateLimit:
      return "RateLimit";
    case RejectReason::MaxLoss:
      return "MaxLoss";
    case RejectReason::PoolExhausted:
      return "PoolExhausted";
    case RejectReason::UnknownOrder:
      return "UnknownOrder";
    case RejectReason::InvalidState:
      return "InvalidState";
    case RejectReason::DuplicateId:
      return "DuplicateId";
    case RejectReason::TransportFull:
      return "TransportFull";
    case RejectReason::NotReconciled:
      return "NotReconciled";
    case RejectReason::VenueReject:
      return "VenueReject";
    case RejectReason::PostOnlyWouldCross:
      return "PostOnlyWouldCross";
    case RejectReason::InsufficientBalance:
      return "InsufficientBalance";
    case RejectReason::VenueRateLimit:
      return "VenueRateLimit";
    case RejectReason::VenueUnknownOrder:
      return "VenueUnknownOrder";
  }
  return "?";
}

// Message discriminator carried in EventHeader::type. BookDelta is 1 so the dispatch
// switch's most likely case is the first compare.
enum class EventType : std::uint8_t {
  Padding = 0,  // ring wrap filler, never dispatched
  BookDelta = 1,
  BookSnapshot = 2,
  BookTicker = 3,
  Trade = 4,
  OrderAck = 5,
  OrderReject = 6,
  OrderCancelAck = 7,
  OrderCancelReject = 8,
  OrderFill = 9,
  OrderExpired = 10,
  PositionUpdate = 11,
  Timer = 12,
  Control = 13,
  ConnectionState = 14,
  Reconcile = 15,
  LatencySample = 16,
  OutNewOrder = 17,
  OutCancel = 18,
  OutReplace = 19,
  OrderAddL3 = 20,
  OrderExecL3 = 21,
  OrderCancelL3 = 22,
  OrderReplaceL3 = 23,
  OptionTicker = 24,  // mark / implied vols / greeks of one option (OptionTickerMsg)
  Count = 25,
};
[[nodiscard]] constexpr std::string_view to_string(EventType t) noexcept {
  switch (t) {
    case EventType::Padding:
      return "Padding";
    case EventType::BookDelta:
      return "BookDelta";
    case EventType::BookSnapshot:
      return "BookSnapshot";
    case EventType::BookTicker:
      return "BookTicker";
    case EventType::Trade:
      return "Trade";
    case EventType::OrderAck:
      return "OrderAck";
    case EventType::OrderReject:
      return "OrderReject";
    case EventType::OrderCancelAck:
      return "OrderCancelAck";
    case EventType::OrderCancelReject:
      return "OrderCancelReject";
    case EventType::OrderFill:
      return "OrderFill";
    case EventType::OrderExpired:
      return "OrderExpired";
    case EventType::PositionUpdate:
      return "PositionUpdate";
    case EventType::Timer:
      return "Timer";
    case EventType::Control:
      return "Control";
    case EventType::ConnectionState:
      return "ConnectionState";
    case EventType::Reconcile:
      return "Reconcile";
    case EventType::LatencySample:
      return "LatencySample";
    case EventType::OutNewOrder:
      return "OutNewOrder";
    case EventType::OutCancel:
      return "OutCancel";
    case EventType::OutReplace:
      return "OutReplace";
    case EventType::OrderAddL3:
      return "OrderAddL3";
    case EventType::OrderExecL3:
      return "OrderExecL3";
    case EventType::OrderCancelL3:
      return "OrderCancelL3";
    case EventType::OrderReplaceL3:
      return "OrderReplaceL3";
    case EventType::OptionTicker:
      return "OptionTicker";
    case EventType::Count:
      return "Count";
  }
  return "?";
}

enum class ConnState : std::uint8_t {
  Disconnected = 0,
  Connecting = 1,
  Live = 2,
  Stale = 3,
  Resyncing = 4,
  Dead = 5,
};
[[nodiscard]] constexpr std::string_view to_string(ConnState s) noexcept {
  switch (s) {
    case ConnState::Disconnected:
      return "Disconnected";
    case ConnState::Connecting:
      return "Connecting";
    case ConnState::Live:
      return "Live";
    case ConnState::Stale:
      return "Stale";
    case ConnState::Resyncing:
      return "Resyncing";
    case ConnState::Dead:
      return "Dead";
  }
  return "?";
}

// The asset a fill's commission is charged in. Binance spot charges a buy in the base asset
// (you receive less), a sell in the quote asset, and either in BNB when the discount is enabled.
enum class FeeAsset : std::uint8_t { Quote = 0, Base = 1, Other = 2 };

enum class Liquidity : std::uint8_t { Unknown = 0, Maker = 1, Taker = 2 };
[[nodiscard]] constexpr std::string_view to_string(Liquidity l) noexcept {
  switch (l) {
    case Liquidity::Unknown:
      return "Unknown";
    case Liquidity::Maker:
      return "Maker";
    case Liquidity::Taker:
      return "Taker";
  }
  return "?";
}

enum class ControlCommand : std::uint8_t {
  Stop = 0,
  PullQuotes = 1,
  ResumeQuotes = 2,
  TripKill = 3,
  ResetKill = 4,
  Reload = 5,
  FlushStats = 6,
  RecalibrateTsc = 7,
};
[[nodiscard]] constexpr std::string_view to_string(ControlCommand c) noexcept {
  switch (c) {
    case ControlCommand::Stop:
      return "Stop";
    case ControlCommand::PullQuotes:
      return "PullQuotes";
    case ControlCommand::ResumeQuotes:
      return "ResumeQuotes";
    case ControlCommand::TripKill:
      return "TripKill";
    case ControlCommand::ResetKill:
      return "ResetKill";
    case ControlCommand::Reload:
      return "Reload";
    case ControlCommand::FlushStats:
      return "FlushStats";
    case ControlCommand::RecalibrateTsc:
      return "RecalibrateTsc";
  }
  return "?";
}

enum class LogLevel : std::uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };
[[nodiscard]] constexpr std::string_view to_string(LogLevel l) noexcept {
  switch (l) {
    case LogLevel::Trace:
      return "TRACE";
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
    case LogLevel::Off:
      return "OFF";
  }
  return "?";
}

}  // namespace fastmm
