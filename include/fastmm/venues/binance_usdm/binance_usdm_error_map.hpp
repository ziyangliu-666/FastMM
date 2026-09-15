#pragma once
// Binance USDⓈ-M futures error code -> (RejectReason, VenueAction). Codes and names from
// https://developers.binance.com/docs/derivatives/usds-margined-futures/error-code (fetched
// 2026-09-15). HTTP statuses from "General Info" -> "HTTP Return Codes": 503 with "Unknown error"
// means the execution status is unknown, so it reconciles.
#include "fastmm/venues/error_action.hpp"

#include <string_view>

namespace fastmm::venues::binance_usdm {

[[nodiscard]] constexpr ErrorMapping map_error(int code, std::string_view msg = {}) noexcept {
  switch (code) {
    // 10xx general server or network issues
    case -1000:  // UNKNOWN
    case -1001:  // DISCONNECTED
    case -1008:  // Request throttled (server overloaded / system-level protection)
    case -1016:  // SERVICE_SHUTTING_DOWN
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case -1002:  // UNAUTHORIZED
    case -1022:  // INVALID_SIGNATURE
    case -2014:  // BAD_API_KEY_FMT
    case -2015:  // REJECTED_MBX_KEY
    case -2017:  // API_KEYS_LOCKED
    case -4109:  // INACTIVE_ACCOUNT
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case -1003:  // TOO_MANY_REQUESTS
    case -1015:  // TOO_MANY_ORDERS
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case -1006:  // UNEXPECTED_RESP: execution status unknown
    case -1007:  // TIMEOUT: execution status unknown
      return {RejectReason::VenueReject, VenueAction::Reconcile, true};
    case -1021:  // INVALID_TIMESTAMP
    case -5028:  // ME_RECVWINDOW_REJECT
      return {RejectReason::VenueReject, VenueAction::ResyncClock, true};
    // 11xx request issues
    case -1100:  // ILLEGAL_CHARS
    case -1101:  // TOO_MANY_PARAMETERS
    case -1102:  // MANDATORY_PARAM_EMPTY_OR_MALFORMED
    case -1103:  // UNKNOWN_PARAM
    case -1104:  // UNREAD_PARAMETERS
    case -1105:  // PARAM_EMPTY
    case -1106:  // PARAM_NOT_REQUIRED
    case -1115:  // INVALID_TIF
    case -1116:  // INVALID_ORDER_TYPE
    case -1117:  // INVALID_SIDE
    case -1121:  // BAD_SYMBOL
    case -1130:  // INVALID_PARAMETER
    case -4015:  // INVALID_CL_ORD_ID_LEN
      return {RejectReason::VenueReject, VenueAction::None, true};
    case -1111:  // BAD_PRECISION
    case -4014:  // PRICE_NOT_INCREASED_BY_TICK_SIZE
      return {RejectReason::InvalidTick, VenueAction::DisableInstrument, true};
    case -4003:  // QTY_LESS_THAN_ZERO
    case -4004:  // QTY_LESS_THAN_MIN_QTY
    case -4005:  // QTY_GREATER_THAN_MAX_QTY
    case -4023:  // QTY_NOT_INCREASED_BY_STEP_SIZE
      return {RejectReason::InvalidLot, VenueAction::DisableInstrument, true};
    case -1122:  // INVALID_SYMBOL_STATUS
    case -4140:  // INVALID_OPENING_POSITION_STATUS
    case -4141:  // SYMBOL_ALREADY_CLOSED
    case -5024:  // MOVE_ORDER_NOT_ALLOWED_SYMBOL_REASON
      return {RejectReason::InstrumentDisabled, VenueAction::None, true};
    // 20xx processing issues
    case -2010:  // NEW_ORDER_REJECTED
      if (contains_ci(msg, "immediately match") || contains_ci(msg, "executed as maker"))
        return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
      return {RejectReason::VenueReject, VenueAction::None, true};
    case -2011:  // CANCEL_REJECTED ("Unknown order sent")
    case -2013:  // NO_SUCH_ORDER
      return {RejectReason::VenueUnknownOrder, VenueAction::Reconcile, true};
    case -2018:  // BALANCE_NOT_SUFFICIENT
    case -2019:  // MARGIN_NOT_SUFFICIENT
    case -2027:  // MAX_LEVERAGE_RATIO
    case -2028:  // MIN_LEVERAGE_RATIO
    case -4050:  // CROSS_BALANCE_INSUFFICIENT
    case -4051:  // ISOLATED_BALANCE_INSUFFICIENT
      return {RejectReason::InsufficientBalance, VenueAction::None, true};
    case -2020:  // UNABLE_TO_FILL
    case -2021:  // ORDER_WOULD_IMMEDIATELY_TRIGGER
    case -5021:  // FOK_ORDER_REJECT
    case -5027:  // SAME_ORDER (modify with no change)
    case -5043:  // Existing_Pending_Modification
    // Reduce-only rejects: the engine has no dedicated reason.
    case -2022:  // REDUCE_ONLY_REJECT
    case -2024:  // POSITION_NOT_SUFFICIENT
    case -2026:  // REDUCE_ONLY_ORDER_TYPE_NOT_SUPPORTED
    case -4062:  // REDUCE_ONLY_CONFLICT
    case -4118:  // REDUCE_ONLY_MARGIN_CHECK_FAILED
      return {RejectReason::VenueReject, VenueAction::None, true};
    case -2023:  // USER_IN_LIQUIDATION
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case -2025:  // MAX_OPEN_ORDER_EXCEEDED
      return {RejectReason::MaxOpenOrders, VenueAction::None, true};
    // 40xx filters and other issues
    case -4001:  // PRICE_LESS_THAN_ZERO
    case -4002:  // PRICE_GREATER_THAN_MAX_PRICE
    case -4013:  // PRICE_LESS_THAN_MIN_PRICE
    case -4016:  // PRICE_HIGHTER_THAN_MULTIPLIER_UP
    case -4024:  // PRICE_LOWER_THAN_MULTIPLIER_DOWN
    case -4131:  // MARKET_ORDER_REJECT (PERCENT_PRICE)
      return {RejectReason::PriceCollar, VenueAction::None, true};
    case -4164:  // MIN_NOTIONAL
    case -5029:  // MODIFICATION_MIN_NOTIONAL
      return {RejectReason::BelowMinNotional, VenueAction::None, true};
    case -4060:  // INVALID_POSITION_SIDE
    case -4061:  // POSITION_SIDE_NOT_MATCH: the account is in hedge mode
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case -4087:  // REDUCE_ONLY_ORDER_PERMISSION
    case -4088:  // NO_PLACE_ORDER_PERMISSION
    case -4105:  // SYMBOL_REDUCE_ONLY
    case -4106:  // SYMBOL_REDUCE_ONLY_BUY
    case -4107:  // SYMBOL_REDUCE_ONLY_SELL
    case -4189:  // ACCOUNT_REDUCE_ONLY
    case -4400:  // TRADING_QUANTITATIVE_RULE
    case -4401:  // LARGE_POSITION_SYM_RULE
      return {RejectReason::InstrumentDisabled, VenueAction::None, true};
    case -4116:  // DUPLICATED_CLIENT_ORDER_ID
      return {RejectReason::DuplicateId, VenueAction::Reconcile, true};
    // 50xx order execution
    case -5022:  // GTX_ORDER_REJECT: the post-only order would have taken
      return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
    case -5025:  // LIMIT_ORDER_ONLY (modify)
    case -5026:  // Exceed_Maximum_Modify_Order_Limit
      return {RejectReason::VenueReject, VenueAction::None, true};
    default:
      return {RejectReason::VenueReject, VenueAction::None, false};
  }
}

// HTTP statuses of REST responses and the WS API `status`.
[[nodiscard]] constexpr ErrorMapping map_http_status(int status) noexcept {
  switch (status) {
    case 418:  // IP auto-banned after 429s
      return {RejectReason::VenueRateLimit, VenueAction::HardStop, true};
    case 429:  // request rate limit; honour Retry-After
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 401:
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 403:  // WAF limit violated
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case 408:  // backend timeout: execution status unknown
    case 503:  // "Unknown error, please check your request or try again later": status unknown
      return {RejectReason::VenueReject, VenueAction::Reconcile, true};
    default:
      if (status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
      return {RejectReason::VenueReject, VenueAction::None, status >= 400};
  }
}

}  // namespace fastmm::venues::binance_usdm
