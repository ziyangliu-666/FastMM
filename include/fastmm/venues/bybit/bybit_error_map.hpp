#pragma once
// Bybit v5 retCode -> (RejectReason, VenueAction) (6.5). Codes and texts from
// https://bybit-exchange.github.io/docs/v5/error ("UTA" and "Spot Trade" tables) and the
// rejectReason enum from https://bybit-exchange.github.io/docs/v5/enum.
#include "fastmm/venues/error_action.hpp"

#include <string_view>

namespace fastmm::venues::bybit {

[[nodiscard]] constexpr ErrorMapping map_error(int code, std::string_view msg = {}) noexcept {
  switch (code) {
    case 0:
      return {RejectReason::None, VenueAction::None, true};
    case 10001:  // Request parameter error
      return {RejectReason::VenueReject, VenueAction::None, true};
    case 10002:  // The request time exceeds the time window range.
      return {RejectReason::VenueReject, VenueAction::ResyncClock, true};
    case 10003:  // API key is invalid
    case 10004:  // Error sign
    case 10005:  // Permission denied
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 10006:  // Too many visits. Exceeded the API Rate Limit.
    case 10018:  // Exceeded the IP Rate Limit.
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 10016:  // Server error.
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case 110001:  // Order does not exist
    case 170213:  // Order does not exist.
      return {RejectReason::VenueUnknownOrder, VenueAction::Reconcile, true};
    case 110007:  // Available balance is insufficient
    case 170131:  // Balance insufficient
      return {RejectReason::InsufficientBalance, VenueAction::None, true};
    case 110079:  // The order is processing and can not be operated
      return {RejectReason::VenueReject, VenueAction::Reconcile, true};
    case 170121:  // Invalid symbol.
      return {RejectReason::InstrumentDisabled, VenueAction::DisableInstrument, true};
    case 170135:  // Order quantity lower than the minimum.
    case 170136:
    case 170137:  // Order volume decimal too long
    case 110017:  // orderQty will be truncated to zero
      return {RejectReason::InvalidLot, VenueAction::DisableInstrument, true};
    case 170124:  // Order amount too large.
    case 170341:  // Request order quantity exceeds maximum limit
      return {RejectReason::MaxOrderQty, VenueAction::None, true};
    case 110003:  // Order price exceeds the allowable range.
    case 170193:  // Buy order price cannot be higher than %s.
    case 170194:  // Sell order price cannot be lower than %s.
      return {RejectReason::PriceCollar, VenueAction::None, true};
    case 170218:  // The LIMIT-MAKER order is rejected due to invalid price.
      return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
    case 170140:  // Order has been cancelled
      return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
    case 170130:  // Data sent for parameter '%s' is not valid.
    case 170210:  // New order rejected.
      if (contains_ci(msg, "price")) return {RejectReason::InvalidTick, VenueAction::None, true};
      return {RejectReason::VenueReject, VenueAction::None, true};
    default:
      if (code >= 170000 && code < 180000)  // other spot trade rejects
        return {RejectReason::VenueReject, VenueAction::None, false};
      return {RejectReason::VenueReject, VenueAction::None, false};
  }
}

// rejectReason on the private `order` topic (enum page, "rejectReason").
[[nodiscard]] constexpr RejectReason map_reject_reason(std::string_view r) noexcept {
  if (r == "EC_PostOnlyWillTakeLiquidity") return RejectReason::PostOnlyWouldCross;
  if (r == "EC_InvalidSymbolStatus") return RejectReason::InstrumentDisabled;
  // VERIFY: the balance-related rejectReason strings are not listed on the enum page.
  if (contains_ci(r, "Balance")) return RejectReason::InsufficientBalance;
  return RejectReason::VenueReject;
}

// HTTP-level statuses. 403 = IP rate limit breached (error page, "HTTP Status Codes"); the
// ban lifts after ~10 minutes (docs/v5/rate-limit), BybitVenue cools down accordingly.
[[nodiscard]] constexpr ErrorMapping map_http_status(int status) noexcept {
  switch (status) {
    case 403:
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 401:
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    default:
      if (status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
      return {RejectReason::VenueReject, VenueAction::None, status >= 400};
  }
}

}  // namespace fastmm::venues::bybit
