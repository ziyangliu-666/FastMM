#pragma once
// Hyperliquid order outcomes -> (RejectReason, VenueAction).
//
// Two sources describe the same outcome. The reply to an action carries free text per order
// ({"error":"Post only order would have immediately matched, bbo was 230.31@230.32. asset=110002"})
// or a text for the whole action ({"status":"err","response":"User or API Wallet 0x.. does not
// exist."}); the orderUpdates stream carries a status word per order (open, filled, canceled,
// badAloPxRejected, marginCanceled, ...; the "orderStatus" info docs list them, read 2026-10-12).
// The texts are matched on stable phrases, case-insensitively.
#include "fastmm/venues/error_action.hpp"

#include <string_view>

namespace fastmm::venues::hyperliquid {

// A per-order or per-action error text.
[[nodiscard]] constexpr ErrorMapping map_error_text(std::string_view text) noexcept {
  if (text.empty()) return {RejectReason::VenueReject, VenueAction::None, false};
  // The signer is not the account or an agent it approved, or the account has no deposit:
  // nothing a retry fixes.
  if (contains_ci(text, "api wallet") || contains_ci(text, "must deposit") ||
      contains_ci(text, "not approved") || contains_ci(text, "invalid signature"))
    return {RejectReason::VenueReject, VenueAction::Fatal, true};
  if (contains_ci(text, "too many") && contains_ci(text, "request"))
    return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  if (contains_ci(text, "rate limit"))
    return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
  if (contains_ci(text, "nonce")) return {RejectReason::VenueReject, VenueAction::Backoff, true};
  if (contains_ci(text, "post only"))
    return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
  if (contains_ci(text, "could not immediately match"))
    return {RejectReason::VenueReject, VenueAction::None, true};  // an IOC that found nothing
  if (contains_ci(text, "minimum value"))
    return {RejectReason::BelowMinNotional, VenueAction::None, true};
  if (contains_ci(text, "tick size") || contains_ci(text, "invalid price"))
    return {RejectReason::InvalidTick, VenueAction::None, true};
  if (contains_ci(text, "invalid size") || contains_ci(text, "zero size"))
    return {RejectReason::InvalidLot, VenueAction::None, true};
  if (contains_ci(text, "insufficient") || contains_ci(text, "margin"))
    return {RejectReason::InsufficientBalance, VenueAction::None, true};
  if (contains_ci(text, "away from the reference price") || contains_ci(text, "oracle"))
    return {RejectReason::PriceCollar, VenueAction::None, true};
  if (contains_ci(text, "never placed") || contains_ci(text, "already canceled") ||
      contains_ci(text, "canceled or filled") || contains_ci(text, "unknown oid") ||
      contains_ci(text, "order does not exist"))
    return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
  if (contains_ci(text, "reduce only") || contains_ci(text, "open interest") ||
      contains_ci(text, "max position") || contains_ci(text, "too many open orders") ||
      contains_ci(text, "max number of orders"))
    return {RejectReason::VenueReject, VenueAction::None, true};
  if (contains_ci(text, "asset") && contains_ci(text, "invalid"))
    return {RejectReason::InstrumentDisabled, VenueAction::DisableInstrument, true};
  return {RejectReason::VenueReject, VenueAction::None, false};
}

// What an orderUpdates status means for the order it names.
enum class StatusClass : std::uint8_t {
  Ignore,    // filled (the fills arrive on userFills), triggered, an unknown word
  Open,      // resting: the ack
  Canceled,  // cancelled on request: the cancel ack
  Expired,   // ended by the venue (margin, self-trade, reduce-only, a scheduled cancel, ...)
  Rejected,  // never rested
};

[[nodiscard]] constexpr StatusClass classify_status(std::string_view s) noexcept {
  if (s == "open") return StatusClass::Open;
  if (s == "canceled") return StatusClass::Canceled;
  if (s == "filled" || s == "triggered") return StatusClass::Ignore;
  if (s.ends_with("Rejected") || s == "rejected") return StatusClass::Rejected;
  if (s.ends_with("Canceled") || s == "scheduledCancel") return StatusClass::Expired;
  return StatusClass::Ignore;
}

// The reject reason of a *Rejected status.
[[nodiscard]] constexpr RejectReason rejected_reason(std::string_view s) noexcept {
  if (s == "badAloPxRejected") return RejectReason::PostOnlyWouldCross;
  if (s == "tickRejected") return RejectReason::InvalidTick;
  if (s == "minTradeNtlRejected") return RejectReason::BelowMinNotional;
  if (s == "perpMarginRejected" || s == "insufficientSpotBalanceRejected")
    return RejectReason::InsufficientBalance;
  if (s == "oracleRejected") return RejectReason::PriceCollar;
  return RejectReason::VenueReject;
}

}  // namespace fastmm::venues::hyperliquid
