#pragma once
// Gate APIv4 error labels -> (RejectReason, VenueAction). Gate reports an error as an HTTP status
// (REST) or header.status (WebSocket API) with a body {"label":..,"message":..}; the label is the
// stable key. Labels from https://www.gate.com/docs/developers/apiv4/en/#label-list
// ("Authentication related", "Futures related", "Server errors", read 2026-10-03) and the WebSocket
// API's rate-limit error (TOO_MANY_REQUESTS, codes 311/312 in the message).
#include "fastmm/venues/error_action.hpp"

#include <string_view>

namespace fastmm::venues::gate {

[[nodiscard]] constexpr ErrorMapping map_label(std::string_view label,
                                               std::string_view msg = {}) noexcept {
  if (label.empty()) return {RejectReason::None, VenueAction::None, true};
  // Authentication related: nothing a retry fixes.
  if (label == "INVALID_CREDENTIALS" || label == "INVALID_KEY" || label == "IP_FORBIDDEN" ||
      label == "READ_ONLY" || label == "INVALID_SIGNATURE" || label == "MISSING_REQUIRED_HEADER" ||
      label == "ACCOUNT_LOCKED" || label == "FORBIDDEN" || label == "USER_NOT_FOUND")
    return {RejectReason::VenueReject, VenueAction::Fatal, true};
  if (label == "REQUEST_EXPIRED")
    return {RejectReason::VenueReject, VenueAction::ResyncClock, true};
  if (label == "TOO_MANY_REQUESTS" || label == "TOO_BUSY")
    return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  if (label == "INTERNAL" || label == "SERVER_ERROR" || label == "INTERNAL_SERVER_ERROR" ||
      label == "SERVER_TIMEOUT")
    return {RejectReason::VenueReject, VenueAction::Backoff, true};
  // Futures related.
  if (label == "ORDER_NOT_FOUND" || label == "ORDER_NOT_OWNED")
    return {RejectReason::VenueUnknownOrder, VenueAction::Reconcile, true};
  if (label == "ORDER_FINISHED" || label == "ORDER_CLOSED" || label == "ORDER_CANCELLED")
    return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
  if (label == "INSUFFICIENT_AVAILABLE" || label == "BALANCE_NOT_ENOUGH" ||
      label == "FUTURES_BALANCE_NOT_ENOUGH" || label == "MARGIN_BALANCE_NOT_ENOUGH")
    return {RejectReason::InsufficientBalance, VenueAction::None, true};
  if (label == "ORDER_POC_IMMEDIATE" || label == "POC_FILL_IMMEDIATELY")
    return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
  if (label == "CONTRACT_NOT_FOUND" || label == "CONTRACT_IN_DELISTING" ||
      label == "INVALID_CURRENCY_PAIR")
    return {RejectReason::InstrumentDisabled, VenueAction::DisableInstrument, true};
  if (label == "SIZE_TOO_SMALL" || label == "AMOUNT_TOO_LITTLE" || label == "QUANTITY_NOT_ENOUGH")
    return {RejectReason::InvalidLot, VenueAction::None, true};
  if (label == "SIZE_TOO_LARGE" || label == "AMOUNT_TOO_MUCH")
    return {RejectReason::MaxOrderQty, VenueAction::None, true};
  if (label == "PRICE_TOO_DEVIATED" || label == "PRICE_OVER_LIQUIDATION" ||
      label == "PRICE_OVER_BANKRUPT")
    return {RejectReason::PriceCollar, VenueAction::None, true};
  if (label == "INVALID_PRECISION") {
    if (contains_ci(msg, "price")) return {RejectReason::InvalidTick, VenueAction::None, true};
    return {RejectReason::InvalidLot, VenueAction::None, true};
  }
  if (label == "TOO_MANY_ORDERS" || label == "RISK_LIMIT_EXCEEDED" ||
      label == "LIQUIDATE_IMMEDIATELY" || label == "INCREASE_POSITION" ||
      label == "REDUCE_EXCEEDED" || label == "POSITION_IN_LIQUIDATION" ||
      label == "POSITION_IN_CLOSE" || label == "POSITION_EMPTY" || label == "ORDER_FOK" ||
      label == "AMEND_WITH_STOP" || label == "NO_CHANGE" || label == "ORDER_PENDING" ||
      label == "POSITION_HOLDING")
    return {RejectReason::VenueReject, VenueAction::None, true};
  if (label == "POSITION_DUAL_MODE")  // the account was switched to dual mode under us
    return {RejectReason::VenueReject, VenueAction::Fatal, true};
  if (label == "DUPLICATE_REQUEST" || label == "ORDER_EXISTS" || label == "REPEATED_CREATION")
    return {RejectReason::DuplicateId, VenueAction::None, true};
  if (label == "INVALID_CLIENT_ORDER_ID" || label == "INVALID_PARAM_VALUE" ||
      label == "MISSING_REQUIRED_PARAM" || label == "INVALID_ARGUMENT")
    return {RejectReason::VenueReject, VenueAction::None, true};
  return {RejectReason::VenueReject, VenueAction::None, false};
}

// HTTP-level statuses (and header.status of a WebSocket API reply) when no label is readable.
[[nodiscard]] constexpr ErrorMapping map_http_status(int status) noexcept {
  switch (status) {
    case 429:
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 401:
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 403:
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    default:
      if (status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
      return {RejectReason::VenueReject, VenueAction::None, status >= 400};
  }
}

// `finish_as` of a finished order on the futures.orders channel that is not a fill or a plain
// cancel: the order ended on the venue's own initiative, like an expired one.
[[nodiscard]] constexpr bool finish_as_expired(std::string_view finish_as) noexcept {
  return finish_as == "ioc" || finish_as == "stp" || finish_as == "reduce_only" ||
         finish_as == "position_closed" || finish_as == "reduce_out" || finish_as == "liquidated" ||
         finish_as == "auto_deleveraged";
}

}  // namespace fastmm::venues::gate
