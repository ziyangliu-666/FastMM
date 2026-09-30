#pragma once
// Coinbase Advanced Trade errors -> (RejectReason, VenueAction). From the OpenAPI spec
// (https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/rest-api/advanced-trade-spec.yaml,
// read 2026-09-30):
//   * POST /orders answers 200 with "success": false and error_response {error (deprecated),
//     message, error_details, new_order_failure_reason (NewOrderFailureReason),
//     preview_failure_reason}: map_order_failure() takes the failure reason;
//   * POST /orders/batch_cancel answers per order {success, failure_reason
//     (CancelOrderFailureReason), order_id}: map_cancel_failure();
//   * any other refusal is an HTTP status with {error, code, message, details}: map_http().
#include "fastmm/venues/error_action.hpp"

#include <string_view>

namespace fastmm::venues::coinbase {

[[nodiscard]] constexpr ErrorMapping map_order_failure(std::string_view r) noexcept {
  if (r.empty() || r == "UNKNOWN_FAILURE_REASON")
    return {RejectReason::VenueReject, VenueAction::None, false};
  if (r == "INSUFFICIENT_FUND" || r == "INSUFFICIENT_FUNDS" || r == "INVALID_LEDGER_BALANCE" ||
      r == "PREVIEW_INSUFFICIENT_FUND")
    return {RejectReason::InsufficientBalance, VenueAction::None, true};
  if (r == "INVALID_LIMIT_PRICE_POST_ONLY" || r == "PREVIEW_INVALID_LIMIT_PRICE_POST_ONLY")
    return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
  if (r == "INVALID_PRICE_PRECISION" || r == "PREVIEW_INVALID_PRICE_PRECISION")
    return {RejectReason::InvalidTick, VenueAction::DisableInstrument, true};
  if (r == "INVALID_SIZE_PRECISION" || r == "PREVIEW_INVALID_SIZE_PRECISION" ||
      r == "PREVIEW_INVALID_BASE_SIZE_TOO_SMALL" || r == "PREVIEW_INVALID_BASE_SIZE_TOO_LARGE")
    return {RejectReason::InvalidLot, VenueAction::DisableInstrument, true};
  if (r == "PREVIEW_INVALID_QUOTE_SIZE_TOO_SMALL")
    return {RejectReason::BelowMinNotional, VenueAction::None, true};
  if (r == "DUPLICATE_CLIENT_ORDER_ID") return {RejectReason::DuplicateId, VenueAction::None, true};
  if (r == "ORDER_ENTRY_DISABLED" || r == "UNTRADABLE_PRODUCT" || r == "TRADING_DISABLED" ||
      r == "PRODUCT_TRADING_HALTED" || r == "INELIGIBLE_PAIR" || r == "INVALID_PRODUCT_ID")
    return {RejectReason::InstrumentDisabled, VenueAction::DisableInstrument, true};
  if (r == "GEOFENCING_RESTRICTION" || r == "PREVIEW_GEOFENCING_RESTRICTION")
    return {RejectReason::VenueReject, VenueAction::Fatal, true};
  if (r == "RFQ_RATE_LIMITED") return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  return {RejectReason::VenueReject, VenueAction::None, true};
}

[[nodiscard]] constexpr ErrorMapping map_cancel_failure(std::string_view r) noexcept {
  if (r == "UNKNOWN_CANCEL_ORDER" || r == "ORDER_IS_FULLY_FILLED")
    return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
  if (r == "DUPLICATE_CANCEL_REQUEST") return {RejectReason::VenueReject, VenueAction::None, true};
  return {RejectReason::VenueReject, VenueAction::None, !r.empty()};
}

[[nodiscard]] constexpr ErrorMapping map_http(int status, std::string_view msg = {}) noexcept {
  if (status >= 200 && status < 300) return {RejectReason::None, VenueAction::None, true};
  if (status == 429 || contains_ci(msg, "rate limit"))
    return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  if (status == 401 || status == 403) return {RejectReason::VenueReject, VenueAction::Fatal, true};
  if (status == 404) return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
  if (status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
  return {RejectReason::VenueReject, VenueAction::None, status == 400};
}

}  // namespace fastmm::venues::coinbase
