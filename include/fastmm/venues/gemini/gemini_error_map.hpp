#pragma once
// Gemini errors -> (RejectReason, VenueAction). Sources, read 2026-09-30:
//   REST reasons  https://developer.gemini.com/error-codes.md: a failed request is a non-200 status
//                 with {"result":"error","reason":R,"message":M}.
//   WebSocket     https://developer.gemini.com/websocket/message-format.md: {"id","status",
//                 "error":{"code":C,"msg":M}}; -1000 internal (500), -1002 authentication required
//                 (401), -1003 rate limit (429), -1013 invalid parameters, -1020 unsupported,
//                 -2010 order rejected (400).
//   Order events  https://developer.gemini.com/websocket/streams.md: `r` of a REJECTED orderUpdate
//                 (MarketNotOpen, InsufficientFunds, InvalidPrice, LimitPriceOffTick,
//                 InvalidQuantity, InvalidStopPrice, InvalidTotalSpend, DuplicateOrder,
//                 InsufficientLiquidity, UnknownInstrument) and of a CANCELED one
//                 (SelfCrossPrevented, FillOrKillWouldNotFill, ImmediateOrCancelWouldPost,
//                 MakerOrCancelWouldTake, AuctionCancelled, ExceedsPriceLimits).
// The spec's 429 example body says "Too Many Requests", not RateLimit: the HTTP status decides.
#include "fastmm/venues/error_action.hpp"

#include <string_view>

namespace fastmm::venues::gemini {

// A REST `reason` or an order event's rejection reason.
[[nodiscard]] constexpr ErrorMapping map_reason(std::string_view r) noexcept {
  if (r.empty()) return {RejectReason::VenueReject, VenueAction::None, false};
  if (r == "RateLimit" || r == "RateLimited" || r == "Too Many Requests")
    return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  if (r == "InvalidNonce") return {RejectReason::VenueReject, VenueAction::ResyncClock, true};
  if (r == "InvalidSignature" || r == "InvalidApiKey" || r == "MissingRole" ||
      r == "MissingApikeyHeader" || r == "MissingPayloadHeader" || r == "MissingSignatureHeader" ||
      r == "AmbiguousAuthentication" || r == "NotGroupApiCompatible" || r == "AccountClosed" ||
      r == "EndpointMismatch" || r == "AccountNotOfTypeRequired")
    return {RejectReason::VenueReject, VenueAction::Fatal, true};
  if (r == "RemoteAddressForbidden" || r == "ApiKeyIpFilteringFailure")
    return {RejectReason::VenueReject, VenueAction::HardStop, true};
  if (r == "System" || r == "Maintenance")
    return {RejectReason::VenueReject, VenueAction::Backoff, true};
  if (r == "InsufficientFunds") return {RejectReason::InsufficientBalance, VenueAction::None, true};
  if (r == "InvalidPrice" || r == "LimitPriceOffTick")
    return {RejectReason::InvalidTick, VenueAction::None, true};
  if (r == "InvalidQuantity") return {RejectReason::InvalidLot, VenueAction::None, true};
  if (r == "DuplicateOrder") return {RejectReason::DuplicateId, VenueAction::None, true};
  if (r == "ExceedsPriceLimits") return {RejectReason::PriceCollar, VenueAction::None, true};
  if (r == "MakerOrCancelWouldTake")
    return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
  if (r == "MarketNotOpen" || r == "UnknownInstrument" || r == "InvalidSymbol")
    return {RejectReason::InstrumentDisabled, VenueAction::None, true};
  if (r == "OrderNotFound") return {RejectReason::VenueUnknownOrder, VenueAction::Reconcile, true};
  return {RejectReason::VenueReject, VenueAction::None, false};
}

// The WebSocket's error.code.
[[nodiscard]] constexpr ErrorMapping map_ws_code(int code) noexcept {
  switch (code) {
    case -1003:
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case -1002:
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case -1000:
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case -1013:
    case -1020:
    case -2010:
      return {RejectReason::VenueReject, VenueAction::None, true};
    default:
      return {RejectReason::VenueReject, VenueAction::None, false};
  }
}

// An order the venue ended for its own terms rather than on request: OrderExpired.
[[nodiscard]] constexpr bool is_expiry_reason(std::string_view r) noexcept {
  return r == "MakerOrCancelWouldTake" || r == "ImmediateOrCancelWouldPost" ||
         r == "FillOrKillWouldNotFill" || r == "SelfCrossPrevented" || r == "ExceedsPriceLimits" ||
         r == "AuctionCancelled";
}

[[nodiscard]] constexpr ErrorMapping map_http_status(int status) noexcept {
  switch (status) {
    case 429:
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 401:
    case 403:
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 406:
      return {RejectReason::InsufficientBalance, VenueAction::None, true};
    default:
      if (status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
      return {RejectReason::VenueReject, VenueAction::None, status >= 400};
  }
}

}  // namespace fastmm::venues::gemini
