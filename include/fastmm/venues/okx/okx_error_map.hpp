#pragma once
// OKX v5 error code -> (RejectReason, VenueAction). Codes and texts from the "Error Code" section
// of https://www.okx.com/docs-v5/en/#error-code (REST/WebSocket public, API, trade and WebSocket
// classes; read 2026-09-26). OKX sends codes as strings: `code` at the top of a reply, `sCode` per
// order, and since 2026-03-10 a `subCode` ("51008_1000") next to it, which this map does not need.
//
// OKX documents no IP-ban code; 50121 ("You can't access OKX with this IP") is the closest, and is
// a hard stop. Account-level blocks (50007, 50009, 50027, 50029, 51024, 59113) and authentication
// failures are fatal for the venue.
#include "fastmm/venues/error_action.hpp"

#include <cstddef>
#include <string_view>

namespace fastmm::venues::okx {

[[nodiscard]] constexpr ErrorMapping map_error(int code, std::string_view msg = {}) noexcept {
  switch (code) {
    case 0:
      return {RejectReason::None, VenueAction::None, true};
    // ---- general -------------------------------------------------------------------------
    case 50001:  // Service temporarily unavailable
    case 50013:  // Systems are busy
    case 50026:  // System error
    case 64007:  // WebSocket internal error
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case 50004:  // API endpoint request timeout (does not mean that the request was successful
                 // or failed, please check the request result)
      return {RejectReason::VenueReject, VenueAction::Reconcile, true};
    case 50011:  // Rate limit reached
    case 50061:  // Sub-account order rate limit (1000 per 2 s)
    case 60014:  // Requests too frequent (WebSocket)
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 50007:  // Account blocked
    case 50009:  // Account frozen (stop-out)
    case 50027:  // The account is restricted from trading
    case 50029:  // Risk control restriction
    case 51024:  // Trading account is blocked
    case 59113:  // KYC level 2 required to trade
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 50017:  // frozen for ADL
    case 50018:
    case 50019:
    case 50020:
    case 50021:
    case 50022:  // frozen for liquidation
    case 50023:  // funding fee frozen
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case 50036:  // expTime can't be earlier than the current system time
      return {RejectReason::VenueReject, VenueAction::ResyncClock, true};
    case 50014:  // Parameter {0} can't be empty
    case 50016:  // Parameter mismatch
    case 50037:  // Order expired (expTime)
    case 50038:  // This feature is unavailable in demo trading
      return {RejectReason::VenueReject, VenueAction::None, true};
    case 50071:  // {param} already exists
    case 51016:  // Duplicated client order ID
      return {RejectReason::DuplicateId, VenueAction::None, true};
    // ---- API (authentication) ------------------------------------------------------------
    case 50102:  // Timestamp request expired
    case 50112:  // Invalid OK-ACCESS-TIMESTAMP
    case 60004:  // Invalid timestamp (WebSocket login)
    case 60006:  // Timestamp request expired (WebSocket login)
      return {RejectReason::VenueReject, VenueAction::ResyncClock, true};
    case 50100:  // API frozen
    case 50101:  // APIKey does not match current environment (demo / live)
    case 50103:  // Request header OK-ACCESS-KEY can't be empty
    case 50104:  // Request header OK-ACCESS-PASSPHRASE can't be empty
    case 50105:  // Request header OK-ACCESS-PASSPHRASE incorrect
    case 50106:  // Request header OK-ACCESS-SIGN can't be empty
    case 50107:  // Request header OK-ACCESS-TIMESTAMP can't be empty
    case 50110:  // Your IP is not in the whitelist
    case 50111:  // Invalid OK-ACCESS-KEY
    case 50113:  // Invalid signature
    case 50114:  // Invalid authorization
    case 50119:  // API key doesn't exist
    case 50120:  // API key doesn't have permission
    case 60005:  // Invalid apiKey
    case 60007:  // Invalid sign
    case 60009:  // Login failure
    case 60024:  // Wrong passphrase
    case 60032:  // API key doesn't exist
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 50121:  // You can't access OKX with this IP address
      return {RejectReason::VenueReject, VenueAction::HardStop, true};
    // ---- trade ---------------------------------------------------------------------------
    case 51000:  // Parameter {param0} error (also a posSide the position mode does not take)
      if (contains_ci(msg, "px")) return {RejectReason::InvalidTick, VenueAction::None, true};
      if (contains_ci(msg, "sz")) return {RejectReason::InvalidLot, VenueAction::None, true};
      return {RejectReason::VenueReject, VenueAction::None, true};
    case 51001:  // Instrument ID or Spread ID doesn't exist
      return {RejectReason::InstrumentDisabled, VenueAction::DisableInstrument, true};
    case 51004:  // Order failed. Order amount exceeds current tier limit
      return {RejectReason::MaxPosition, VenueAction::None, true};
    case 51006:  // Order price is not within the price limit
    case 51137:  // Your opening price has triggered the limit price
    case 51138:
      return {RejectReason::PriceCollar, VenueAction::None, true};
    case 51007:  // Order failed. Please place orders of at least 1 contract or more
    case 51020:  // Your order should meet or exceed the minimum order amount
    case 51121:  // Order quantity must be a multiple of the lot size
      return {RejectReason::InvalidLot, VenueAction::DisableInstrument, true};
    case 51008:  // Order failed. Insufficient balance / margin
      return {RejectReason::InsufficientBalance, VenueAction::None, true};
    case 51010:  // You can't complete this request under your current account mode
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 51169:  // Order failed because you don't have any positions in this direction
                 // (reduce-only)
      return {RejectReason::MaxPosition, VenueAction::None, true};
    case 51174:  // The number of orders on the instrument has reached the limit
      return {RejectReason::MaxOpenOrders, VenueAction::None, true};
    case 51400:  // Cancellation failed as the order has been filled, canceled or does not exist
    case 51503:  // Order modification failed as the order has been filled or canceled
    case 51063:  // OrdId does not exist
    case 51603:  // Order does not exist
      return {RejectReason::VenueUnknownOrder, VenueAction::Reconcile, true};
    case 51511:  // Amend failed: the price is not valid for a post-only order
      return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
    case 51513:  // Number of amend requests in progress for this order exceeds 3
      return {RejectReason::VenueRateLimit, VenueAction::None, true};
    // ---- WebSocket -----------------------------------------------------------------------
    case 60011:  // Please log in
    case 60012:  // Invalid request
    case 60013:  // Invalid args
    case 60018:  // Wrong URL or channel doesn't exist
    case 64003:  // Your trading fee tier doesn't meet the requirement to access this channel
      return {RejectReason::VenueReject, VenueAction::None, true};
    default:
      return {RejectReason::VenueReject, VenueAction::None, false};
  }
}

// The code texts OKX sends ("0", "51008"); -1 when the text is not a number.
[[nodiscard]] constexpr int parse_code(std::string_view s) noexcept {
  int v = 0;
  std::size_t i = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
    v = v * 10 + (s[i] - '0');
    if (v > 1'000'000) return -1;
  }
  return i == 0 ? -1 : v;  // "51008_1000" -> 51008
}

// HTTP statuses: 429 carries 50011/50013 in the body; a 5xx without a body is the gateway's.
[[nodiscard]] constexpr ErrorMapping map_http_status(int status) noexcept {
  switch (status) {
    case 429:
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 401:
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    default:
      if (status >= 500) return {RejectReason::VenueReject, VenueAction::Backoff, true};
      return {RejectReason::VenueReject, VenueAction::None, status >= 400};
  }
}

}  // namespace fastmm::venues::okx
