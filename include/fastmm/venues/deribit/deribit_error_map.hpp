#pragma once
// Deribit JSON-RPC error code -> (RejectReason, VenueAction).
//
// Codes and short messages come from the "Complete RPC Error Codes Reference" table of
// https://docs.deribit.com/articles/errors (checked 2026-09-14). The same page opens with a
// "Common Error Codes" summary whose numbers contradict that table for several conditions (it
// lists 11006 post_only_reject, 10002 insufficient_funds, 10028... where the complete table has
// 11054, 10009 and different meanings for 10002-10009); the testnet answered 13009 "unauthorized"
// and 13004 "invalid_credentials" exactly as the complete table says
// (tests/fixtures/deribit/rpc_*.json), so the complete table is authoritative here and the error
// message text is used as a fallback for the conditions the engine cares about.
//
// A 13009 (unauthorized: expired or invalid token) or 10000 (authorization_required) additionally
// makes DeribitVenue re-authenticate the private connection (needs_reauth()).
#include "fastmm/venues/error_action.hpp"

#include <string_view>

namespace fastmm::venues::deribit {

[[nodiscard]] constexpr bool needs_reauth(int code) noexcept {
  return code == 13009 || code == 10000;
}

[[nodiscard]] constexpr ErrorMapping map_error(int code, std::string_view msg = {}) noexcept {
  switch (code) {
    case 0:
      return {RejectReason::None, VenueAction::None, true};
    case 10000:  // authorization_required
    case 13009:  // unauthorized (wrong or expired token)
      return {RejectReason::VenueReject, VenueAction::None, true};
    case 13004:  // invalid_credentials
    case 13021:  // forbidden
    case 11042:  // permission_denied
    case 13403:  // scope_exceeded
      return {RejectReason::VenueReject, VenueAction::Fatal, true};
    case 10002:  // qty_too_low
      return {RejectReason::InvalidLot, VenueAction::None, true};
    case 10021:  // invalid_amount
    case 10027:  // non_integer_contract_amount
      return {RejectReason::InvalidLot, VenueAction::DisableInstrument, true};
    case 10003:  // order_overlap (self-trading not enabled)
      return {RejectReason::SelfTradePrevention, VenueAction::None, true};
    case 10004:  // order_not_found
      return {RejectReason::VenueUnknownOrder, VenueAction::Reconcile, true};
    case 10010:  // already_closed
    case 10029:  // not_owner_of_order
    case 11008:  // already_filled
    case 11044:  // not_open_order
      return {RejectReason::VenueUnknownOrder, VenueAction::None, true};
    case 10005:  // price_too_low <Limit>
    case 10006:  // price_too_low4idx <Limit>
    case 10007:  // price_too_high <Limit>
    case 10011:  // price_not_allowed
      return {RejectReason::PriceCollar, VenueAction::None, true};
    case 10009:  // not_enough_funds
    case 10039:  // not_enough_funds_in_currency <Currency>
      return {RejectReason::InsufficientBalance, VenueAction::None, true};
    case 10012:  // book_closed
    case 10020:  // invalid_or_unsupported_instrument
    case 13019:  // orderbook_closed
    case 13020:  // not_found (instrument)
      return {RejectReason::InstrumentDisabled, VenueAction::DisableInstrument, true};
    case 10023:  // invalid_price
    case 10026:  // price_precision_exceeded
    case 10043:  // price_wrong_tick
      return {RejectReason::InvalidTick, VenueAction::None, true};
    case 10028:  // too_many_requests (the venue also terminates the session)
      return {RejectReason::VenueRateLimit, VenueAction::RateLimit, true};
    case 10019:  // locked_by_admin
    case 10040:  // retry
    case 10041:  // settlement_in_progress
    case 10047:  // matching_engine_queue_full
    case 10066:  // too_many_concurrent_requests
    case 11051:  // system_maintenance
    case 11094:  // internal_server_error
    case 13028:  // temporarily_unavailable
    case 13030:  // mmp_trigger
    case 13503:  // unavailable
      return {RejectReason::VenueReject, VenueAction::Backoff, true};
    case 11054:  // post_only_reject (reject_post_only set)
      return {RejectReason::PostOnlyWouldCross, VenueAction::None, true};
    case 13888:  // timed_out (valid_until passed): the order state is unknown
      return {RejectReason::VenueReject, VenueAction::Reconcile, true};
    case 13666:   // request_cancelled_by_user
    case 13777:   // replaced (edit superseded by another edit)
    case 11029:   // invalid_arguments
    case 11043:   // bad_argument
    case 11049:   // bad_arguments
    case 11050:   // bad_request
    case -32600:  // request entity too large
    case -32601:  // Method not found
    case -32602:  // Invalid params
    case -32700:  // Parse error
    case -32000:  // Missing params
      return {RejectReason::VenueReject, VenueAction::None, true};
    default:
      if (contains_ci(msg, "post_only_reject")) {
        return {RejectReason::PostOnlyWouldCross, VenueAction::None, false};
      }
      if (contains_ci(msg, "too_many_requests")) {
        return {RejectReason::VenueRateLimit, VenueAction::RateLimit, false};
      }
      if (contains_ci(msg, "not_enough_funds")) {
        return {RejectReason::InsufficientBalance, VenueAction::None, false};
      }
      return {RejectReason::VenueReject, VenueAction::None, false};
  }
}

}  // namespace fastmm::venues::deribit
