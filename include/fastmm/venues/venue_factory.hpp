#pragma once
// Venue factory: maps a [venues.<name>] config section to a concrete connector by `kind`
// (config/schema.hpp: binance_spot | binance_usdm | bybit | deribit | nasdaq_itch | sim; "sim" is
// the Binance-compatible simulator).
#include "fastmm/config/config.hpp"
#include "fastmm/venues/venue.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace fastmm::venues {

struct VenueFactoryOptions {
  bool dry_run = false;        // public market data only: no keys, no orders
  std::string record_raw_dir;  // non-empty: append raw frames to <dir>/<venue>-<channel>.jsonl
  bool busy_poll = false;      // [engine] spin_mode = "busy" (nasdaq_itch polls its sockets)
};

enum class VenueKind : std::uint8_t {
  Unknown = 0,
  BinanceSpot = 1,
  BybitSpot = 2,
  Deribit = 3,
  BinanceUsdm = 4,
  NasdaqItch = 5,
};
[[nodiscard]] VenueKind venue_kind(std::string_view kind) noexcept;

[[nodiscard]] inline bool has_credentials(const VenueSection& s) noexcept {
  return !s.api_key.empty() && !s.api_secret.empty();
}

// Throws std::invalid_argument for an unknown kind or bad URLs.
std::unique_ptr<Venue> make_venue(VenueId id,
                                  const VenueSection& section,
                                  const VenueFactoryOptions& opts);

}  // namespace fastmm::venues
