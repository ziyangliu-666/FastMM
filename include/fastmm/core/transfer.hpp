#pragma once
// An internal transfer: an amount of one asset moved between two accounts of the same exchange
// (the accounts of a pool, core/account_pool.hpp), as a connector that can do it carries it out
// (venues::Venue::transfer, VenueCaps::internal_transfer) and the pool treasury asks for it
// (core/treasury.hpp). Control path only: these types allocate.
//
// A transfer is named by `client_id`, unique per transfer and never reused: the venue is asked for
// it under that id, and its state is asked for by that id after a timeout or a restart, so a
// transfer whose answer was lost is looked up instead of sent twice.
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/strong_id.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace fastmm {

enum class TransferState : std::uint8_t {
  Pending = 0,   // the venue took it and has not finished it
  Done = 1,      // the amount has moved
  Failed = 2,    // the venue refused it or gave up: nothing moved
  NotFound = 3,  // the venue has no transfer under this id (status only)
  Unknown = 4,   // no usable answer (a transport error, a timeout, a 5xx): it may have happened
};
[[nodiscard]] constexpr std::string_view to_string(TransferState s) noexcept {
  switch (s) {
    case TransferState::Pending:
      return "pending";
    case TransferState::Done:
      return "done";
    case TransferState::Failed:
      return "failed";
    case TransferState::NotFound:
      return "not_found";
    case TransferState::Unknown:
      return "unknown";
  }
  return "?";
}

struct TransferRequest {
  std::string client_id;  // [0-9a-z], at most kMaxClientId characters
  VenueId from;           // the pool account the amount leaves
  VenueId to;             // the pool account it goes to
  // The venue's names of the two accounts (Venue::transfer_account(): a Binance sub-account's
  // email, empty for the master account). Filled in by whoever hands the request to the venue.
  std::string from_account;
  std::string to_account;
  std::string asset;
  Notional amount;
  std::int64_t created_ms = 0;  // wall clock; a venue's history query starts here

  static constexpr std::size_t kMaxClientId = 30;
};

struct TransferResult {
  TransferState state = TransferState::Unknown;
  std::string venue_ref;  // the venue's id of the transfer (Binance tranId), when it gave one
  std::string detail;     // the venue's error or a description of the failure, for the log
};

}  // namespace fastmm
