#pragma once
// Coinbase Exchange errors -> (RejectReason, VenueAction). The venue has no error codes: a refused
// REST request is an HTTP status with {"message": "..."} (https://docs.cdp.coinbase.com/exchange/
// rest-api/requests, read 2026-09-30: 400 bad request, 401 invalid API key, 403 forbidden, 404 not
// found, 500 internal error; 429 is the rate limit,
// .../exchange/introduction/rate-limits-overview). The status decides the class and the message the
// detail, matched case-insensitively. The documentation lists no message texts: the ones below are
// not confirmed against the venue (no keys yet), and a message not among them keeps its status's
// class.
//
// A limit order refused for post-only is not an error: the reply is 200 with "status":"rejected"
// and "reject_reason" (map_reject_reason).
#include "fastmm/venues/error_action.hpp"

#include <string_view>

namespace fastmm::venues::coinbase {

[[nodiscard]] constexpr ErrorMapping map_error(int http_status,
                                               std::string_view msg = {}) noexcept {
  if (http_status >= 200 && http_status < 300) return {RejectReason::None, VenueAction::None, true};
  if (http_status == 429 || contains_ci(msg, "rate limit"))
    return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  if (contains_ci(msg, "timestamp"))
    return {RejectReason::VenueReject, VenueAction::ResyncClock, true};
  if (http_status == 401 || http_status == 403 || contains_ci(msg, "invalid signature") ||
      contains_ci(msg, "invalid api key") || contains_ci(msg, "invalid passphrase"))
    return {RejectReason::VenueReject, VenueAction::Fatal, true};
  if (contains_ci(msg, "insufficient funds"))
    return {RejectReason::InsufficientBalance, VenueAction::None, true};
  if (contains_ci(msg, "price is too accurate") || contains_ci(msg, "price is too small") ||
      contains_ci(msg, "price is too large"))
    return {RejectReason::InvalidTick, VenueAction::DisableInstrument, true};
  if (contains_ci(msg, "size is too accurate") || contains_ci(msg, "size is too small") ||
      contains_ci(msg, "size is too large"))
    return {RejectReason::InvalidLot, VenueAction::DisableInstrument, true};
  if (contains_ci(msg, "funds is too small") || contains_ci(msg, "min_market_funds"))
    return {RejectReason::BelowMinNotional, VenueAction::None, true};
  if (http_status == 404 || contains_ci(msg, "not found") || contains_ci(msg, "order already done"))
    return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
  if (contains_ci(msg, "post only") || contains_ci(msg, "cancel only") ||
      contains_ci(msg, "limit only") || contains_ci(msg, "trading disabled"))
    return {RejectReason::VenueReject, VenueAction::None, true};
  if (http_status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
  return {RejectReason::VenueReject, VenueAction::None, http_status == 400};
}

// "reject_reason" of an order the venue took and then refused ("status":"rejected").
[[nodiscard]] constexpr RejectReason map_reject_reason(std::string_view r) noexcept {
  if (contains_ci(r, "post only") || contains_ci(r, "post_only"))
    return RejectReason::PostOnlyWouldCross;
  if (contains_ci(r, "insufficient")) return RejectReason::InsufficientBalance;
  return RejectReason::VenueReject;
}

}  // namespace fastmm::venues::coinbase
