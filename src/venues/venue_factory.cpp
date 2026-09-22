#include "fastmm/venues/venue_factory.hpp"

#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"
#include "fastmm/venues/bybit/bybit_venue.hpp"
#include "fastmm/venues/deribit/deribit_venue.hpp"
#include "fastmm/venues/nasdaq/nasdaq_itch_venue.hpp"

#include <stdexcept>

namespace fastmm::venues {

VenueKind venue_kind(std::string_view kind) noexcept {
  if (kind == "binance_spot" || kind == "binance" || kind == "sim") return VenueKind::BinanceSpot;
  if (kind == "bybit" || kind == "bybit_spot") return VenueKind::BybitSpot;
  if (kind == "deribit") return VenueKind::Deribit;
  if (kind == "binance_usdm") return VenueKind::BinanceUsdm;
  if (kind == "nasdaq_itch") return VenueKind::NasdaqItch;
  return VenueKind::Unknown;
}

std::unique_ptr<Venue> make_venue(VenueId id,
                                  const VenueSection& s,
                                  const VenueFactoryOptions& opts) {
  switch (venue_kind(s.kind)) {
    case VenueKind::BinanceSpot: {
      binance::VenueSectionView v;
      v.name = s.name;
      v.ws_url = s.ws_url;
      v.ws_api_url = s.ws_api_url;
      v.rest_url = s.rest_url;
      v.api_key = s.api_key;
      v.api_secret = s.api_secret;
      v.supports_replace = s.supports_replace;
      v.insecure_tls = s.insecure_tls;
      v.ca_file = s.ca_file;
      v.recv_window_ms = s.recv_window_ms;
      v.extra = &s.extra;
      binance::BinanceVenueConfig c = binance::make_binance_config(v, opts.dry_run);
      c.record_raw_dir = opts.record_raw_dir;
      return std::make_unique<binance::BinanceVenue>(id, std::move(c));
    }
    case VenueKind::BybitSpot: {
      bybit::BybitVenueConfig c = bybit::make_bybit_config(s, opts.dry_run);
      c.record_raw_dir = opts.record_raw_dir;
      return std::make_unique<bybit::BybitVenue>(id, std::move(c));
    }
    case VenueKind::Deribit: {
      deribit::DeribitVenueConfig c = deribit::make_deribit_config(s, opts.dry_run);
      c.record_raw_dir = opts.record_raw_dir;
      return std::make_unique<deribit::DeribitVenue>(id, std::move(c));
    }
    case VenueKind::BinanceUsdm: {
      binance_usdm::BinanceUsdmVenueConfig c =
          binance_usdm::make_binance_usdm_config(s, opts.dry_run);
      c.record_raw_dir = opts.record_raw_dir;
      return std::make_unique<binance_usdm::BinanceUsdmVenue>(id, std::move(c));
    }
    case VenueKind::NasdaqItch:
      return std::make_unique<nasdaq::NasdaqItchVenue>(
          id, nasdaq::make_nasdaq_itch_config(s, opts.dry_run, opts.busy_poll));
    case VenueKind::Unknown:
      break;
  }
  throw std::invalid_argument(
      "venue '" + s.name + "': unsupported kind '" + s.kind +
      "' (expected binance_spot, binance_usdm, sim, bybit, deribit or nasdaq_itch)");
}

}  // namespace fastmm::venues
