#pragma once
// Venue error classification shared by every connector's error map: the engine-facing
// RejectReason plus what the connector itself must do (cool down, resync the clock,
// reconcile, stop).
#include "fastmm/core/enums.hpp"

#include <cstdint>
#include <string_view>

namespace fastmm::venues {

// What the connector does beyond reporting the reject to the engine.
enum class VenueAction : std::uint8_t {
  None = 0,
  Backoff = 1,            // transient venue-side failure: retry later
  RateLimit = 2,          // enter RateLimiter cooldown
  ResyncClock = 3,        // timestamp outside the window: fetch server time, recompute offset
  Reconcile = 4,          // unknown order / status unknown: request open orders
  DisableInstrument = 5,  // precision/filter failure: config is wrong for this symbol
  HardStop = 6,           // IP ban: stop REST
  Fatal = 7,              // bad key/signature/permissions: cannot continue
};
[[nodiscard]] constexpr std::string_view to_string(VenueAction a) noexcept {
  switch (a) {
    case VenueAction::None:
      return "None";
    case VenueAction::Backoff:
      return "Backoff";
    case VenueAction::RateLimit:
      return "RateLimit";
    case VenueAction::ResyncClock:
      return "ResyncClock";
    case VenueAction::Reconcile:
      return "Reconcile";
    case VenueAction::DisableInstrument:
      return "DisableInstrument";
    case VenueAction::HardStop:
      return "HardStop";
    case VenueAction::Fatal:
      return "Fatal";
  }
  return "?";
}

struct ErrorMapping {
  RejectReason reason = RejectReason::VenueReject;
  VenueAction action = VenueAction::None;
  bool known = false;  // false: code not in the table (still mapped to VenueReject)
};

// Case-insensitive substring search (venue error messages vary in case).
[[nodiscard]] constexpr bool contains_ci(std::string_view hay, std::string_view needle) noexcept {
  if (needle.size() > hay.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      char a = hay[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
      if (a != b) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

}  // namespace fastmm::venues
