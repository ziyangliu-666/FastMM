#pragma once
// Binance error code -> (RejectReason, VenueAction) (6.4). Codes and messages from
// https://github.com/binance/binance-spot-api-docs/blob/master/errors.md. -1010/-2010/-2011
// carry the real cause in `msg` ("Messages for -1010 ERROR_MSG_RECEIVED, -2010
// NEW_ORDER_REJECTED, -2011 CANCEL_REJECTED" and "Filter failures" tables), so the message
// is inspected for those codes. HTTP 429/418 (rest-api.md "IP Limits") are mapped through
// map_http_status().
#include "fastmm/venues/error_action.hpp"

#include <cstdint>
#include <string_view>

namespace fastmm::venues {

namespace binance {

// errors.md "Filter failures" table -> engine reasons.
[[nodiscard]] constexpr ErrorMapping map_filter_failure(std::string_view msg) noexcept {
  if (contains_ci(msg, "PRICE_FILTER") || contains_ci(msg, "PERCENT_PRICE"))
    return {RejectReason::InvalidTick, VenueAction::DisableInstrument, true};
  if (contains_ci(msg, "MARKET_LOT_SIZE") || contains_ci(msg, "LOT_SIZE"))
    return {RejectReason::InvalidLot, VenueAction::DisableInstrument, true};
  if (contains_ci(msg, "MIN_NOTIONAL") || contains_ci(msg, "NOTIONAL"))
    return {RejectReason::BelowMinNotional, VenueAction::DisableInstrument, true};
  if (contains_ci(msg, "MAX_NUM_ORDERS") || contains_ci(msg, "EXCHANGE_MAX_NUM_ORDERS"))
    return {RejectReason::MaxOpenOrders, VenueAction::None, true};
  if (contains_ci(msg, "MAX_POSITION")) return {RejectReason::MaxPosition, VenueAction::None, true};
  return {RejectReason::VenueReject, VenueAction::None, true};
}

// errors.md "Messages for -1010 ERROR_MSG_RECEIVED, -2010 NEW_ORDER_REJECTED, -2011
// CANCEL_REJECTED".
[[nodiscard]] constexpr ErrorMapping map_engine_message(std::string_view msg) noexcept {
  if (contains_ci(msg, "Filter failure")) return map_filter_failure(msg);
  if (contains_ci(msg, "Order would immediately match and take"))
    return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
  if (contains_ci(msg, "insufficient balance"))
    return {RejectReason::InsufficientBalance, VenueAction::None, true};
  if (contains_ci(msg, "Duplicate order sent"))
    return {RejectReason::DuplicateId, VenueAction::Reconcile, true};
  if (contains_ci(msg, "Unknown order sent"))
    return {RejectReason::VenueUnknownOrder, VenueAction::Reconcile, true};
  if (contains_ci(msg, "Too many new orders") || contains_ci(msg, "Too many"))
    return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  if (contains_ci(msg, "Market is closed") || contains_ci(msg, "not currently trading") ||
      contains_ci(msg, "not supported"))
    return {RejectReason::InstrumentDisabled, VenueAction::None, true};
  return {RejectReason::VenueReject, VenueAction::None, true};
}

[[nodiscard]] constexpr ErrorMapping map_error(int code, std::string_view msg = {}) noexcept {
  switch (code) {
    // 10xx general server / network
    case -1000:  // UNKNOWN
    case -1001:  // DISCONNECTED
    case -1008:  // SERVER_BUSY
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case -1002:  // UNAUTHORIZED
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case -1003:  // TOO_MANY_REQUESTS
    case -1015:  // TOO_MANY_ORDERS
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case -1006:  // UNEXPECTED_RESP: execution status unknown
    case -1007:  // TIMEOUT: send status unknown
      return {RejectReason::VenueReject, VenueAction::Reconcile, true};
    case -1013:  // INVALID_MESSAGE (filter failures on REST)
      return map_filter_failure(msg);
    case -1021:  // INVALID_TIMESTAMP
      return {RejectReason::VenueReject, VenueAction::ResyncClock, true};
    case -1022:  // INVALID_SIGNATURE
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    // 11xx request issues
    case -1100:  // ILLEGAL_CHARS
    case -1102:  // MANDATORY_PARAM_EMPTY_OR_MALFORMED
    case -1121:  // BAD_SYMBOL
      return {RejectReason::VenueReject, VenueAction::None, true};
    case -1111:  // BAD_PRECISION
      return {RejectReason::InvalidTick, VenueAction::DisableInstrument, true};
    // 20xx processing issues
    case -1010:  // ERROR_MSG_RECEIVED
    case -2010:  // NEW_ORDER_REJECTED
      return map_engine_message(msg);
    case -2011: {  // CANCEL_REJECTED
      ErrorMapping m = map_engine_message(msg);
      if (m.reason == RejectReason::VenueReject) m.action = VenueAction::Reconcile;
      return m;
    }
    case -2013:  // NO_SUCH_ORDER
      return {RejectReason::VenueUnknownOrder, VenueAction::Reconcile, true};
    case -2014:  // BAD_API_KEY_FMT
    case -2015:  // REJECTED_MBX_KEY
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case -2021:  // cancel-replace partially failed: one leg is in an unknown state
      return {RejectReason::VenueReject, VenueAction::Reconcile, true};
    case -2022:  // cancel-replace failed (both legs)
      return map_engine_message(msg);
    case -2026:  // ORDER_ARCHIVED
      return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
    default:
      return {RejectReason::VenueReject, VenueAction::None, false};
  }
}

// HTTP-level statuses (REST responses and WS API `status`).
[[nodiscard]] constexpr ErrorMapping map_http_status(int status) noexcept {
  switch (status) {
    case 418:  // IP auto-banned
      return {RejectReason::VenueRateLimit, VenueAction::HardStop, true};
    case 429:  // rate limit; honour Retry-After
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 401:
    case 403:
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 409:  // cancel-replace partial failure
      return {RejectReason::VenueReject, VenueAction::Reconcile, true};
    default:
      if (status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
      return {RejectReason::VenueReject, VenueAction::None, status >= 400};
  }
}

}  // namespace binance
}  // namespace fastmm::venues
